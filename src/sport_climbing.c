/* Climbing: the doodle's scene Fq (library 7B2C344CB74B48B4AFAEBCF1D033F55A).
 *
 * Lucky climbs a cliff to the owl champion before the 90 s clock runs out:
 * walk on ledges, jump (OK) from hold to hold, some holds move, circle or
 * crumble, snowballs fall from above and knock her back to the last lantern.
 * Systems, in the doodle's order: Ep (pause), Uo (rules), Pp (draw order),
 * pq (altitude meter), oq (clock), vq (obstacles and holds), yq (the camera's
 * intro), qq (the climber), aq (sprite directions), fq (velocity), Cq/Dq
 * (camera), Up (off-screen snowballs), Aq (parallax), zq (the end); Wo (the
 * countdown) joins after the intro. Variants: "" / "default" and "hard". */
#include <math.h>
#include <stdio.h>
#include "ent.h"
#include "spr.h"

enum { M_GROUND, M_JUMP, M_GRAB, M_HANG, M_FALL };
enum { H_STATIC, H_MOVING, H_CIRCLING };
#define HOLD_MAX 50
#define GROUND_MAX 16
#define CHECK_MAX 10
#define NEVER 1e30f   /* Number.MAX_SAFE_INTEGER */
#define SKY_MAX 8
#define BG_MAX 768

typedef struct {
  NodeId n;
  uint8_t kind;
  bool falling, based, T;   /* T: crumbling ($T) */
  bool clockwise;
  float speed, nx, ny, dist, radius;
  float bx, by, phase;      /* jS, hJ */
  int16_t CT, W, tw;        /* crumble timer, time until it is back, fall tween step */
  float PL;                 /* its y before the fall */
} Hold;

typedef struct { NodeId n; Rect r; } Area;
typedef struct { NodeId n, lantern; Rect r; float px, py; bool mY; float delay; } Check;

typedef struct {
  NodeId root, map, climber, target, meter_text, clock_text, meter, clock, iba, bg;
  /* the cliff's tiles: where their keys are, their boxes */
  uint16_t bg_off[BG_MAX];
  uint8_t bg_box[BG_MAX][4];
  int nbg;
  uint32_t bg_ticks;
  Hold holds[HOLD_MAX];
  int nholds;
  Area grounds[GROUND_MAX];
  int ngrounds;
  Rect goal;
  bool has_goal;
  Check checks[CHECK_MAX];
  int nchecks;
  /* the climber (Dg) */
  uint8_t mode;
  bool zaa, moving;
  int zY, d7, b7;
  int DY, kC, D_;           /* hold, checkpoint, ground: indices or -1 */
  float EL;
  /* altitude meter (ug) and clock (Fg) */
  bool meter_init;
  float v_, PL, KY;
  int altitude;
  /* systems switched on and off like the doodle's */
  bool qq_on, oq_on, pq_on, vq_on, yq_on;
  /* the camera: yq's state, the intro tween, Dq's goal */
  int cam_state, tween_t;
  float t0x, t0y, t1x, t1y, qx, qy;
  /* parallax (Aq) */
  bool par_init;
  float par_dx, par_dy, par_hx, par_hy, par_fx, par_fy;
  /* the sky (Iba's copies of one big picture, drawn by code) */
  uint16_t sky_sprite;
  int nsky;
  float sky_x[SKY_MAX], sky_y[SKY_MAX];
  float sky_ref;               /* the copy whose rows the forest texture matches */
  bool tex, tex_on;            /* the forest texture is ready, shown */
  uint8_t *tex_mem;
  uint8_t tex_sheet;
  int tex_cx, tex_cy;
  Countdown cd;
  bool counting;
  /* the end (zq) */
  bool ended;
  int end_t, score, rating;
  char meter_buf[32], clock_buf[16];
} State;

static State *S;

/* ---------------------------------------------------------------- helpers */
static bool has(NodeId n, int c) { return n && nodes[n].T != NONE16 && comp_has(nodes[n].T, c); }

/* createjs Rectangle.contains: edges included */
static bool inside(Rect r, float x, float y) { return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h; }

/* zj: the raw x, y of a node */
static float rawx(NodeId n) { return nodes[n].x; }
static float rawy(NodeId n) { return nodes[n].y; }

static void set_raw(NodeId n, float x, float y) {
  nodes[n].x = x;
  nodes[n].y = y;
  ent_moved(n);
}

/* mq(p, [groundBounds]) */
static int ground_at(float x, float y) {
  for (int i = 0; i < S->ngrounds; i++)
    if (inside(S->grounds[i].r, x, y)) return i;
  return -1;
}

/* kq: metres climbed at height y */
static int altitude_of(float y) {
  float b = fmaxf(S->v_, fminf(S->PL, y));
  return (int)-floorf((b - S->PL) / 16);
}

/* rq: the climber's pose, and its sprite from the start if asked */
static void climber_anim(const char *label, bool restart) {
  NodeId g = S->climber;
  ent_label(g, label, false);
  if (!restart) return;
  for (NodeId c = nodes[g].first; c; c = nodes[c].next)
    if ((nodes[c].flags & NF_ONSTAGE) && has(c, C_sprite)) { node_goto(c, NULL, 0, true); break; }
}

static Ent *cl_vel(void) { return ent_get(S->climber); }

/* sq: jump */
static void jump(void) {
  S->zY = 0;
  S->zaa = true;
  S->mode = M_JUMP;
  climber_anim("jumpHold", true);
  SOUND(nza);
}

/* uq: hang on a hold (a crumbled one lets go) */
static void hang_on(int h) {
  if (h < 0) return;
  Hold *o = &S->holds[h];
  if (o->falling) {
    if (o->T && o->CT <= 0) { S->mode = M_JUMP; return; }
    if (!o->T) { o->CT = 60; o->T = true; }
  }
  ent_set_pos(S->climber, rawx(o->n), rawy(o->n) + 27);
  S->DY = h;
}

/* ---------------------------------------------------------------- the map's
 * timeline children that are only drawn from the data (bitmaps, static clips)
 * become nodes, so that sorting the map by draw order keeps them */
static const uint8_t *varint(const uint8_t *p, unsigned *v) {
  unsigned r = 0, s = 0;
  for (int i = 0; i < 5; i++) {
    uint8_t b = *p++;
    r |= (unsigned)(b & 0x7F) << s;
    if (!(b & 0x80)) break;
    s += 7;
  }
  *v = r;
  return p;
}

typedef struct { unsigned start; uint8_t flags, kind; uint16_t ref, mat, name; float x, y; int16_t rx4, ry4; uint8_t alpha; } TKey;

static const uint8_t *tkey(const uint8_t *p, TKey *k) {
  p = varint(p, &k->start);
  k->flags = *p++;
  if (k->flags & K_ABSENT) return p;
  if (k->flags & K_KIND) { k->kind = *p++; k->ref = rd16(p); p += 2; }
  else { k->kind = CK_SYM; k->ref = rd16(p); p += 2; }
  k->x = rds16(p) * 0.25f;
  k->y = rds16(p + 2) * 0.25f;
  p += 4;
  k->mat = 0;
  if (k->flags & K_MAT) { k->mat = rd16(p); p += 2; }
  k->rx4 = k->ry4 = 0;
  if (k->flags & K_REG) { k->rx4 = rds16(p); k->ry4 = rds16(p + 2); p += 4; }
  k->alpha = 255;
  if (k->flags & K_ALPHA) k->alpha = *p++;
  if (k->flags & K_CLIP) p += 4;
  k->name = NONE16;
  if (k->flags & K_NAME) { k->name = rd16(p); p += 2; }
  return p;
}

/* createjs getBounds() of a symbol at a frame: its nominal bounds, or the
 * union of what its timeline shows (the library has no nominal bounds) */
static bool sym_bounds(uint16_t sym, unsigned frame, int depth, float *bx, float *by, float *bw, float *bh) {
  SymInfo si;
  if (sym >= SYM_COUNT) return false;
  sym_info(sym, &si);
  if (si.type == SYM_BITMAP) {
    Sprite sp;
    sprite_info((uint16_t)si.v, &sp);
    *bx = 0; *by = 0; *bw = sp.w; *bh = sp.h;
    return true;
  }
  Clip c;
  if (si.type != SYM_CLIP || depth > 3 || !clip_get(sym, &c)) return false;
  if (c.nb[2] || c.nb[3]) { *bx = c.nb[0]; *by = c.nb[1]; *bw = c.nb[2]; *bh = c.nb[3]; return true; }
  float lx = 1e9f, ly = 1e9f, hx = -1e9f, hy = -1e9f;
  const uint8_t *p = c.slots;
  for (unsigned s = 0; s < c.nslots; s++) {
    unsigned nk;
    p = varint(p, &nk);
    TKey cur = {0}, k;
    int idx = -1;
    for (unsigned i = 0; i < nk; i++) {
      p = tkey(p, &k);
      if (k.start <= frame) { cur = k; idx = (int)i; }
    }
    float x, y, w, h;
    if (idx < 0 || (cur.flags & K_ABSENT) || cur.kind != CK_SYM || !sym_bounds(cur.ref, 0, depth + 1, &x, &y, &w, &h)) continue;
    float a = 1, b = 0, cc = 0, d = 1;
    if (cur.mat) { const float *m = mat(cur.mat); a = m[0]; b = m[1]; cc = m[2]; d = m[3]; }
    float tx = cur.x - (cur.rx4 * .25f * a + cur.ry4 * .25f * cc), ty = cur.y - (cur.rx4 * .25f * b + cur.ry4 * .25f * d);
    float xs[4] = {x, x + w, x, x + w}, ys[4] = {y, y, y + h, y + h};
    for (int i = 0; i < 4; i++) {
      float X = a * xs[i] + cc * ys[i] + tx, Y = b * xs[i] + d * ys[i] + ty;
      lx = fminf(lx, X); hx = fmaxf(hx, X); ly = fminf(ly, Y); hy = fmaxf(hy, Y);
    }
  }
  if (lx > hx) return false;
  *bx = lx; *by = ly; *bw = hx - lx; *bh = hy - ly;
  return true;
}

/* Oj(e, map): the transformed bounds of e's "bounds" child, taken to the map's
 * space by two corners. The child is often drawn from the data (no node). */
static bool box_of(NodeId e, Rect *r) {
  uint16_t sym = NONE16;
  unsigned frame = 0;
  Mat m = MAT_ID;
  for (NodeId c = nodes[e].first; c; c = nodes[c].next)
    if ((nodes[c].flags & NF_ONSTAGE) && has(c, C_bounds)) { sym = nodes[c].sym; frame = nodes[c].frame; m = node_local(c); break; }
  Clip cl;
  if (sym == NONE16 && clip_get(nodes[e].sym, &cl)) {
    const uint8_t *p = cl.slots;
    for (unsigned s = 0; s < cl.nslots && sym == NONE16; s++) {
      unsigned nk;
      p = varint(p, &nk);
      TKey cur = {0}, k;
      int idx = -1;
      for (unsigned i = 0; i < nk; i++) {
        p = tkey(p, &k);
        if (k.start <= nodes[e].frame) { cur = k; idx = (int)i; }
      }
      Clip b;
      if (idx < 0 || (cur.flags & K_ABSENT) || cur.kind != CK_SYM || cur.ref >= SYM_COUNT) continue;
      if (!clip_get(cur.ref, &b) || b.T == NONE16 || !comp_has(b.T, C_bounds)) continue;
      float a = 1, bb = 0, cc = 0, d = 1;
      if (cur.mat) { const float *mm = mat(cur.mat); a = mm[0]; bb = mm[1]; cc = mm[2]; d = mm[3]; }
      m = (Mat){a, bb, cc, d, cur.x - (cur.rx4 * .25f * a + cur.ry4 * .25f * cc), cur.y - (cur.rx4 * .25f * bb + cur.ry4 * .25f * d)};
      sym = cur.ref;
    }
  }
  float x, y, w, h;
  if (sym == NONE16 || !sym_bounds(sym, frame, 0, &x, &y, &w, &h)) return false;
  /* getTransformedBounds: in e's space */
  float xs[4] = {x, x + w, x, x + w}, ys[4] = {y, y, y + h, y + h};
  float lx = 1e9f, ly = 1e9f, hx = -1e9f, hy = -1e9f;
  for (int i = 0; i < 4; i++) {
    float X = m.a * xs[i] + m.c * ys[i] + m.tx, Y = m.b * xs[i] + m.d * ys[i] + m.ty;
    lx = fminf(lx, X); hx = fmaxf(hx, X); ly = fminf(ly, Y); hy = fmaxf(hy, Y);
  }
  x = lx; y = ly; w = hx - lx; h = hy - ly;
  /* its two corners in the map's space */
  Mat t = node_to(e, S->map);
  float x0 = t.a * x + t.c * y + t.tx, y0 = t.b * x + t.d * y + t.ty;
  float x1 = t.a * (x + w) + t.c * (y + h) + t.tx, y1 = t.b * (x + w) + t.d * (y + h) + t.ty;
  r->x = fminf(x0, x1);
  r->y = fminf(y0, y1);
  r->w = fabsf(x1 - x0);
  r->h = fabsf(y1 - y0);
  return true;
}

static void materialize(NodeId m) {
  Clip c;
  if (!clip_get(nodes[m].sym, &c)) return;
  const uint8_t *p = c.slots;
  NodeId prev = 0, ch = nodes[m].first;
  for (unsigned s = 0; s < c.nslots; s++) {
    while (ch && nodes[ch].slot != NONE16 && nodes[ch].slot < s) { prev = ch; ch = nodes[ch].next; }
    unsigned nk;
    p = varint(p, &nk);
    TKey cur = {0}, k;
    int idx = -1;
    for (unsigned i = 0; i < nk; i++) {
      p = tkey(p, &k);
      if (k.start <= nodes[m].frame) { cur = k; idx = (int)i; }
    }
    if (idx < 0 || (cur.flags & K_ABSENT) || (ch && nodes[ch].slot == s)) continue;
    NodeId n = 0;
    if (cur.kind == CK_SYM) n = node_new_sym(cur.ref);
    else if (cur.kind == CK_SHAPE) { n = node_new(NK_SHAPE); if (n) nodes[n].ref = cur.ref; }
    if (!n) continue;
    Node *q = &nodes[n];
    q->x = cur.x; q->y = cur.y; q->rx4 = cur.rx4; q->ry4 = cur.ry4; q->mat = cur.mat; q->alpha = cur.alpha;
    q->name = cur.name;
    if (cur.flags & K_HIDDEN) q->flags &= (uint8_t)~NF_VISIBLE;
    q->parent = m;
    q->slot = (uint16_t)s;
    q->key = (uint16_t)idx;
    q->next = ch;
    if (prev) nodes[prev].next = n; else nodes[m].first = n;
    prev = n;
  }
}

/* ---------------------------------------------------------------- the cliff
 * The cliff behind everything (f1 / Jba: 400 to 740 tiles, some of them the
 * sea's animated water) is drawn by code from a table of where each tile is,
 * so a frame only reads the tiles in view, and it needs no nodes: the water
 * tiles show the frame all of them are at (they started together). */
#define BG_UNIT 16
#define BG_ORG (-512)

static void bg_hook(NodeId n, Mat m, uint8_t al) {
  Clip c;
  if (n != S->bg || !clip_get(nodes[n].sym, &c)) return;
  float det = m.a * m.d - m.b * m.c;
  if (fabsf(det) < 1e-9f) return;
  /* the view in the cliff's space, in table units */
  float lx = 1e9f, ly = 1e9f, hx = -1e9f, hy = -1e9f;
  for (int i = 0; i < 4; i++) {
    float X = (i & 1) ? VIEW_W : 0, Y = (i & 2) ? VIEW_H : 0, dx = X - m.tx, dy = Y - m.ty;
    float u = (m.d * dx - m.c * dy) / det, v = (-m.b * dx + m.a * dy) / det;
    lx = fminf(lx, u); hx = fmaxf(hx, u); ly = fminf(ly, v); hy = fmaxf(hy, v);
  }
  int x0 = (int)floorf((lx - BG_ORG) / BG_UNIT), x1 = (int)floorf((hx - BG_ORG) / BG_UNIT);
  int y0 = (int)floorf((ly - BG_ORG) / BG_UNIT), y1 = (int)floorf((hy - BG_ORG) / BG_UNIT);
  for (int i = 0; i < S->nbg; i++) {
    const uint8_t *b = S->bg_box[i];
    if (b[0] > x1 || b[2] < x0 || b[1] > y1 || b[3] < y0) continue;
    unsigned nk;
    const uint8_t *p = varint(c.slots + S->bg_off[i], &nk);
    TKey cur = {0}, k;
    int idx = -1;
    for (unsigned j = 0; j < nk; j++) {
      p = tkey(p, &k);
      if (k.start <= nodes[n].frame) { cur = k; idx = (int)j; }
    }
    if (idx < 0 || (cur.flags & (K_ABSENT | K_HIDDEN)) || !cur.alpha || cur.kind != CK_SYM) continue;
    float a = 1, bb = 0, cc = 0, d = 1;
    if (cur.mat) { const float *mm = mat(cur.mat); a = mm[0]; bb = mm[1]; cc = mm[2]; d = mm[3]; }
    Mat km = mat_mul(m, (Mat){a, bb, cc, d, cur.x - (cur.rx4 * .25f * a + cur.ry4 * .25f * cc), cur.y - (cur.rx4 * .25f * bb + cur.ry4 * .25f * d)});
    node_draw_sym(cur.ref, S->bg_ticks, km, (uint8_t)((al * cur.alpha + 127) / 255));
  }
}

static uint8_t bg_unit(float v) {
  int u = (int)floorf((v - BG_ORG) / BG_UNIT);
  return (uint8_t)(u < 0 ? 0 : u > 255 ? 255 : u);
}

/* the map's biggest clip child becomes an empty (lazy) node drawn by bg_hook */
static void bg_setup_cliff(void) {
  NodeId old = 0;
  unsigned most = 0;
  Clip c;
  for (NodeId ch = nodes[S->map].first; ch; ch = nodes[ch].next)
    if (nodes[ch].kind == NK_CLIP && clip_get(nodes[ch].sym, &c) && c.nslots > most) { most = c.nslots; old = ch; }
  if (!old || most < 64 || !clip_get(nodes[old].sym, &c)) return;
  NodeId bg = node_new_sym_lazy(nodes[old].sym);
  if (!bg) return;
  Node *o = &nodes[old], *q = &nodes[bg];
  q->x = o->x; q->y = o->y; q->rx4 = o->rx4; q->ry4 = o->ry4; q->mat = o->mat; q->alpha = o->alpha;
  q->flags = o->flags; q->name = o->name; q->key = o->key; q->frame = o->frame;
  uint16_t slot = o->slot;
  node_add_at(S->map, bg, old);
  node_free(old);
  nodes[bg].slot = slot;
  S->bg = bg;
  const uint8_t *p = c.slots;
  for (unsigned s = 0; s < c.nslots && S->nbg < BG_MAX; s++) {
    uint16_t off = (uint16_t)(p - c.slots);
    unsigned nk;
    p = varint(p, &nk);
    TKey cur = {0}, k;
    int idx = -1;
    for (unsigned j = 0; j < nk; j++) {
      p = tkey(p, &k);
      if (k.start <= nodes[bg].frame) { cur = k; idx = (int)j; }
    }
    float x, y, w, h;
    if (idx < 0 || (cur.flags & K_ABSENT) || cur.kind != CK_SYM || !sym_bounds(cur.ref, 0, 0, &x, &y, &w, &h)) continue;
    float a = 1, bb = 0, cc = 0, d = 1;
    if (cur.mat) { const float *mm = mat(cur.mat); a = mm[0]; bb = mm[1]; cc = mm[2]; d = mm[3]; }
    float tx = cur.x - (cur.rx4 * .25f * a + cur.ry4 * .25f * cc), ty = cur.y - (cur.rx4 * .25f * bb + cur.ry4 * .25f * d);
    float xs[4] = {x, x + w, x, x + w}, ys[4] = {y, y, y + h, y + h};
    float lx = 1e9f, ly = 1e9f, hx = -1e9f, hy = -1e9f;
    for (int i = 0; i < 4; i++) {
      float X = a * xs[i] + cc * ys[i] + tx, Y = bb * xs[i] + d * ys[i] + ty;
      lx = fminf(lx, X); hx = fmaxf(hx, X); ly = fminf(ly, Y); hy = fmaxf(hy, Y);
    }
    uint8_t *b = S->bg_box[S->nbg];
    b[0] = bg_unit(lx); b[1] = bg_unit(ly); b[2] = bg_unit(hx); b[3] = bg_unit(hy);
    S->bg_off[S->nbg++] = off;
  }
  node_draw_hook_id = bg;
  node_draw_hook = bg_hook;
}

static void end(void) {
  node_draw_hook = NULL;
  node_draw_hook_id = 0;
}

/* ---------------------------------------------------------------- the sky
 * Iba (the parallax backdrop) holds copies of one 432 x 1007 picture (sky,
 * sea, then forest), stacked with overlaps, the last one on top. The picture
 * is a single LZMA stream: drawing a row decodes every row above it, and the
 * forest's rows are dear (some 0.13 ms each on the calculator). So the sky is
 * drawn here: the copy that shows, as a sprite, while its rows are cheap (the
 * sky and the sea); lower down, a texture of the picture's forest decoded
 * once (the background layer's "water"), under the sea's colour where the
 * sea still shows. */
#define TEX_W 384        /* the picture's columns 32..415: all the parallax ever shows */
#define TEX_X0 32
#define TEX_H 128
#define TEX_ROW0 879     /* its rows 879..1006 */
#define SKY_CHEAP 400    /* the deepest row the sprite may be drawn down to */
#define SEA_END 520      /* the sea's last row, about */

static bool sky_slot(unsigned slot, const SlotInfo *si, void *ctx) {
  (void)slot; (void)ctx;
  SymInfo t;
  sym_info(si->sym, &t);
  if (t.type != SYM_BITMAP || S->nsky >= SKY_MAX) return true;
  if (S->nsky && (uint16_t)t.v != S->sky_sprite) return true;
  S->sky_sprite = (uint16_t)t.v;
  S->sky_x[S->nsky] = si->x;
  S->sky_y[S->nsky] = si->y;
  S->nsky++;
  return true;
}

static void sky_paint(int x0, int y0, int w, int h) { (void)x0; (void)y0; (void)w; (void)h; }

static void sky_setup(void) {
  if (!S->iba) return;
  clip_each_slot(nodes[S->iba].sym, nodes[S->iba].frame, sky_slot, NULL);
  if (!S->nsky) return;
  node_set_visible(S->iba, false);
  S->sky_ref = S->sky_y[S->nsky - 1];
  Sprite sp;
  sprite_info(S->sky_sprite, &sp);
  const uint8_t *st = spr_stream(S->sky_sprite);
  if (!st || sp.w < TEX_X0 + TEX_W || sp.h < TEX_ROW0 + TEX_H) return;
  /* the layer's memory: one clear pixel (the layer), then the texture */
  mem_layout(TEX_W, TEX_H + 1, sp.sheet, sky_paint);
  int w, h;
  uint8_t *mem = bg_layer(&w, &h);
  if (!mem || w != TEX_W || h != TEX_H + 1) { bg_off(); return; }
  uint8_t *tex = mem + TEX_W;
  z_open(rd32(st + 2), rd32(st + 6), rd32(st + 10));
  bool ok = z_get(NULL, (uint32_t)TEX_ROW0 * sp.w);
  for (int r = 0; ok && r < TEX_H; r++)
    ok = z_get(NULL, TEX_X0) && z_get(tex + r * TEX_W, TEX_W) && z_get(NULL, (uint32_t)(sp.w - TEX_X0 - TEX_W));
  if (!ok) { bg_off(); return; }
  S->tex = true;
  S->tex_mem = mem;
  S->tex_sheet = sp.sheet;
  bg_off();
}

/* the texture under everything, or nothing (the sky sprite covers the view) */
static void texture_on(bool on) {
  if (!S->tex || on == S->tex_on) return;
  S->tex_on = on;
  if (on) {
    bg_setup(S->tex_mem, 1, 1, S->tex_sheet, sky_paint);
    bg_water(S->tex_mem + TEX_W, TEX_W, TEX_H);
    S->tex_cx = S->tex_cy = 0x7fffffff;
  } else bg_off();
}

/* the copy on top at row l of Iba, -1 if none */
static int sky_top(float l) {
  Sprite sp;
  sprite_info(S->sky_sprite, &sp);
  for (int k = S->nsky - 1; k >= 0; k--)
    if (l >= S->sky_y[k] && l < S->sky_y[k] + sp.h) return k;
  return -1;
}

static void draw_under(void) {
  if (!S || !S->iba || !S->nsky) return;
  Mat M = mat_mul((Mat){1.0f / 3, 0, 0, 1.0f / 3, 0, 0}, node_global(S->iba));
  if (M.d < 1e-3f || M.a < 1e-3f) return;
  float v0 = -M.ty / M.d, v1 = v0 + VIEW_H / M.d;
  int k0 = sky_top(v0), k1 = sky_top(v1 - 1);
  if (k0 >= 0 && k0 == k1 && v1 - S->sky_y[k0] <= SKY_CHEAP) {
    Mat t = M;
    t.tx += M.a * S->sky_x[k0] + M.c * S->sky_y[k0];
    t.ty += M.b * S->sky_x[k0] + M.d * S->sky_y[k0];
    texture_on(false);
    gfx_sprite(S->sky_sprite, t, 255);
    return;
  }
  texture_on(true);
  if (!S->tex) return;
  /* the texture: the picture's columns from TEX_X0, the rows of copy sky_ref from TEX_ROW0 */
  int x0 = (int)floorf(M.tx + M.a * S->sky_x[0] + .5f), y0 = (int)floorf(M.ty + M.d * S->sky_ref + .5f);
  int cx = -x0 + TEX_X0, cy = -y0 + TEX_ROW0;
  if (cx != S->tex_cx || cy != S->tex_cy) {
    bg_camera(cx, cy);
    S->tex_cx = cx;
    S->tex_cy = cy;
  }
  /* where the sea still shows */
  if (k0 >= 0 && v0 - S->sky_y[k0] < SEA_END) {
    int ye = (int)floorf(M.ty + M.d * (S->sky_y[k0] + SEA_END) + .5f);
    if (ye > 0) gfx_rect(0, 0, VIEW_W, ye < VIEW_H ? ye : VIEW_H, rgb565(52, 212, 180), 255);
  }
}

/* ---------------------------------------------------------------- start */
static void start(void) {
  S = scene_state(sizeof(State));
  int frame = !strcmp(game.variant, "hard") ? 2 : !strcmp(game.variant, "notut") ? 1 : 0;
  NodeId root = node_new_sym_frame(S_climbing_wT, frame);
  if (!root) return;
  node_add(game.root, root);
  S->root = root;
  /* the variant's map, whole (the root was made with just what its frame
   * shows): the partial one goes first, the nodes have room for one map */
  for (NodeId c = nodes[root].first; c; c = nodes[c].next) {
    if (!has(c, C_map) || !(nodes[c].flags & NF_ONSTAGE)) continue;
    Node o = nodes[c];
    NodeId after = o.next;
    node_free(c);
    NodeId m = node_new_sym(o.sym);
    if (!m) break;
    Node *q = &nodes[m];
    q->x = o.x; q->y = o.y; q->rx4 = o.rx4; q->ry4 = o.ry4; q->mat = o.mat; q->alpha = o.alpha;
    q->flags = o.flags; q->flags2 = o.flags2 & (uint8_t)~NF2_PARTIAL;   /* whole: no children to make later */
    q->name = o.name; q->key = o.key;
    node_add_at(root, m, after);
    nodes[m].slot = o.slot;
    S->map = m;
    break;
  }
  if (!S->map) return;
  bg_setup_cliff();
  materialize(S->map);
  ent_register_tree(root);
  S->iba = ent_first(C_parallax);
  sky_setup();
  S->meter = ent_first(C_altitudeMeter);
  S->clock = ent_first(C_clock);
  S->climber = ent_first(C_climber);
  S->target = ent_first(C_cameraTarget);
  if (S->meter) S->meter_text = node_find(S->meter, "textfield");
  if (S->clock) S->clock_text = node_find(S->clock, "textfield");
  S->KY = S->clock ? comp_float(nodes[S->clock].T, C_clock, F_seconds, 0) : 0;
  /* the map's entities, in draw order (Pp sorts by y) */
  sys_sort_draw();
  for (NodeId c = nodes[S->map].first; c; c = nodes[c].next) {
    if (!nodes[c].ent) continue;
    uint16_t T = nodes[c].T;
    if (comp_has(T, C_climbingHold) && S->nholds < HOLD_MAX) {
      Hold *h = &S->holds[S->nholds++];
      h->n = c;
      nodes[c].flags2 |= NF2_KEEP;
      if (comp_has(T, C_movingClimbingHold)) {
        h->kind = H_MOVING;
        h->speed = comp_float(T, C_movingClimbingHold, F_speed, 1);
        h->nx = 1; h->ny = 0;
        comp_vec(T, C_movingClimbingHold, F_directionNormal, &h->nx, &h->ny);
        h->dist = comp_float(T, C_movingClimbingHold, F_distance, 100);
      } else if (comp_has(T, C_circlingClimbingHold)) {
        h->kind = H_CIRCLING;
        h->speed = comp_float(T, C_circlingClimbingHold, F_speed, .5f);
        h->clockwise = comp_bool(T, C_circlingClimbingHold, F_clockwise, false);
        h->radius = comp_float(T, C_circlingClimbingHold, F_radius, 25);
      }
      h->falling = comp_has(T, C_fallingClimbingHold);
    }
    Rect r;
    if (!comp_has(T, C_boundable) || !box_of(c, &r)) continue;
    if (comp_has(T, C_groundBounds) && S->ngrounds < GROUND_MAX) S->grounds[S->ngrounds++] = (Area){c, r};
    if (comp_has(T, C_goalBounds) && !S->has_goal) { S->goal = r; S->has_goal = true; }
    if (comp_has(T, C_checkpoint) && S->nchecks < CHECK_MAX) {
      Check *k = &S->checks[S->nchecks++];
      k->n = c;
      k->r = r;
      ent_pos(c, &k->px, &k->py);
      k->lantern = node_child(c, "lantern");
    }
  }
  S->mode = M_GROUND;
  S->DY = S->kC = S->D_ = -1;
  S->EL = NEVER;
  S->qq_on = S->oq_on = S->pq_on = S->vq_on = S->yq_on = true;
}

/* ---------------------------------------------------------------- systems */
/* pq: the altitude meter */
static void sys_meter(void) {
  if (!S->pq_on || !S->climber) return;
  if (!S->meter_init) {
    S->meter_init = true;
    S->v_ = S->goal.y + S->goal.h;
    S->PL = rawy(S->climber) - 16;
  }
  S->altitude = altitude_of(rawy(S->climber));
  snprintf(S->meter_buf, sizeof S->meter_buf, "%d m / %d m", S->altitude, altitude_of(S->v_));
  if (S->meter_text) node_set_text(S->meter_text, S->meter_buf);
}

/* oq: the clock */
static void sys_clock(void) {
  if (!S->oq_on) return;
  S->KY -= 1.0f / 30;
  int m = (int)fmaxf(0, floorf(S->KY / 60));
  int g = (int)fmaxf(0, floorf(S->KY) - 60 * m);
  snprintf(S->clock_buf, sizeof S->clock_buf, "%d:%s%d", m, g < 10 ? "0" : "", g);
  if (S->clock_text) node_set_text(S->clock_text, S->clock_buf);
}

/* the crumbled holds' fall (a tween of 30 ticks, powIn(2.2)) */
static void tweens(void) {
  for (int i = 0; i < S->nholds; i++) {
    Hold *h = &S->holds[i];
    if (h->tw <= 0 || h->tw > 30) continue;
    float e = powf(h->tw / 30.0f, 2.2f);
    nodes[h->n].alpha = (uint8_t)(255 * (1 - e) + .5f);
    set_raw(h->n, rawx(h->n), h->PL + 180 * e);
    h->tw = h->tw >= 30 ? 0 : (int16_t)(h->tw + 1);
  }
}

/* vq: holds (wq, xq), snowballs hitting the climber, new snowballs */
static void sys_obstacles(void) {
  if (!S->vq_on) return;
  for (int i = 0; i < S->nholds; i++) {
    Hold *h = &S->holds[i];
    if (!h->falling || !h->T) continue;
    if (h->CT > 0) {
      if (--h->CT == 0) {
        h->PL = rawy(h->n);
        h->tw = 1;
        h->W = 120;
      }
    } else if (h->W > 0 && --h->W == 0) {
      h->tw = 0;
      nodes[h->n].alpha = 255;
      set_raw(h->n, rawx(h->n), h->PL);
      h->T = false;
    }
  }
  for (int i = 0; i < S->nholds; i++) {
    Hold *h = &S->holds[i];
    if (h->kind == H_STATIC) continue;
    if (!h->based) { ent_pos(h->n, &h->bx, &h->by); h->based = true; }
    h->phase = fmodf(h->phase + h->speed / 30, 1);
    float ox, oy;
    if (h->kind == H_MOVING) {
      float l = sqrtf(h->nx * h->nx + h->ny * h->ny), d = (sinf(h->phase * 6.2831853f) + 1) / 2 * h->dist;
      ox = l > 0 && d != 0 ? h->nx / l * d : 0;
      oy = l > 0 && d != 0 ? h->ny / l * d : 0;
    } else {
      float k = h->clockwise ? 1 - h->phase : h->phase;
      ox = sinf(k * 6.2831853f) * h->radius;
      oy = cosf(k * 6.2831853f) * h->radius;
    }
    ent_set_pos(h->n, h->bx + ox, h->by + oy);
  }
  NodeId g = S->climber;
  if (!g) return;
  if (S->mode != M_GROUND) {
    float x = rawx(g), y = rawy(g);
    for (NodeId c = nodes[S->map].first; c; c = nodes[c].next) {
      Rect r;
      if (!has(c, C_obstacleBounds) || !has(c, C_boundable) || !box_of(c, &r) || !inside(r, x, y)) continue;
      if (S->kC >= 0) { S->checks[S->kC].mY = true; S->checks[S->kC].delay = 1; }
      Ent *v = cl_vel();
      if (v) { v->vx = 0; v->vy = -4; }
      S->mode = M_FALL;
      SOUND(mza);
      SOUND(jda);
      break;
    }
  }
  if (--S->b7 <= 0) {
    int d = S->altitude > 43 ? 3 : S->altitude > 25 ? 2 : S->altitude > 15 ? 1 : 0;
    static const int8_t push[4] = {60, 30, 0, 0}, every[4] = {60, 50, 40, 30};
    NodeId b = ent_spawn(S_climbing_sx);
    if (b) {
      ent_add(b, S->map);
      float c = (frand() - .5f) * 300;
      c += (c < 0 ? -1 : 1) * push[d];
      ent_set_pos(b, rawx(g) - c, rawy(g) - 300);
    }
    S->b7 = every[d];
  }
}

/* yq: the camera looks at the champion, then comes down to the climber */
static float quad_in_out(float t) { return t < .5f ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t); }

static void sys_climb_camera(void) {
  if (!S->yq_on || !S->target) return;
  if (S->cam_state == 0) {
    if (S->meter) node_set_visible(S->meter, false);
    if (S->clock) node_set_visible(S->clock, false);
    S->qq_on = S->oq_on = false;
    S->cam_state = 1;
    ent_pos(S->target, &S->t0x, &S->t0y);
    ent_pos(S->climber, &S->t1x, &S->t1y);
    S->tween_t = 0;
  } else if (S->cam_state == 2) {
    if (!(S->kC >= 0 && S->checks[S->kC].mY)) {
      float x, y;
      ent_pos(S->climber, &x, &y);
      ent_set_pos(S->target, x, y);
    }
  }
}

/* the tween of yq: wait 1200 ms, then 1600 ms of quadInOut (between ticks) */
static void camera_tween(void) {
  if (S->cam_state != 1) return;
  S->tween_t++;
  float ms = S->tween_t * (1000.0f / 30);
  float t = ms <= 1200 ? 0 : ms >= 2800 ? 1 : quad_in_out((ms - 1200) / 1600);
  ent_set_pos(S->target, S->t0x + (S->t1x - S->t0x) * t, S->t0y + (S->t1y - S->t0y) * t);
  if (ms >= 2800 - .01f) {
    S->cam_state = 2;
    sys_countdown_start(&S->cd);
    S->counting = true;
    if (S->meter) node_set_visible(S->meter, true);
    if (S->clock) node_set_visible(S->clock, true);
  }
}

/* qq: the climber */
static void sys_climber(void) {
  if (!S->qq_on) return;
  NodeId g = S->climber;
  Ent *v = cl_vel();
  if (!g || !v) return;
  /* tq: the nearest hold */
  int near = -1, last = -1;
  bool close = false, ok = true;
  float best = 9999999999999.0f, cx = rawx(g), cy = rawy(g) - 27, lasty = -1e30f;
  for (int i = 0; i < S->nholds; i++) {
    Hold *h = &S->holds[i];
    float x, y;
    ent_pos(h->n, &x, &y);
    float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
    if (d < best) { best = d; near = i; }
    if (d < 15) close = true;
    if (y >= lasty) { lasty = y; last = i; }   /* the doodle keeps the last hold's state */
  }
  if (last >= 0 && S->holds[last].falling) ok = !(S->holds[last].T && S->holds[last].CT <= 0);
  switch (S->mode) {
    case M_GRAB:
      v->vx = v->vy = 0;
      hang_on(S->DY);
      if (--S->d7 <= 0) S->mode = M_HANG;
      S->EL = NEVER;
      break;
    case M_HANG:
      v->vx = v->vy = 0;
      hang_on(S->DY);
      if (in.pressed[A_ACTION]) { v->vx = 1.5f * in.jx; v->vy = -7; jump(); }
      if (in.jy > .84f) jump();
      break;
    case M_JUMP: {
      S->zY++;
      if (S->zaa) {
        v->vy = -7 * (1 - S->zY / 35.0f);
        if (!in.held[A_ACTION] || S->zY > 10) S->zaa = false;
      } else v->vy = fminf(13.3f, v->vy + .5f);
      if (in.jx != 0) {
        float h = (1 + fabsf(in.jx)) / (1 + sqrtf(.5f));
        v->vx = clampf(v->vx + in.jx * h * 1.5f, -2.8f * h, 2.8f * h);
      } else v->vx *= .5f;
      if ((S->zY > 16 || near != S->DY) && close && ok) {
        S->mode = M_GRAB;
        SOUND(lza);
        S->d7 = 3;
        climber_anim("hold", true);
        hang_on(near);
      }
      break;
    }
    case M_GROUND: {
      Ent *e = v;
      e->vx = e->vy = 0;
      float len = sqrtf(in.jx * in.jx + in.jy * in.jy);
      if (len > 0) {
        e->vx = in.jx * 2;
        e->vy = in.jy * 2;
        e->dir = (int8_t)dir_of(in.jx, in.jy, false);
        if (in.jy < 0 && ground_at(rawx(g) + e->vx, rawy(g) + e->vy) < 0) e->vy = 0;
        climber_anim("walk", false);
      } else climber_anim("idle", false);
      S->moving = len > 0;
      S->DY = -1;
      if (in.pressed[A_ACTION]) {
        e->vx = 1.5f * in.jx;
        e->vy = -7;
        jump();
        S->EL = rawy(g);
        S->D_ = ground_at(rawx(g), rawy(g));
      }
      break;
    }
    case M_FALL:
      v->vx = 0;
      v->vy = fminf(6.7f, v->vy + .5f);
      climber_anim("fall", false);
      break;
  }
  /* landing, or walking off a ledge */
  if (!((S->mode == M_JUMP && v->vy < 0) || S->mode == M_FALL)) {
    int a = ground_at(rawx(g), rawy(g));
    if (a >= 0) {
      if (S->mode != M_GROUND && (S->EL == NEVER || rawy(g) > S->EL - v->vy || a != S->D_)) {
        S->mode = M_GROUND;
        v->vy = 0;
        if (a == S->D_ && S->EL != NEVER && S->EL + v->vy >= rawy(g)) set_raw(g, rawx(g), S->EL);
        S->D_ = -1;
        S->EL = NEVER;
      }
    } else if (S->mode == M_GROUND) S->mode = M_JUMP;
  }
  /* checkpoints: the highest lantern reached; falling below it brings her back */
  float px, py;
  ent_pos(g, &px, &py);
  int m = -1;
  for (int i = 0; i < S->nchecks; i++)
    if (inside(S->checks[i].r, px, py)) { m = i; break; }
  if (m >= 0 && m != S->kC) {
    if (S->checks[m].lantern) { SOUND(kza); node_goto(S->checks[m].lantern, "on", 0, false); }
    if (S->kC < 0 || S->checks[m].py < S->checks[S->kC].py) S->kC = m;
  }
  if (S->kC >= 0) {
    Check *k = &S->checks[S->kC];
    if (k->mY) {
      k->delay -= 1.0f / 30;
      if (k->delay <= 0) {
        k->mY = false;
        S->mode = M_GROUND;
        ent_set_pos(g, k->px, k->py);
      }
    } else if (py > k->py + 50) {
      k->mY = true;
      k->delay = 1;
      S->mode = M_FALL;
      SOUND(jda);
    }
  }
}

/* Cq and Dq with the doodle's camera (ease .6, at most 60 px a tick) */
static void camera_goal(float *gx, float *gy) {
  NodeId b = S->map;
  float kx, ky;
  ent_pos(S->target, &kx, &ky);
  Mat l = node_local(b);
  float det = l.a * l.d - l.b * l.c;
  if (fabsf(det) < 1e-9f) { *gx = nodes[b].x; *gy = nodes[b].y; return; }
  /* the stage's centre in the map's space */
  float cx = (l.d * (480 - l.tx) - l.c * (270 - l.ty)) / det, cy = (-l.b * (480 - l.tx) + l.a * (270 - l.ty)) / det;
  float dx = kx - cx, dy = ky - cy;
  float ox = l.a * dx + l.c * dy, oy = l.b * dx + l.d * dy;
  if (fabsf(ox) < .1f) ox = 0;
  if (fabsf(oy) < .1f) oy = 0;
  float fx = 0, fy = 0;
  comp_vec(nodes[S->target].T, C_cameraTarget, F_offset, &fx, &fy);
  *gx = nodes[b].x - ox - fx;
  *gy = nodes[b].y - oy - fy;
}

static void sys_camera(void) {
  if (!S->target) return;
  camera_goal(&S->qx, &S->qy);
  NodeId b = S->map;
  uint16_t T = nodes[b].T;
  float ease = comp_float(T, C_camera, F_ease, .6f), speed = comp_float(T, C_camera, F_speed, 60);
  float dx = S->qx - nodes[b].x, dy = S->qy - nodes[b].y, d = sqrtf(dx * dx + dy * dy);
  if (d > 0) {
    float s = fminf(ease * d, speed) / d;
    nodes[b].x += dx * s;
    nodes[b].y += dy * s;
  }
}

/* Aq: the sky moves slower than the cliff */
static void sys_parallax(void) {
  NodeId p = S->iba;
  if (!p) return;
  if (!S->par_init) {
    S->par_init = true;
    S->par_dx = rawx(p); S->par_dy = rawy(p);
    S->par_hx = nodes[S->map].x; S->par_hy = nodes[S->map].y;
    S->par_fx = S->par_fy = 0;
    comp_vec(nodes[p].T, C_parallax, F_factor, &S->par_fx, &S->par_fy);
    /* the forest texture matches the copy of the sky seen from where the climber starts */
    if (S->nsky && S->climber) {
      float gx, gy;
      NodeId t = S->target;
      float tx, ty, cx, cy;
      ent_pos(t, &tx, &ty);
      ent_pos(S->climber, &cx, &cy);
      ent_set_pos(t, cx, cy);
      camera_goal(&gx, &gy);
      ent_set_pos(t, tx, ty);
      float iy = S->par_dy - S->par_fy * (S->par_hy - gy);
      int k = sky_top(-iy / 3);
      if (k >= 0) S->sky_ref = S->sky_y[k];
    }
  }
  float cx = S->par_hx - nodes[S->map].x, cy = S->par_hy - nodes[S->map].y;
  nodes[p].x = S->par_dx - S->par_fx * cx;
  nodes[p].y = S->par_dy - S->par_fy * cy;
}

/* Zo: the end, with the doodle's rating */
static void finish(int score, int rating) {
  if (S->ended) return;
  S->ended = true;
  S->score = score;
  S->rating = rating;
  S->end_t = 80;
  SOUND($Za);
  S->pq_on = S->qq_on = S->yq_on = S->vq_on = false;
}

/* zq */
static void sys_end(void) {
  if (S->ended) {
    if (--S->end_t == 0) menus_game_over_rated((float)S->score, S->rating);
    return;
  }
  NodeId g = S->climber;
  if (!g) return;
  Ent *v = cl_vel();
  if (S->has_goal && inside(S->goal, rawx(g), rawy(g))) {
    toast_style(msg("YOU_WIN"), 80, 0x222222, -1);
    finish(altitude_of(rawy(g)), 3);
    S->moving = false;
    if (v) v->vx = v->vy = 0;
  } else if (S->KY < 0) {
    toast_style(msg("TIMES_UP"), 80, 0x222222, -1);
    int b = altitude_of(rawy(g));
    finish(b, b > 60 ? 2 : b > 30 ? 1 : 0);
    S->moving = false;
    if (v) v->vx = v->vy = 0;
  }
}

static void tick(void) {
  sys_back_pauses();
  if (menus_active()) return;
  sys_tutorial_once();
  if (menus_active() || !S || !S->map) return;
  S->bg_ticks++;
  tweens();
  camera_tween();
  sys_sort_draw();
  sys_meter();
  sys_clock();
  sys_obstacles();
  sys_climb_camera();
  sys_climber();
  sys_sprite_dirs();
  sys_velocity();
  sys_camera();
  sys_delete_offscreen();
  sys_parallax();
  sys_end();
  if (S->counting) {
    /* Wo keeps the climber and the clock off until GO */
    S->qq_on = S->oq_on = false;
    if (sys_countdown(&S->cd)) {
      S->counting = false;
      S->qq_on = S->oq_on = true;
    }
  }
}

const SceneDef scene_climbing = {"climbing", start, tick, end, draw_under, NULL, NULL};
