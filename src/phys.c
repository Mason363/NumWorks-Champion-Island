/* The physics world (see phys.h). Boxes never rotate, so contacts are along
 * the axes: penetrations are pushed apart along the axis of least overlap and
 * the approaching speed is cancelled (or bounced, with restitution), for a
 * few iterations per substep like cannon.js's solver. */
#include "phys.h"
_Static_assert(BODY_MAX < 128, "a contact is a body index in an int8_t");
#include <math.h>

Body bodies[BODY_MAX];

#define MAT_MAX 8
#define TYPE_MAX 8
#define CM_MAX 8
static char mat_names[MAT_MAX][16];
static int nmats;
typedef struct { char name[12]; uint8_t material; float mass, damping; uint16_t group, mask; } BodyType;
static BodyType types[TYPE_MAX];
static int ntypes;
typedef struct { uint8_t a, b; float friction, restitution; } ContactMat;
static ContactMat cms[CM_MAX];
static int ncms;
static float gravity_z = -400;
static int substeps = 4, iterations = 40;
static uint16_t ground_mask = 4;

int phys_material(const char *name) {
  for (int i = 0; i < nmats; i++)
    if (!strcmp(mat_names[i], name)) return i;
  if (nmats >= MAT_MAX) return 0;
  strncpy(mat_names[nmats], name, sizeof mat_names[0] - 1);
  return nmats++;
}

void phys_contact_material(const char *a, const char *b, float friction, float restitution) {
  int ma = phys_material(a), mb = phys_material(b);
  for (int i = 0; i < ncms; i++)
    if ((cms[i].a == ma && cms[i].b == mb) || (cms[i].a == mb && cms[i].b == ma)) {
      cms[i].friction = friction;
      cms[i].restitution = restitution;
      return;
    }
  if (ncms < CM_MAX) cms[ncms++] = (ContactMat){(uint8_t)ma, (uint8_t)mb, friction, restitution};
}

static const ContactMat *contact_of(int a, int b) {
  for (int i = 0; i < ncms; i++)
    if ((cms[i].a == a && cms[i].b == b) || (cms[i].a == b && cms[i].b == a)) return &cms[i];
  return NULL;
}

void phys_body_type(const char *type, const char *material, float mass, uint16_t group, uint16_t mask, float damping) {
  int t = phys_type(type);
  if (t < 0) {
    if (ntypes >= TYPE_MAX) return;
    t = ntypes++;
  }
  BodyType *bt = &types[t];
  strncpy(bt->name, type, sizeof bt->name - 1);
  bt->material = (uint8_t)phys_material(material);
  bt->mass = mass;
  bt->group = group;
  bt->mask = mask;
  bt->damping = damping;
  /* the ground collides with any type that collides with group 1 */
  if (mask & 1) ground_mask |= group;
}

int phys_type(const char *type) {
  for (int i = 0; i < ntypes; i++)
    if (!strcmp(types[i].name, type)) return i;
  return -1;
}

void phys_reset(void) {
  memset(bodies, 0, sizeof bodies);
  memset(types, 0, sizeof types);
  memset(mat_names, 0, sizeof mat_names);
  nmats = ntypes = ncms = 0;
  gravity_z = -400;
  substeps = 4;
  iterations = 40;
  ground_mask = 4;
  phys_material("groundMaterial");
  phys_material("bodyMaterial");
  phys_contact_material("groundMaterial", "bodyMaterial", 0, 0);
  phys_contact_material("bodyMaterial", "bodyMaterial", 0, 0);
  phys_body_type("character", "bodyMaterial", 10, 4, 7, .4f);
  phys_body_type("prop", "bodyMaterial", 0, 2, 4, .4f);
}

void phys_gravity(float gz) { gravity_z = gz; }
void phys_steps(int s, int it) { substeps = s; iterations = it; }

int phys_add(int type, uint8_t shape, float x, float y, float z, float hx, float hy, float hz) {
  if (type < 0) return -1;
  for (int i = 0; i < BODY_MAX; i++)
    if (!bodies[i].used) {
      Body *b = &bodies[i];
      memset(b, 0, sizeof *b);
      const BodyType *t = &types[type];
      b->used = b->active = true;
      b->type = (uint8_t)type;
      b->shape = shape;
      b->x = x; b->y = y; b->z = z;
      b->hx = hx; b->hy = hy; b->hz = hz;
      b->inv_mass = t->mass > 0 ? 1 / t->mass : 0;
      b->damping = t->damping;
      b->group = t->group;
      b->mask = t->mask;
      b->material = t->material;
      return i;
    }
  return -1;
}

void phys_remove(int i) {
  if (i >= 0 && i < BODY_MAX) bodies[i].used = false;
}

/* height of a ramp's slope at x (from its bottom) */
static float ramp_height(const Body *r, float x) {
  float t = (x - (r->x - r->hx)) / (2 * r->hx);
  t = t < 0 ? 0 : t > 1 ? 1 : t;
  if (r->shape == SH_RIGHT_RAMP) t = 1 - t;
  return 2 * r->hz * t;
}

static void add_contact(Body *b, int other) {
  for (int i = 0; i < b->ncontacts; i++)
    if (b->contacts[i] == other) return;
  if (b->ncontacts < 6) b->contacts[b->ncontacts++] = (int8_t)other;
}

/* resolve a dynamic body against another body (static or dynamic) */
static void resolve(int ia, int ib) {
  Body *a = &bodies[ia], *b = &bodies[ib];
  float dx = b->x - a->x, dy = b->y - a->y, dz = b->z - a->z;
  float ox = a->hx + b->hx - fabsf(dx), oy = a->hy + b->hy - fabsf(dy), oz = a->hz + b->hz - fabsf(dz);
  if (ox <= 0 || oy <= 0 || oz <= 0) return;
  float nx = 0, ny = 0, nz = 0, pen;
  if (b->shape != SH_BOX) {
    /* a ramp: the top is a slope, the high end a wall, the low end open */
    float top = b->z - b->hz + ramp_height(b, a->x);
    float bottom = a->z - a->hz;
    float over = top - bottom;
    bool high_side = b->shape == SH_LEFT_RAMP ? a->x > b->x + b->hx : a->x < b->x - b->hx;
    if (over > 0 && (over < 12 || !high_side) && a->vz <= 0.5f * fabsf(a->vx) + 60) {
      nz = -1; pen = over;
    } else if (over <= 0) return;
    else if (ox < oy) { nx = dx > 0 ? 1 : -1; pen = ox; }
    else { ny = dy > 0 ? 1 : -1; pen = oy; }
  } else if (ox <= oy && ox <= oz) { nx = dx > 0 ? 1 : -1; pen = ox; }
  else if (oy <= oz) { ny = dy > 0 ? 1 : -1; pen = oy; }
  else { nz = dz > 0 ? 1 : -1; pen = oz; }
  /* n points from a to b */
  float wa = a->inv_mass, wb = b->inv_mass, w = wa + wb;
  if (w <= 0) return;
  a->x -= nx * pen * wa / w; a->y -= ny * pen * wa / w; a->z -= nz * pen * wa / w;
  b->x += nx * pen * wb / w; b->y += ny * pen * wb / w; b->z += nz * pen * wb / w;
  float rv = (b->vx - a->vx) * nx + (b->vy - a->vy) * ny + (b->vz - a->vz) * nz;
  const ContactMat *cm = contact_of(a->material, b->material);
  float e = cm ? cm->restitution : 0, mu = cm ? cm->friction : 0.3f;
  if (rv < 0) {
    float j = -(1 + e) * rv / w;
    a->vx -= j * nx * wa; a->vy -= j * ny * wa; a->vz -= j * nz * wa;
    b->vx += j * nx * wb; b->vy += j * ny * wb; b->vz += j * nz * wb;
    if (mu > 0) {
      /* friction against the sliding speed */
      float tx = (b->vx - a->vx) - rv * nx, ty = (b->vy - a->vy) - rv * ny, tz = (b->vz - a->vz) - rv * nz;
      float tl = sqrtf(tx * tx + ty * ty + tz * tz);
      if (tl > 1e-6f) {
        float jt = fminf(mu * j, tl / w);
        tx /= tl; ty /= tl; tz /= tl;
        a->vx += jt * tx * wa; a->vy += jt * ty * wa; a->vz += jt * tz * wa;
        b->vx -= jt * tx * wb; b->vy -= jt * ty * wb; b->vz -= jt * tz * wb;
      }
    }
  }
  add_contact(a, ib);
  add_contact(b, ia);
}

void phys_step(float dt) {
  float h = dt / substeps;
  for (int i = 0; i < BODY_MAX; i++) bodies[i].ncontacts = 0;
  for (int s = 0; s < substeps; s++) {
    for (int i = 0; i < BODY_MAX; i++) {
      Body *b = &bodies[i];
      if (!b->used || !b->active || b->inv_mass <= 0) continue;
      b->vz += gravity_z * h;
      float damp = powf(1 - b->damping, h);
      b->vx *= damp; b->vy *= damp; b->vz *= damp;
      b->x += b->vx * h; b->y += b->vy * h; b->z += b->vz * h;
    }
    int iters = iterations > 8 ? 8 : iterations;    /* boxes settle in a few passes */
    for (int it = 0; it < iters; it++) {
      for (int i = 0; i < BODY_MAX; i++) {
        Body *a = &bodies[i];
        if (!a->used || !a->active || a->inv_mass <= 0) continue;
        /* the ground plane */
        if ((ground_mask & a->group) && a->z - a->hz < 0) {
          a->z = a->hz;
          if (a->vz < 0) {
            const ContactMat *cm = contact_of(0, a->material);
            a->vz = cm ? -a->vz * cm->restitution : 0;
          }
          add_contact(a, -1);
        }
        for (int j = 0; j < BODY_MAX; j++) {
          Body *b = &bodies[j];
          if (j == i || !b->used || !b->active) continue;
          if (!(a->mask & b->group) || !(b->mask & a->group)) continue;
          if (b->inv_mass > 0 && j < i) continue;   /* dynamic pairs once */
          resolve(i, j);
        }
      }
    }
  }
}

float phys_ground(float x, float y, float from_z) {
  float g = 0;
  for (int i = 0; i < BODY_MAX; i++) {
    const Body *b = &bodies[i];
    if (!b->used || !b->active || b->inv_mass > 0 || !(b->group & 3)) continue;
    if (x < b->x - b->hx || x > b->x + b->hx || y < b->y - b->hy || y > b->y + b->hy) continue;
    float top = b->shape == SH_BOX ? b->z + b->hz : b->z - b->hz + ramp_height(b, x);
    if (top <= from_z && top > g) g = top;
  }
  return g;
}

bool phys_touching(int a, int b) {
  if (a < 0 || a >= BODY_MAX) return false;
  for (int i = 0; i < bodies[a].ncontacts; i++)
    if (bodies[a].contacts[i] == b) return true;
  return false;
}
