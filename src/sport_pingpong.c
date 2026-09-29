/* Table tennis: the doodle's scene "ut" (kitsune20.js, from "var at =" to
 * "ut.prototype.start"), with its systems in the doodle's order:
 *
 *   Ep  Back pauses              Uo  the rules, the first time
 *   Wo  3, 2, 1, Go (et nt jt kt Ar tt dq Up wait for it)
 *   qt / pt  the end: a banner, then the results (the tutorial goes on to the
 *            island's intro cutscene)
 *   ot  balls out of play go     jt  the tengu serves a new ball every 20 ticks
 *                                    while fewer are in play than it() allows
 *   Rp  the player moves         et  paddles return the balls that enter their
 *                                    bounds; a full meter and OK: the power shot
 *   nt  bounce sounds            yr  the cannon world (balls bounce on the table)
 *   Ar  ground heights           lt  points, the HUD
 *   tt  shadows   dq  z sprites  Qp  draw order   Up  far balls go   Vp  effects end
 *   kt  smoke and flame trails behind the balls the player hit twice or more
 *
 * The trails are the doodle's "mna" and "UV" clips, a ball bitmap cached
 * through a colour filter; they are kept here as small records and drawn as
 * the filtered ball, in the map's draw order, without nodes. */
#include <math.h>
#include <stdio.h>
#ifdef HOST
#include <stdlib.h>
#endif
#include "../src/ent.h"
#include "../src/font.h"
#include "../src/phys.h"

#define AT (-135.0f / 900.0f)       /* at = -135 $f $f: gravity per tick squared */
#define BALL_MAX 10                 /* it() never allows more than 8 */
#define WALL_MAX 14
#define PART_MAX 48
#define KIDS_MAX 64

enum { SIDE_NONE, SIDE_PLAYER, SIDE_ENEMY };          /* playerSide / enemySide on a ball: who hit it last */
enum { PART_SMOKE, PART_FLAME };                      /* mna, UV */

typedef struct {
  NodeId n, sprite, shadow;
  int8_t body;          /* the physics has seen it (Gp) */
  uint8_t state;         /* 0 normal, 1 smoking, 2 flaming, 3 power shot */
  uint8_t dl;            /* DL: the player's hits */
  uint8_t side;
  bool live;             /* jU: in play */
  bool sleep;
  bool has_prev;
  float taa;             /* Taa: speed the player's hits add */
  float vx, vy, vz;      /* velocity component (px per tick) and pC */
  float z, kt;           /* zObject z and KT */
  float px, py;          /* iS: where its sprite was */
  Rect box;              /* its bounds, as of the last physics step */
} Ball;

typedef struct { float x, y; uint8_t kind, age; } Part;

typedef struct {
  NodeId root, map, ui, player, enemy, table, court, ptarget, etarget, ebounds, superbg;
  NodeId t_wp, t_eu, t_score, meter, pprog, eprog, plabel;
  NodeId walls[WALL_MAX];
  int16_t wall_body[WALL_MAX];
  int nwalls;
  int16_t player_body;
  Rect table_box, court_box, ptarget_box, etarget_box, ebounds_box;
  float table_top;                 /* its z + height */
  float pvx, pvy, pvz;             /* the player's velocity component */
  bool pmove;                      /* its playerMovement is findable (IC) */
  float pspeed;
  uint8_t pstate, estate;          /* pingponger states */
  bool tutorial, super_on;
  int super_t;                     /* ticks since the power shot began (the dark background fades in) */
  Ball balls[BALL_MAX];
  int nballs;
  /* pingpongGame */
  int wp, eu, haa, dab, dJ, max_balls, ty;
  float my;
  bool has_target;                 /* the player side's currentTarget, kept 500 ms */
  float tgx, tgy;
  int tg_t;
  int cd;                          /* Wo */
  bool counting, uo;
  bool over, reported;             /* Zo, Xd */
  int end_t, rating;
  float score;
  int last_meter, last_pprog, last_eprog, meter_f;
  Part parts[PART_MAX];
  int nparts;
  char s_wp[16], s_eu[16], s_score[16];
} State;

static State *S;

/* ---------------------------------------------------------------- the data's timelines */
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

static Mat key_mat(const TKey *k) {
  float a = 1, b = 0, c = 0, d = 1;
  if (k->mat) { const float *m = mat(k->mat); a = m[0]; b = m[1]; c = m[2]; d = m[3]; }
  return (Mat){a, b, c, d, k->x - (k->rx4 * .25f * a + k->ry4 * .25f * c), k->y - (k->rx4 * .25f * b + k->ry4 * .25f * d)};
}

/* the axis-aligned box of rectangle (x, y, w, h) placed by m */
static void mat_box(Mat m, float *x, float *y, float *w, float *h) {
  float xs[4] = {*x, *x + *w, *x, *x + *w}, ys[4] = {*y, *y, *y + *h, *y + *h};
  float lx = 1e9f, ly = 1e9f, hx = -1e9f, hy = -1e9f;
  for (int i = 0; i < 4; i++) {
    float X = m.a * xs[i] + m.c * ys[i] + m.tx, Y = m.b * xs[i] + m.d * ys[i] + m.ty;
    lx = fminf(lx, X); hx = fmaxf(hx, X); ly = fminf(ly, Y); hy = fmaxf(hy, Y);
  }
  *x = lx; *y = ly; *w = hx - lx; *h = hy - ly;
}

/* createjs getBounds() of a symbol at a frame: the union of what its timeline
 * shows (the libraries' nominal bounds are not kept) */
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
    mat_box(key_mat(&cur), &x, &y, &w, &h);
    lx = fminf(lx, x); hx = fmaxf(hx, x + w); ly = fminf(ly, y); hy = fmaxf(hy, y + h);
  }
  if (lx > hx) return false;
  *bx = lx; *by = ly; *bw = hx - lx; *bh = hy - ly;
  return true;
}

/* a map child drawn from the data (no node) becomes a node, so that sorting
 * the map's children keeps it (the tutorial's background) */
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

/* Mj(..., advance): a new clip shows its first frame and runs its script
 * (most stop there) */
static void init_frame0(NodeId n, int depth) {
  Node *p = &nodes[n];
  if (p->kind == NK_CLIP && node_mode(p) == MODE_INDEPENDENT && p->frame == 0 && (p->flags & NF_ONSTAGE))
    node_goto(n, NULL, 0, (p->flags & NF_PLAYING) != 0);
  if (depth < 8)
    for (NodeId c = p->first; c; c = nodes[c].next) init_frame0(c, depth + 1);
}

/* ---------------------------------------------------------------- positions (all in the map's space) */
static bool has(NodeId n, int c) { return n && nodes[n].T != NONE16 && comp_has(nodes[n].T, c); }

static void pos_of(NodeId n, float *x, float *y) {   /* N() */
  Mat t = node_to(n, S->map);
  *x = t.tx;
  *y = t.ty;
}

static void set_local(NodeId n, float x, float y) {  /* Dj() */
  Mat t = node_local(n);
  nodes[n].x = x + nodes[n].rx4 * .25f * t.a;
  nodes[n].y = y + nodes[n].ry4 * .25f * t.d;
}

static void set_pos(NodeId n, float x, float y) {    /* Fj() */
  Mat t = node_to(nodes[n].parent, S->map);
  float det = t.a * t.d - t.b * t.c;
  if (fabsf(det) < 1e-9f) return;
  float dx = x - t.tx, dy = y - t.ty;
  set_local(n, (t.d * dx - t.c * dy) / det, (-t.b * dx + t.a * dy) / det);
}

static NodeId bounds_child(NodeId e) {
  for (NodeId c = nodes[e].first; c; c = nodes[c].next)
    if (has(c, C_bounds)) return c;
  for (NodeId c = nodes[e].first; c; c = nodes[c].next)
    for (NodeId d = nodes[c].first; d; d = nodes[d].next)
      if (has(d, C_bounds)) return d;
  return 0;
}

/* Gj(): the transformed bounds of e's bounds child, taken to the map by two corners */
static bool box_of(NodeId e, Rect *r) {
  NodeId b = e ? bounds_child(e) : 0;
  float x, y, w, h;
  if (!b || !sym_bounds(nodes[b].sym, nodes[b].frame, 0, &x, &y, &w, &h)) return false;
  mat_box(node_local(b), &x, &y, &w, &h);
  Mat t = node_to(e, S->map);
  float x0 = t.a * x + t.c * y + t.tx, y0 = t.b * x + t.d * y + t.ty;
  float x1 = t.a * (x + w) + t.c * (y + h) + t.tx, y1 = t.b * (x + w) + t.d * (y + h) + t.ty;
  r->x = fminf(x0, x1);
  r->y = fminf(y0, y1);
  r->w = fabsf(x1 - x0);
  r->h = fabsf(y1 - y0);
  return true;
}

/* createjs Rectangle.contains: edges included */
static bool contains(Rect r, float x, float y) { return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h; }

/* Tj(): a random whole point of a rectangle */
static void rand_point(Rect r, float *x, float *y) {
  *x = floorf(r.x + frand() * r.w);
  *y = floorf(r.y + frand() * r.h);
}

/* Uj(): a meter clip at a fraction of its timeline */
static void meter(NodeId n, float v, int *last) {
  if (!n) return;
  int nf = node_frames(n), f = (int)floorf(v * (nf - 1));
  f = f < 0 ? 0 : f > nf - 1 ? nf - 1 : f;
  if (f != *last) node_goto(n, NULL, f, false);
  *last = f;
}

/* Rj(): a new entity in the map at (x, y) */
static NodeId spawn(uint16_t sym, float x, float y) {
  NodeId e = ent_spawn(sym);
  if (!e) return 0;
  ent_add(e, S->map);
  set_pos(e, x, y);
  init_frame0(e, 0);
  return e;
}

/* ---------------------------------------------------------------- balls */
static Ball *ball_of(NodeId n) {
  for (int i = 0; i < S->nballs; i++)
    if (S->balls[i].n == n) return &S->balls[i];
  return NULL;
}

static void ball_remove(int i) {
  Ball *b = &S->balls[i];
  ent_remove(b->n);
  memmove(&S->balls[i], &S->balls[i + 1], (size_t)(S->nballs - i - 1) * sizeof(Ball));
  S->nballs--;
}

/* it(): how many balls the tengu keeps in play */
static int allowed_balls(void) {
  static const int dt[8] = {0, 1, 5, 13, 16, 19, 22, 25};
  int g = 1;
  for (int m = 0; m < 8 && !(S->max_balls <= g); m++) {
    if (dt[m] <= S->wp) g = m + 1;
    else break;
  }
  return g;
}

/* jt: the tengu serves */
static void spawn_balls(void) {
  S->ty++;
  if (!(S->ty > 20 && S->nballs < allowed_balls()) || S->nballs >= BALL_MAX) return;
  S->ty = 0;
  float x, y;
  pos_of(S->enemy, &x, &y);
  NodeId n = spawn(S_pingpong_Qba, x, y);
  if (!n) return;
  Ball *b = &S->balls[S->nballs++];
  memset(b, 0, sizeof *b);
  b->n = n;
  b->sprite = node_child(n, "currSprite");
  /* its "smoking" pose (6 clips) is never shown (only a label of the ball's
   * root is asked for, which it does not have): it goes, to spare nodes */
  for (NodeId c = b->sprite ? nodes[b->sprite].first : 0; c; c = nodes[c].next)
    if (nodes[c].sym == S_pingpong_Sba) { node_free(c); break; }
  b->shadow = node_child(n, "shadow");
  b->body = -1;
  b->live = true;
  b->taa = 1;
  uint16_t T = nodes[n].T;
  comp_vec(T, C_velocity, F_velocity, &b->vx, &b->vy);
  b->z = comp_float(T, C_zObject, F_z, 0);
  if (!box_of(n, &b->box)) b->box = (Rect){x, y, 0, 0};
}

/* ot */
static void remove_dead(void) {
  for (int i = 0; i < S->nballs; i++)
    if (!S->balls[i].live) ball_remove(i--);
}

/* Up: balls far off the stage */
static void remove_far(void) {
  for (int i = 0; i < S->nballs; i++) {
    Mat g = node_global(S->balls[i].n);
    if (!(g.tx >= -1440 && g.ty >= -1440 && g.tx <= 960 + 1440 && g.ty <= 540 + 1440)) ball_remove(i--);
  }
}

/* ---------------------------------------------------------------- et: paddles */
/* ht(): where a ball is sent. The player's side keeps its target 500 ms. */
static void target_of(int side, float *x, float *y) {
  if (side == SIDE_ENEMY) { rand_point(S->etarget_box, x, y); return; }
  if (S->has_target) { *x = S->tgx; *y = S->tgy + 4 * frand() - 2; return; }
  rand_point(S->ptarget_box, x, y);
  S->tgx = *x;
  S->tgy = *y;
  S->has_target = true;
  S->tg_t = 15;
}

static void paddle_swing(int side) {   /* gt */
  NodeId paddle = side == SIDE_PLAYER ? S->player : S->enemy;
  Rect h;
  bool hb = side == SIDE_PLAYER ? box_of(S->player, &h) : (h = S->ebounds_box, S->ebounds != 0);
  if (!paddle || !hb) return;
  for (int i = 0; i < S->nballs; i++) {
    Ball *b = &S->balls[i];
    uint8_t f = side == SIDE_PLAYER ? S->pstate : S->estate;
    float x, y;
    pos_of(b->n, &x, &y);
    if (!(b->live && b->state != 3 && b->side != side && contains(h, x, y - b->z) && (side == SIDE_PLAYER || b->dl < 3) &&
          (f == 0 || (side == SIDE_ENEMY && b->dl == 0))))
      continue;
    b->side = (uint8_t)side;
    float bonus = 0;
    if (side == SIDE_PLAYER) {
      b->dl++;
      if (b->dl >= 3) { b->taa *= 6; b->state = 2; }       /* flaming */
      else if (b->dl >= 2) b->state = 1;                   /* smoking */
      bonus = b->taa;
    }
    float v = sqrtf(b->vx * b->vx + b->vy * b->vy) + bonus;
    float tx, ty;
    target_of(side == SIDE_PLAYER ? SIDE_ENEMY : SIDE_PLAYER, &tx, &ty);
    float dx = tx - x, dy = ty - y, d = sqrtf(dx * dx + dy * dy);
    if (d > 0 && v > 0) { b->vx = dx * v / d; b->vy = dy * v / d; }
    else b->vx = b->vy = 0;
    float t = v > 0 ? d / v : 0;
    if (t > 1e-3f) b->vz = (S->table_top - .5f * AT * t * t - b->z) / t;
    ent_label(paddle, "swing", false);
    if (side == SIDE_PLAYER) {
      S->pstate = 1;
      S->pmove = false;
      S->pvx = S->pvy = 0;
      S->haa++;
      S->my = clampf(S->my + 1 / (6 + 1.2f * allowed_balls()), 0, 1);
      SOUND(hit);
    } else {
      S->estate = 1;
      S->dab++;
      SOUND(enemy_hit);
      float ex, ey;
      pos_of(paddle, &ex, &ey);
      if (hypotf(ex - x, ey - y) > 15) {
        /* the tengu vanishes and appears where the ball is */
        spawn(nodes[paddle].sym == S_pingpong_bga ? S_pingpong_RGa : S_pingpong_xDa, ex, ey);
        set_pos(paddle, x, y);
      }
    }
  }
}

/* ft: the power shot */
static bool power_shot(void) {
  if (!in.pressed[A_ACTION]) return false;          /* ak.Ca: pressed this tick */
  if (S->pstate == 2 || S->pstate == 3 || S->my < 1) return false;
  SOUND(power);
  S->pstate = 2;
  ent_play_label(S->player, "super");
  S->pmove = false;
  S->pvx = S->pvy = 0;
  S->estate = 3;
  for (int i = 0; i < S->nballs; i++)
    if (S->balls[i].body >= 0 && S->balls[i].live) S->balls[i].sleep = true;
  S->haa += S->nballs;
  S->my = 0;
  S->ty = -1000000;
  nodes[S->enemy].flags &= (uint8_t)~NF_TICK;
  S->super_on = true;
  S->super_t = 0;
  return true;
}

/* (the engine also ticks the sprites a clip is not showing, and their scripts
 * dispatch: only the pose on stage counts, as in createjs) */
static bool showing(NodeId n, const char *label) { return !strcmp(node_label(n), label); }

static void on_super_swing(NodeId t, uint16_t ev, void *ctx) {
  if (!S || !S->super_on || !showing(t, "super")) return;
  SOUND(power_swing);
  for (int i = 0; i < S->nballs; i++) {
    Ball *b = &S->balls[i];
    b->state = 3;
    if (b->sprite) node_goto(b->sprite, "flaming", 0, true);
    b->dl = 3;
    if (b->body >= 0 && b->live) {
      b->sleep = false;
      b->vx = 20;
      b->vy = 0;
      b->vz = 0;
      b->side = SIDE_PLAYER;
    }
  }
}

static void on_super_end(NodeId t, uint16_t ev, void *ctx) {
  if (!S || !S->super_on || !showing(t, "super")) return;
  S->super_on = false;
  S->pstate = 0;
  S->pmove = true;
  ent_label(S->player, "walk", false);
  S->ty = 0;
  S->estate = 0;
  nodes[S->enemy].flags |= NF_TICK;
  if (S->superbg) nodes[S->superbg].alpha = 0;
}

/* st: a paddle's swing ends */
static void on_swing_finished(NodeId t, uint16_t ev, void *ctx) {
  if (!S || !showing(t, "swing")) return;
  ent_label(t, "walk", false);
  if (t == S->player) { S->pstate = 0; S->pmove = true; }
  else S->estate = 0;
}

/* ---------------------------------------------------------------- yr: the cannon world */
static int16_t body_for(NodeId n, const char *type, Rect r, float z, float height) {
  int16_t b = (int16_t)phys_add(phys_type(type), SH_BOX, r.x + r.w / 2, r.y + r.h / 2, z + height / 2, r.w / 2, r.h / 2, height / 2);
  if (b >= 0) bodies[b].user = n;
  return b;
}

/* A ball in cannon.js: 4 substeps; contacts found from the positions at the
 * start of each substep (the table's box, the ground plane), solved as spook
 * contact equations (stiffness 1e7, relaxation 3, restitution 1, a face's 4
 * points), then gravity and the move. The spook terms give the bounces the
 * little kick they have in the doodle. */
#define SUBSTEPS 4
static void ball_step(Ball *b) {
  const float h = 1.0f / (FPS * SUBSTEPS), G = -135;
  const float sa = 4 / (h * 13), sb = 12.0f / 13, eps = 4 / (h * h * 1e7f * 13), f = .4f / (.4f + eps);
  float hw = b->box.w / 2, hh = b->box.h / 2, hz = 1.5f;
  float x = b->box.x + hw, y = b->box.y + hh, z = b->z + hz;
  float vx = 30 * b->vx, vy = 30 * b->vy, vz = 30 * b->vz;
  Rect t = S->table_box;
  float tx = t.x + t.w / 2, ty = t.y + t.h / 2, thz = S->table_top / 2, tz = thz;
  for (int s = 0; s < SUBSTEPS; s++) {
    float dvx = 0, dvy = 0, dvz = 0;
    /* the ground */
    if (z - hz < 0) {
      float B = (hz - z) * sa - 2 * vz * sb - h * G;
      if (B > 0) dvz += f * B;
    }
    /* the table: along the axis where they overlap least */
    if (S->table) {
      float ox = hw + t.w / 2 - fabsf(x - tx), oy = hh + t.h / 2 - fabsf(y - ty), oz = hz + thz - fabsf(z - tz);
      if (ox > 0 && oy > 0 && oz > 0) {
        float nx = 0, ny = 0, nz = 0, pen;
        if (ox <= oy && ox <= oz) { nx = x > tx ? 1 : -1; pen = ox; }
        else if (oy <= oz) { ny = y > ty ? 1 : -1; pen = oy; }
        else { nz = z > tz ? 1 : -1; pen = oz; }
        float B = pen * sa - 2 * (vx * nx + vy * ny + vz * nz) * sb - h * G * nz;
        if (B > 0) { dvx += f * B * nx; dvy += f * B * ny; dvz += f * B * nz; }
      }
    }
    vx += dvx;
    vy += dvy;
    vz += dvz + G * h;
    x += vx * h;
    y += vy * h;
    z += vz * h;
  }
  float ox, oy;
  pos_of(b->n, &ox, &oy);
  float nx = x + ox - (b->box.x + hw), ny = y + oy - (b->box.y + hh);
  set_pos(b->n, nx, ny);
  b->box.x += nx - ox;
  b->box.y += ny - oy;
  b->z = z - hz;
  b->vx = vx / 30;
  b->vy = vy / 30;
  b->vz = vz / 30;
}

static void physics(void) {
  /* the player: a character among the walls */
  Rect r;
  bool pb = box_of(S->player, &r);
  float px, py;
  pos_of(S->player, &px, &py);
  if (pb && S->player_body < 0) S->player_body = body_for(S->player, "character", r, 0, 100);
  if (S->player_body >= 0) {
    Body *b = &bodies[S->player_body];
    b->x = r.x + r.w / 2; b->y = r.y + r.h / 2; b->z = 50;
    b->vx = 30 * S->pvx; b->vy = 30 * S->pvy; b->vz = 30 * S->pvz;
    phys_step(1.0f / FPS);
    if (pb) {
      S->pvx = b->vx / 30; S->pvy = b->vy / 30; S->pvz = b->vz / 30;
      set_pos(S->player, b->x + px - (r.x + r.w / 2), b->y + py - (r.y + r.h / 2));
    }
  }
  /* the balls (a sleeping body keeps its place and speed) */
  for (int i = 0; i < S->nballs; i++) {
    Ball *b = &S->balls[i];
    if (!box_of(b->n, &b->box)) continue;
    b->body = 1;
    if (!b->sleep) ball_step(b);
  }
}

/* Ar: the highest surface under each ball's corners (rays from z + 9 down: the table, the ground) */
static void ground_heights(void) {
  for (int i = 0; i < S->nballs; i++) {
    Ball *b = &S->balls[i];
    Rect r = b->box, t = S->table_box;
    float xs[4] = {r.x, r.x + r.w, r.x, r.x + r.w}, ys[4] = {r.y, r.y, r.y + r.h, r.y + r.h}, g = 0;
    for (int k = 0; S->table && S->table_top <= b->z + 9 && k < 4; k++)
      if (contains(t, xs[k], ys[k])) g = fmaxf(g, fminf(S->table_top, b->z));
    b->kt = g;
  }
}

/* ---------------------------------------------------------------- lt: points and the HUD */
static void popup(float x, float y, const char *s) {   /* mt */
  NodeId p = spawn(S_pingpong_f5, x, y);
  NodeId t = p ? node_find(p, "text") : 0;
  if (t) node_set_text(t, s);
}

static void scoring(void) {
  static const char *const minus[4] = {"-40", "-30", "-20", "-10"};
  for (int i = 0; i < S->nballs; i++) {
    Ball *b = &S->balls[i];
    float x, y;
    pos_of(b->n, &x, &y);
    if (!b->live || !S->court || contains(S->court_box, x, y)) continue;
    if (b->side == SIDE_PLAYER) { SOUND(point); popup(x - 22, y, "+100"); S->wp++; }
    else if (b->side == SIDE_ENEMY) { SOUND(miss); popup(x + 22, y, minus[b->dl < 4 ? b->dl : 3]); S->eu++; }
    b->live = false;
  }
  if (S->dJ >= 10) {
    snprintf(S->s_wp, sizeof S->s_wp, "%02d / %d", S->wp, S->dJ);
    snprintf(S->s_eu, sizeof S->s_eu, "%02d / %d", S->eu, S->dJ);
  } else {
    snprintf(S->s_wp, sizeof S->s_wp, "%d / %d", S->wp, S->dJ);
    snprintf(S->s_eu, sizeof S->s_eu, "%d / %d", S->eu, S->dJ);
  }
  snprintf(S->s_score, sizeof S->s_score, "%d", 100 * (S->wp > S->eu ? S->wp - S->eu : 0));
  if (S->t_wp) node_set_text(S->t_wp, S->s_wp);
  if (S->t_eu) node_set_text(S->t_eu, S->s_eu);
  if (S->t_score) node_set_text(S->t_score, S->s_score);
  meter(S->pprog, (float)S->wp / S->dJ, &S->last_pprog);
  meter(S->eprog, (float)S->eu / S->dJ, &S->last_eprog);
  /* the power meter's progress bar is masked (frames 1-30): the clip stays on
   * its empty frame and draw_over draws the bar as far as the mask shows it */
  int mf = (int)floorf(S->my * 31);
  S->meter_f = mf < 0 ? 0 : mf > 31 ? 31 : mf;
  meter(S->meter, S->meter_f == 31 ? 1.0f : 0.0f, &S->last_meter);
  if (S->plabel) node_set_text(S->plabel, msg("TUT_PINGPONG_DESKTOP_ACTION"));
}

/* ---------------------------------------------------------------- qt / pt: the end */
static void end_of_game(void) {
  if (S->over && --S->end_t <= 0 && !S->reported) {
    S->reported = true;
    if (S->tutorial) game_go("cutscene:intro");
    else menus_game_over_rated(S->score, S->rating);
    return;
  }
  if (S->over || !(S->wp >= S->dJ || S->eu >= S->dJ)) return;
  int32_t sh = 0x111111, ol = 0x555555;
  if (S->tutorial) {
    toast_style(msg(S->wp > S->eu ? "YOU_WIN" : "YOU_LOSE"), 80, sh, ol);
    store_set_bool("TUTORIAL_DONE", true);
    S->score = 1;
    S->rating = 3;
  } else {
    S->rating = S->wp >= 30 ? 3 : S->wp >= 20 ? 2 : S->wp >= 10 ? 1 : 0;
    S->score = 100.0f * (S->wp + (S->wp > S->eu ? S->wp - S->eu : 0));
    toast_style(msg(S->wp > S->eu ? "YOU_WIN" : "TENGU_WINS"), 80, sh, ol);
  }
  SOUND(end);
  S->over = true;
  S->end_t = 80;
}

/* ---------------------------------------------------------------- tt, dq, kt */
static void shadows(void) {
  for (int i = 0; i < S->nballs; i++)
    if (S->balls[i].shadow) set_local(S->balls[i].shadow, 0, -S->balls[i].kt);
}

static void zsprites(void) {
  for (int i = 0; i < S->nballs; i++)
    if (S->balls[i].sprite && node_visible(S->balls[i].sprite)) set_local(S->balls[i].sprite, 0, -S->balls[i].z);
}

static void add_part(float x, float y, uint8_t kind) {
  Part *p;
  if (S->nparts < PART_MAX) p = &S->parts[S->nparts++];
  else {   /* the oldest makes room */
    p = &S->parts[0];
    for (int i = 1; i < S->nparts; i++)
      if (S->parts[i].age > p->age) p = &S->parts[i];
  }
  *p = (Part){x, y, kind, 0};
}

static void trails(void) {
  for (int i = 0; i < S->nballs; i++) {
    Ball *b = &S->balls[i];
    if (b->sleep || !b->sprite) continue;
    float x, y;
    pos_of(b->sprite, &x, &y);
    if (b->has_prev) {
      if (b->state == 2) {
        if (sqrtf(b->vx * b->vx + b->vy * b->vy) > 15) {
          add_part(x + (b->px - x) * .33f, y + (b->py - y) * .33f, PART_FLAME);
          add_part(x + (b->px - x) * .66f, y + (b->py - y) * .66f, PART_FLAME);
        } else add_part(x + (b->px - x) * .5f, y + (b->py - y) * .5f, PART_FLAME);
        add_part(b->px, b->py, PART_FLAME);
      } else if (b->state == 1 || b->state == 3) add_part(b->px, b->py, PART_SMOKE);
    }
    b->px = x;
    b->py = y;
    b->has_prev = true;
  }
}

/* the trails are clips too: they age with the Ticker and go at their last frame (Vp) */
static void parts_age(void) {
  for (int i = 0; i < S->nparts; i++) S->parts[i].age++;
}

static void parts_end(void) {
  for (int i = 0; i < S->nparts; i++)
    if (S->parts[i].age >= (S->parts[i].kind == PART_FLAME ? 6 : 5)) S->parts[i--] = S->parts[--S->nparts];
}

/* ---------------------------------------------------------------- Qp: draw order */
static float sort_key(NodeId c) {
  uint16_t T = nodes[c].T;
  float x, y;
  if (T != NONE16 && nodes[c].ent) {
    if (comp_has(T, C_drawOrderOverride)) return comp_float(T, C_drawOrderOverride, F_drawOrder, 0);
    if (comp_has(T, C_zObject) && comp_has(T, C_boundable) && comp_has(T, C_zBoundable)) {
      float h = comp_float(T, C_zBoundable, F_height, 100);
      Ball *b = nodes[c].sym == S_pingpong_Qba ? ball_of(c) : NULL;
      if (b) return b->box.y + h + b->kt;
      if (c == S->table) return S->table_box.y + h + comp_float(T, C_zObject, F_z, 0);
      Rect r;
      if (box_of(c, &r)) return r.y + h + comp_float(T, C_zObject, F_z, 0);
    }
  }
  pos_of(c, &x, &y);
  return y;
}

static void sort_map(void) {
  NodeId kids[KIDS_MAX];
  float keys[KIDS_MAX];
  int n = 0;
  NodeId rest = 0;
  for (NodeId c = nodes[S->map].first; c; c = nodes[c].next) {
    if (n == KIDS_MAX) { rest = c; break; }
    kids[n] = c;
    keys[n] = sort_key(c);
    for (int j = n++; j > 0 && keys[j - 1] > keys[j]; j--) {
      float k = keys[j]; keys[j] = keys[j - 1]; keys[j - 1] = k;
      NodeId t = kids[j]; kids[j] = kids[j - 1]; kids[j - 1] = t;
    }
  }
  if (!n) return;
  nodes[S->map].first = kids[0];
  for (int i = 0; i + 1 < n; i++) nodes[kids[i]].next = kids[i + 1];
  nodes[kids[n - 1]].next = rest;
  nodes[S->map].flags |= NF_SORTED;
}

/* ---------------------------------------------------------------- start */
static void find_walls(NodeId n, int depth) {
  for (NodeId c = nodes[n].first; c; c = nodes[c].next) {
    if (has(c, C_boundable) && has(c, C_collidable) && !has(c, C_velocity) && c != S->table) {
      Rect r;
      if (S->nwalls < WALL_MAX && box_of(c, &r)) {
        S->walls[S->nwalls] = c;
        S->wall_body[S->nwalls++] = body_for(c, "prop", r, 0, 100);
      }
    } else if (depth < 2 && !has(c, C_velocity) && !has(c, C_pingponger)) find_walls(c, depth + 1);
  }
}

static NodeId map_child_with(int c1, int c2) {
  for (NodeId c = nodes[S->map].first; c; c = nodes[c].next)
    if (has(c, c1) && (c2 < 0 || has(c, c2))) return c;
  return 0;
}

static void start(void) {
  S = scene_state(sizeof(State));
  const char *v = game.variant;
  const char *label = !strcmp(v, "hard") || !strcmp(v, "ultra") || !strcmp(v, "tutorial") ? v : "game";
  S->tutorial = !strcmp(v, "tutorial");
  S->player_body = -1;
  S->last_meter = S->last_pprog = S->last_eprog = -1;
  S->pmove = true;
  S->ty = 10000;
  S->cd = 105;
  S->counting = true;

  /* rt: the table's world */
  phys_reset();
  phys_gravity(-135);
  phys_body_type("ball", "ballMaterial", 10, 8, 17, 0);
  phys_body_type("table", "tableMaterial", 0, 18, 8, .4f);
  phys_contact_material("ballMaterial", "tableMaterial", 0, 1);
  phys_contact_material("groundMaterial", "ballMaterial", 0, 1);

  NodeId root = node_new_sym(S_pingpong__S_bb);
  if (!root) return;
  S->root = root;
  node_add(game.root, root);
  node_goto(root, label, 0, false);
  node_update_one(root);
  /* only this variant's map */
  for (NodeId c = nodes[root].first, nx; c; c = nx) {
    nx = nodes[c].next;
    if (!(nodes[c].flags & NF_ONSTAGE)) node_free(c);
  }
  node_update(root);
  for (NodeId c = nodes[root].first; c; c = nodes[c].next) {
    if (has(c, C_map)) S->map = c;
    if (node_named(c, "ui")) S->ui = c;
  }
  if (!S->map) return;
  materialize(S->map);
  node_update(root);
  init_frame0(root, 0);
  ent_register_tree(root);

  S->player = map_child_with(C_pingponger, C_playerSide);
  S->enemy = map_child_with(C_pingponger, C_enemySide);
  S->table = map_child_with(C_pingpongTable, -1);
  S->court = map_child_with(C_pingpongCourt, -1);
  S->ptarget = map_child_with(C_playerSide, C_pingpongTarget);
  S->etarget = map_child_with(C_enemySide, C_pingpongTarget);
  S->ebounds = map_child_with(C_enemySide, C_pingpongPaddleBounds);
  S->superbg = map_child_with(C_superMoveBackground, -1);
  if (!S->player || !S->enemy) { S->map = 0; return; }
  uint16_t T = nodes[S->map].T;
  S->dJ = comp_int(T, C_pingpongGame, F_maxPoints, 30);
  S->max_balls = comp_int(T, C_pingpongGame, F_maxBalls, 5);
  if (S->dJ < 1) S->dJ = 1;
  S->pspeed = comp_float(nodes[S->player].T, C_playerMovement, F_speed, 3);
  if (S->ui) {
    S->t_wp = node_find(S->ui, "playerPoints");
    S->t_eu = node_find(S->ui, "enemyPoints");
    S->t_score = node_find(S->ui, "metaScore");
    S->meter = node_find(S->ui, "superMeter");
    S->pprog = node_find(S->ui, "playerProgress");
    S->eprog = node_find(S->ui, "enemyProgress");
    S->plabel = S->meter ? node_find(S->meter, "powerShotLabel") : 0;
  }
  if (S->table) {
    uint16_t tt = nodes[S->table].T;
    S->table_top = comp_float(tt, C_zObject, F_z, 0) + comp_float(tt, C_zBoundable, F_height, 100);
    if (!box_of(S->table, &S->table_box)) S->table = 0;
  }
  if (!S->court || !box_of(S->court, &S->court_box)) S->court = 0;
  if (!S->ptarget || !box_of(S->ptarget, &S->ptarget_box)) S->ptarget_box = (Rect){0, 0, 0, 0};
  if (!S->etarget || !box_of(S->etarget, &S->etarget_box)) S->etarget_box = (Rect){0, 0, 0, 0};
  if (!S->ebounds || !box_of(S->ebounds, &S->ebounds_box)) S->ebounds = 0;
  find_walls(S->map, 0);

  node_on(S->player, "swing_finished", on_swing_finished, NULL);
  node_on(S->enemy, "swing_finished", on_swing_finished, NULL);
  node_on(S->player, "super_swing", on_super_swing, NULL);
  node_on(S->player, "super_end", on_super_end, NULL);
  sort_map();
}

/* ---------------------------------------------------------------- the systems */
static void countdown(void) {   /* Wo */
  if (!S->counting) return;
  int32_t sh = 0x222222, ol = 0xaaaaaa;
  if (S->cd == 90) { toast_style("3", 100, sh, ol); SOUND(beep); }
  else if (S->cd == 60) { toast_style("2", 100, sh, ol); SOUND(beep); }
  else if (S->cd == 30) { toast_style("1", 100, sh, ol); SOUND(beep); }
  else if (S->cd == 0) { toast_style(msg("GO"), 100, sh, ol); SOUND(go); S->counting = false; }
  if (S->cd > 0) S->cd--;
}


#if defined(HOST) || defined(PP_AUTO)
/* tests: PPAUTO=1 (or a build with -DPP_AUTO) plays the player: it goes to
 * the ball coming its way and uses the power shot when two balls are in play */
static void autopilot(void) {
#ifdef PP_AUTO
  const bool on = true;
#else
  static int on = -1;
  if (on < 0) on = getenv("PPAUTO") != NULL;
#endif
  if (!on || !S->pmove) return;
  Rect r;
  if (!box_of(S->player, &r)) return;
  float best = 1e9f, gx = 0, gy = 0;
  bool any = false;
  for (int i = 0; i < S->nballs; i++) {
    Ball *b = &S->balls[i];
    float x, y;
    pos_of(b->n, &x, &y);
    if (!b->live || b->side == SIDE_PLAYER || x > S->table_box.x + S->table_box.w) continue;
    if (x < best) { best = x; gx = x; gy = y - b->z; any = true; }
  }
  in.jx = in.jy = 0;
  if (any) {
    float dx = gx - (r.x + r.w / 2), dy = gy - (r.y + r.h / 2), d = sqrtf(dx * dx + dy * dy);
    if (d > 2) { in.jx = dx / d; in.jy = dy / d; }
  }
  in.pressed[A_ACTION] = S->my >= 1 && S->nballs >= 2;
}
#else
static void autopilot(void) {}
#endif

static void movement(void) {   /* Rp */
  if (!S->pmove) return;
  S->pvx = in.jx * S->pspeed;
  S->pvy = in.jy * S->pspeed;
}


#ifdef HOST
/* tests: PPDBG=1 prints the state every tick (compare with the doodle's) */
static void debug_log(void) {
  static int on = -1;
  if (on < 0 && (on = getenv("PPDBG") != NULL)) printf("state %u bytes\n", (unsigned)sizeof(State));
  if (!on) return;
  float px, py, ex, ey;
  pos_of(S->player, &px, &py);
  pos_of(S->enemy, &ex, &ey);
  printf("T %u p %.2f %.2f e %.2f %.2f wp %d eu %d my %.3f", game.ticks, px, py, ex, ey, S->wp, S->eu, S->my);
  for (int i = 0; i < S->nballs; i++) {
    Ball *b = &S->balls[i];
    float x, y;
    pos_of(b->n, &x, &y);
    printf(" [%.2f %.2f z %.2f kt %.2f v %.3f %.3f %.3f dl %d st %d]", x, y, b->z, b->kt, b->vx, b->vy, b->vz, b->dl, b->state);
  }
  printf("\n");
}
#else
static void debug_log(void) {}
#endif

static void tick(void) {
  if (!S || !S->map) { sys_back_pauses(); return; }
  nodes[S->map].flags |= NF_VISIBLE;
  parts_age();
  sys_back_pauses();                                   /* Ep */
  if (menus_active()) return;
  if (!S->uo) {                                        /* Uo */
    S->uo = true;
    sys_tutorial_once();
    if (menus_active()) return;
  }
  countdown();                                         /* Wo */
  bool on = !S->counting;
  end_of_game();                                       /* qt, pt */
  remove_dead();                                       /* ot */
  if (on && !S->over) spawn_balls();                   /* jt */
  autopilot();
  movement();                                          /* Rp */
  if (on && !S->over && !power_shot()) {               /* et */
    paddle_swing(SIDE_PLAYER);
    paddle_swing(SIDE_ENEMY);
  }
  physics();                                           /* yr */
  if (on) ground_heights();                            /* Ar */
  debug_log();
  if (!S->over || S->tutorial) scoring();              /* lt */
  if (on) { shadows(); zsprites(); }                   /* tt, dq */
  sort_map();                                          /* Qp */
  if (on) remove_far();                                /* Up */
  parts_end();                                         /* Vp */
  sys_ephemeral();
  if (on) trails();                                    /* kt */
  if (S->has_target && --S->tg_t <= 0) S->has_target = false;
  if (S->super_on && S->superbg) {                     /* the dark background: alpha to 1 in 10 ticks */
    S->super_t++;
    nodes[S->superbg].alpha = (uint8_t)(S->super_t >= 10 ? 255 : S->super_t * 255 / 10);
  }
}

/* ---------------------------------------------------------------- drawing */
/* the trails: the doodle's ball bitmap through mna's or UV's colour filter */
static void draw_part(const Part *p, Mat m) {
  static const uint8_t alpha_flame[7] = {255, 170, 85, 0, 0, 0, 0}, alpha_smoke[6] = {255, 255, 191, 128, 64, 0};
  uint8_t a = p->kind == PART_FLAME ? alpha_flame[p->age < 7 ? p->age : 6] : alpha_smoke[p->age < 6 ? p->age : 5];
  if (!a) return;
  uint16_t c = p->kind == PART_FLAME ? rgb565(200, 206, 255) : rgb565(255, 129, 74);
  int x = (int)floorf(m.a * p->x + m.c * p->y + m.tx - 3 + .5f), y = (int)floorf(m.b * p->x + m.d * p->y + m.ty - 3 + .5f);
  gfx_rect(x + 1, y, 4, 2, c, a);
  gfx_rect(x, y + 2, 6, 2, c, a);
  gfx_rect(x + 1, y + 4, 4, 2, c, a);
}

/* the points that pop up: "+100" in white, the tengu's in red */
static void draw_popup(NodeId f, Mat parent) {
  if (!node_visible(f) || !nodes[f].alpha) return;
  Mat m = mat_mul(parent, node_local(f));
  for (NodeId a = nodes[f].first; a; a = nodes[a].next) {
    if (!node_visible(a) || !nodes[a].alpha) continue;
    Mat ma = mat_mul(m, node_local(a));
    unsigned al = nodes[f].alpha * nodes[a].alpha / 255;
    for (NodeId t = nodes[a].first; t; t = nodes[t].next) {
      if (nodes[t].kind != NK_TEXT || !node_visible(t) || nodes[t].ref == NONE16) continue;
      Mat mt = mat_mul(ma, node_local(t));
      const char *s = node_text(t);
      node_text_draw(t, mt, s, s[0] == '-' ? 0xF986 : 0xFFFF, (uint8_t)(al * nodes[t].alpha / 255));   /* #ff3333, #fff */
    }
  }
}

/* The map, drawn here in its sorted order with the trails among its
 * children; the engine's own pass then skips it (draw_over shows it again). */
static void draw_under(void) {
  if (!S || !S->map) return;
  Node *mp = &nodes[S->map];
  mp->flags |= NF_VISIBLE;
  if (!node_visible_chain(S->map)) return;
  Mat m = mat_mul((Mat){1.0f / 3, 0, 0, 1.0f / 3, 0, 0}, node_global(S->map));
  /* the trails by their y (their draw order), oldest first among equals */
  int order[PART_MAX], np = 0;
  for (int i = 0; i < S->nparts; i++) {
    int j = np++;
    for (; j > 0 && S->parts[order[j - 1]].y > S->parts[i].y; j--) order[j] = order[j - 1];
    order[j] = i;
  }
  int pi = 0;
  for (NodeId c = mp->first; c; c = nodes[c].next) {
    if (pi < np) {
      float k = sort_key(c);
      while (pi < np && S->parts[order[pi]].y < k) draw_part(&S->parts[order[pi++]], m);
    }
    if (nodes[c].sym == S_pingpong_f5) draw_popup(c, m);
    else node_draw(c, m);
  }
  while (pi < np) draw_part(&S->parts[order[pi++]], m);
  mp->flags &= (uint8_t)~NF_VISIBLE;
}

/* l4a's frames 1 to 30: PowerShotBarProgressArt (a yellow bar with round
 * ends, at (1, 1)) under a mask whose right edge is at 3 + 10 (frame - 1) */
static void draw_meter(void) {
  int f = S->meter_f;
  if (!S->meter || f < 1 || f > 30 || !node_visible_chain(S->meter)) return;
  Mat m = mat_mul((Mat){1.0f / 3, 0, 0, 1.0f / 3, 0, 0}, node_global(S->meter));
  int w = 2 + 10 * (f - 1), x = (int)floorf(m.tx + 1 + .5f), y = (int)floorf(m.ty + 1 + .5f);
  uint16_t c = rgb565(253, 202, 64);
  gfx_rect(x, y + 1, w, 4, c, 255);
  gfx_rect(x + 1, y, w - 1, 1, c, 255);
  gfx_rect(x + 1, y + 5, w - 1, 1, c, 255);
}

static void draw_over(void) {
  if (!S) return;
  if (S->map) nodes[S->map].flags |= NF_VISIBLE;
  if (S->map) draw_meter();
}

static void end(void) {
  phys_reset();
  S = NULL;
}

const SceneDef scene_pingpong = {"pingpong", start, tick, end, draw_under, draw_over, NULL};
