/* Rugby: the doodle's scene It (library 6CC3977E663B4BA1A079CF41822948DB).
 *
 * Lucky and four teammates carry the ball across the oni's field. The arrows
 * move the carrier, OK passes to the teammate the white arrow points at (one
 * behind, on the side last pressed). Oni chase the carrier: six ticks in
 * contact with the ball and the oni win. Powerups last five seconds: speed,
 * noclip (through the rocks) and pacman (oni flee and can be eaten, 100
 * points each). Reaching the end zone wins (three stars); the score is the
 * distance run, 10 points per metre-ish unit.
 *
 * Systems, in the doodle's order: Ep (pause), Uo (rules), Wo (countdown),
 * Vp (effects), Hr (end zone trigger), Os (lava animation), Ls (visibility),
 * Pp (draw order), wt (carrier), xt (ball in flight), Dt (powerups), zt (oni),
 * yt (teammates), Bt (tackles), dq (ball height), yr (physics), Gt (the
 * teammates' A* paths), Cq/Dq (camera), aq (sprite directions), Ct (HUD),
 * Et (the end).
 *
 * The field is long, so it is not built as one tree: its 159 rocks are drawn
 * from the data through one node per kind and get physics bodies only near
 * what moves, the tile background is drawn from its timeline, characters
 * only have the children of their current pose, and oni, powerups and the
 * end zone exist as nodes only while the doodle shows them (Ls). */
#include <math.h>
#include <stdio.h>
#include "ent.h"
#include "phys.h"

#define ACT_MAX 32
#define OB_MAX 176
#define FX_MAX 6
#define PROTO_MAX 16
#define GRID_BYTES 1100
#define SQRT2 1.41421356f

enum { A_CHAR, A_ENEMY, A_POWER, A_END };
enum { AF_VIS = 1, AF_GONE = 2, AF_ALLY = 4, AF_PF = 8, AF_TARGET = 16 };
enum { PW_NONE, PW_SPEED, PW_PACMAN, PW_NOCLIP };
enum { EN_RED, EN_BLUE, EN_CHARGE, EN_HEAVY };
static const float en_speed[4] = {2.2f, 3, 6, 2.5f}, en_turn[4] = {50, 3, 100, 20};
#define EN_RANGE 200.0f      /* pY */
#define END_DELAY 80         /* Yo: ticks from the end to the results */

typedef struct {
  NodeId n;                  /* its node while it has one */
  uint16_t sym;
  uint8_t slot, kind, type, flags;
  uint8_t frame;             /* the clip's frame (pose), kept while it has no node */
  int8_t dir;                /* direction component, -1: none (mh of a zero vector) */
  uint8_t timer, hits;       /* oni: charge timer (p_), ticks in contact (S6) */
  int16_t body;
  float x, y, vx, vy;        /* position (map) and velocity (px per tick) */
  float tx, ty;              /* teammates: pathfinder target */
  float p0x, p0y, p1x, p1y;  /* the path's first two points */
  uint8_t npath;             /* the path's length, up to 2 */
} Actor;

typedef struct {
  NodeId root, map, hud, hud_text, hud_dist, hud_arrows, ball;
  NodeId proto[PROTO_MAX];   /* one node per kind of rock and background tile */
  uint16_t proto_sym[PROTO_MAX];
  int16_t proto_nb[PROTO_MAX][4];
  int nproto, proto_last;
  int bg_slot;
  Actor act[ACT_MAX];
  int nact;
  int8_t order[ACT_MAX];     /* actors in the map's order (by y) */
  /* rocks: bounds in 1/4 px, their map slot, their body */
  int16_t ob_r[OB_MAX][4];
  uint8_t ob_slot[OB_MAX];
  int8_t ob_body[OB_MAX];
  uint8_t ob_vis[(OB_MAX + 7) / 8];
  int nob;
  uint8_t slot_ob[256];      /* map slot -> rock + 1 */
  /* the pathMap (Si): a grid of 10 px cells, 1 = free */
  Rect pa;
  int gw, gh;
  uint8_t grid[GRID_BYTES];
  Rect end_r;                /* the end zone's bounds */
  /* the player component (oj), on the carrier */
  int pl;
  uint32_t tick;
  uint8_t power;             /* sB */
  int power_t;               /* V_ */
  bool has_nc, ball_held, tackled;
  float ncx, ncy, distance, ax, ay, r0;
  int8_t above[4], below[4]; /* iJ, FC */
  int nabove, nbelow, score, eaten;
  /* the ball in flight (rj) */
  bool ball_nc;
  float bsx, bsy, bz;
  int8_t bdir;
  NodeId fx[FX_MAX];
  /* camera */
  float cam_tx, cam_ty, cam_ease, cam_speed;
  Rect view;
  /* flow */
  Countdown cd;
  bool started, ended;
  int tile_t, end_t, win_t, rating;
  uint32_t vis_t;
  char score_buf[16], dist_buf[32];
} State;

static State *S;

/* ---------------------------------------------------------------- timeline keys (the format node.c reads) */
typedef struct {
  uint16_t start;
  uint8_t flags, kind;
  uint16_t ref;
  float x, y;
  int16_t rx4, ry4;
  uint16_t mat, name;
  uint8_t alpha;
} Key;

static const uint8_t *rd_varint(const uint8_t *p, unsigned *v) {
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

static const uint8_t *rd_key(const uint8_t *p, Key *k) {
  unsigned start;
  p = rd_varint(p, &start);
  k->start = (uint16_t)start;
  k->flags = *p++;
  k->kind = CK_SYM;
  k->ref = NONE16;
  k->x = k->y = 0;
  k->rx4 = k->ry4 = 0;
  k->mat = 0;
  k->alpha = 255;
  k->name = NONE16;
  if (k->flags & K_ABSENT) return p;
  if (k->flags & K_KIND) { k->kind = *p++; k->ref = rd16(p); p += 2; }
  else { k->ref = rd16(p); p += 2; }
  k->x = rds16(p) * 0.25f;
  k->y = rds16(p + 2) * 0.25f;
  p += 4;
  if (k->flags & K_MAT) { k->mat = rd16(p); p += 2; }
  if (k->flags & K_REG) { k->rx4 = rds16(p); k->ry4 = rds16(p + 2); p += 4; }
  if (k->flags & K_ALPHA) k->alpha = *p++;
  if (k->flags & K_CLIP) p += 4;
  if (k->flags & K_NAME) { k->name = rd16(p); p += 2; }
  return p;
}

/* the slots of a clip at a frame, one after the other */
typedef struct { const uint8_t *p; unsigned n, i, frame; } Slots;
static bool slots_begin(Slots *it, uint16_t sym, unsigned frame) {
  Clip c;
  it->n = it->i = 0;
  if (!clip_get(sym, &c)) return false;
  it->p = c.slots;
  it->n = c.nslots;
  it->frame = frame;
  return true;
}
/* the key a slot shows now; false when the clip has no more slots */
static bool slots_next(Slots *it, Key *out, bool *present) {
  if (it->i >= it->n) return false;
  unsigned nk;
  it->p = rd_varint(it->p, &nk);
  *present = false;
  for (unsigned i = 0; i < nk; i++) {
    Key k;
    it->p = rd_key(it->p, &k);
    if (k.start <= it->frame) { *out = k; *present = !(k.flags & K_ABSENT); }
  }
  it->i++;
  return true;
}

static bool slot_key(uint16_t sym, unsigned slot, Key *k) {
  Slots it;
  bool pr;
  if (!slots_begin(&it, sym, 0)) return false;
  while (slots_next(&it, k, &pr))
    if (it.i - 1 == slot) return pr;
  return false;
}

static Mat key_mat(const Key *k) {
  float a = 1, b = 0, c = 0, d = 1;
  if (k->mat) { const float *m = mat(k->mat); a = m[0]; b = m[1]; c = m[2]; d = m[3]; }
  float rx = k->rx4 * 0.25f, ry = k->ry4 * 0.25f;
  return (Mat){a, b, c, d, k->x - (rx * a + ry * c), k->y - (rx * b + ry * d)};
}

/* a node takes a key's transform, like a timeline child */
static void place(NodeId n, const Key *k) {
  Node *p = &nodes[n];
  p->x = k->x;
  p->y = k->y;
  p->rx4 = k->rx4;
  p->ry4 = k->ry4;
  p->mat = k->mat;
  p->alpha = k->alpha;
  if (k->name != NONE16) p->name = k->name;
  if (k->flags & K_HIDDEN) p->flags &= (uint8_t)~NF_VISIBLE;
  else p->flags |= NF_VISIBLE;
}

/* how many things a clip shows at its frame (CreateJS children.length) */
static int shown(NodeId n) {
  Slots it;
  Key k;
  bool pr;
  int c = 0;
  if (!slots_begin(&it, nodes[n].sym, nodes[n].frame)) return 0;
  while (slots_next(&it, &k, &pr)) c += pr;
  return c;
}

/* ---------------------------------------------------------------- helpers */
static bool has(NodeId n, int c) { return n && nodes[n].T != NONE16 && comp_has(nodes[n].T, c); }
static float jround(float v) { return floorf(v + 0.5f); }     /* Math.round */
static float len2(float x, float y) { return sqrtf(x * x + y * y); }
/* createjs Rectangle.intersects: edges touch */
static bool touch(Rect a, Rect b) { return b.x <= a.x + a.w && a.x <= b.x + b.w && b.y <= a.y + a.h && a.y <= b.y + b.h; }
static int frame_of(uint16_t sym, const char *label) {
  Clip c;
  return clip_get(sym, &c) ? clip_label(&c, label) : -1;
}
static float angle_of(float x, float y) {   /* hh */
  float a = fmodf(atan2f(y, x) * (180.0f / 3.14159265f) + 360.0f, 360.0f);
  return a;
}
static float angle_diff(float a, float b) {  /* ih */
  float d = b - a;
  if (d > 180) d -= 360;
  else if (d < -180) d += 360;
  return d;
}

/* Kj(e, label, keep) for a clip built whole (sprites): gotoAndStop, the pose
 * keeping its frame when the clip shows just that one child */
static void goto_label(NodeId e, const char *label, bool keep) {
  if (!e || nodes[e].kind != NK_CLIP) return;
  int f = frame_of(nodes[e].sym, label);
  if (f < 0) return;
  if (!strcmp(node_label(e), label)) { node_stop(e); return; }
  NodeId old = 0;
  int of = 0, ot = 0;
  bool paused = false;
  if (keep && shown(e) == 1) {
    old = node_onstage_child(e);
    if (old) { of = nodes[old].frame; ot = node_frames(old); paused = !(nodes[old].flags & NF_PLAYING); }
  }
  node_goto(e, NULL, f, false);
  node_update_one(e);
  if (old) {
    NodeId now = node_onstage_child(e);
    if (now && now != old && node_frames(now) == ot) {
      nodes[now].frame = (uint16_t)of;
      if (paused) node_stop(now); else node_play(now);
    }
  }
}

/* Lj */
static void dir_label(NodeId e, int d) {
  if (d < 0) return;
  const char *l = dir_names[d];
  if (!node_has_label(e, l)) {
    if (d == DIR_NE || d == DIR_SE) l = "e";
    else if (d == DIR_NW || d == DIR_SW) l = "w";
    else return;
    if (!node_has_label(e, l)) return;
  }
  goto_label(e, l, true);
}

/* a clip built lazily shows a frame: only that frame's children exist */
static void lazy_frame(NodeId n, int f) {
  node_goto(n, NULL, f, false);
  node_stream(n, -1e9f, -1e9f, 1e9f, 1e9f);
}

/* Nj(e, label) on a clip built lazily */
static void lazy_label(NodeId n, const char *label) {
  if (!n) return;
  if (!strcmp(node_label(n), label)) { node_stop(n); return; }
  int f = frame_of(nodes[n].sym, label);
  if (f >= 0) lazy_frame(n, f);
}

/* Nj(e, label) on an actor, with or without its node */
static void act_label(Actor *a, const char *label) {
  int f = frame_of(a->sym, label);
  if (f < 0) return;
  if (a->n) {
    if (!strcmp(node_label(a->n), label)) { node_stop(a->n); return; }
    lazy_frame(a->n, f);
  }
  a->frame = (uint8_t)f;
}

static void act_pos(Actor *a, float *x, float *y) {
  if (a->n) ent_pos(a->n, &a->x, &a->y);
  *x = a->x;
  *y = a->y;
}

static void act_set_pos(Actor *a, float x, float y) {
  a->x = x;
  a->y = y;
  if (a->n) ent_set_pos(a->n, x, y);
}

/* createjs getBounds(): the symbol's nominal bounds, or the union of what its
 * timeline shows (the doodle's libraries have no nominal bounds) */
static bool sym_bounds(uint16_t sym, unsigned frame, int depth, Rect *r) {
  SymInfo si;
  if (sym >= SYM_COUNT || depth > 8) return false;
  sym_info(sym, &si);
  if (si.type == SYM_BITMAP) {
    Sprite sp;
    sprite_info((uint16_t)si.v, &sp);
    *r = (Rect){0, 0, sp.w, sp.h};
    return true;
  }
  Clip c;
  if (si.type != SYM_CLIP || !clip_get(sym, &c)) return false;
  if (c.nb[2] || c.nb[3]) { *r = (Rect){c.nb[0], c.nb[1], c.nb[2], c.nb[3]}; return true; }
  float lx = 1e9f, ly = 1e9f, hx = -1e9f, hy = -1e9f;
  Slots it;
  Key k;
  bool pr;
  slots_begin(&it, sym, frame);
  while (slots_next(&it, &k, &pr)) {
    Rect b;
    if (!pr || (k.flags & K_HIDDEN) || k.kind != CK_SYM || !sym_bounds(k.ref, 0, depth + 1, &b)) continue;
    Mat m = key_mat(&k);
    float xs[4] = {b.x, b.x + b.w, b.x, b.x + b.w}, ys[4] = {b.y, b.y, b.y + b.h, b.y + b.h};
    for (int i = 0; i < 4; i++) {
      float X = m.a * xs[i] + m.c * ys[i] + m.tx, Y = m.b * xs[i] + m.d * ys[i] + m.ty;
      lx = fminf(lx, X); hx = fmaxf(hx, X); ly = fminf(ly, Y); hy = fmaxf(hy, Y);
    }
  }
  if (lx > hx) return false;
  *r = (Rect){lx, ly, hx - lx, hy - ly};
  return true;
}

/* Gj: the transformed bounds of e's "bounds" child, taken to the map */
static bool bounds_of(NodeId e, Rect *r) {
  for (NodeId c = nodes[e].first; c; c = nodes[c].next) {
    Rect b;
    if (!has(c, C_bounds)) continue;
    if (!sym_bounds(nodes[c].sym, nodes[c].frame, 0, &b)) return false;
    Mat m = node_local(c);
    float xs[4] = {b.x, b.x + b.w, b.x, b.x + b.w}, ys[4] = {b.y, b.y, b.y + b.h, b.y + b.h};
    float lx = 1e9f, ly = 1e9f, hx = -1e9f, hy = -1e9f;
    for (int i = 0; i < 4; i++) {
      float X = m.a * xs[i] + m.c * ys[i] + m.tx, Y = m.b * xs[i] + m.d * ys[i] + m.ty;
      lx = fminf(lx, X); hx = fmaxf(hx, X); ly = fminf(ly, Y); hy = fmaxf(hy, Y);
    }
    Mat t = node_to(e, S->map);
    float x0 = t.a * lx + t.c * ly + t.tx, y0 = t.b * lx + t.d * ly + t.ty;
    float x1 = t.a * hx + t.c * hy + t.tx, y1 = t.b * hx + t.d * hy + t.ty;
    *r = (Rect){fminf(x0, x1), fminf(y0, y1), fabsf(x1 - x0), fabsf(y1 - y0)};
    return true;
  }
  return false;
}

static bool act_bounds(Actor *a, Rect *r) { return a->n && bounds_of(a->n, r); }

/* the sprites of an actor follow its direction (aq) */
static void act_dirs(NodeId n, int d) {
  if (!n || d < 0) return;
  for (NodeId c = nodes[n].first; c; c = nodes[c].next)
    if ((nodes[c].flags & (NF_ONSTAGE | NF_VISIBLE)) == (NF_ONSTAGE | NF_VISIBLE) && has(c, C_sprite) &&
        node_has_label(c, "n") && node_has_label(c, "s") && node_has_label(c, "e") && node_has_label(c, "w"))
      dir_label(c, d);
}

static void cursor(NodeId n, const char *name, bool on) {
  NodeId c = n ? node_child(n, "cursors") : 0;
  NodeId k = c ? node_child(c, name) : 0;
  if (k) node_set_visible(k, on);
}

/* ---------------------------------------------------------------- actors with and without nodes */
static void materialize(Actor *a) {
  if (a->n || (a->flags & AF_GONE)) return;
  Key k;
  if (!slot_key(nodes[S->map].sym, a->slot, &k)) return;
  NodeId n = node_new_sym_lazy(a->sym);
  if (!n) return;
  node_add(S->map, n);
  place(n, &k);
  lazy_frame(n, a->frame);
  if (a->kind == A_END) node_play(n);   /* the end zone's glow: nothing stops it in the doodle */
  a->n = n;
  ent_set_pos(n, a->x, a->y);
  act_dirs(n, a->dir);
}

static void dematerialize(Actor *a) {
  if (!a->n) return;
  ent_pos(a->n, &a->x, &a->y);
  a->frame = (uint8_t)nodes[a->n].frame;
  if (a->body >= 0) phys_remove(a->body);
  a->body = -1;
  node_free(a->n);
  a->n = 0;
}

static void act_remove(Actor *a) {   /* co, with removeFx (Yp) */
  if (a->n && has(a->n, C_removeFx)) {
    uint16_t s = comp_sym(nodes[a->n].T, C_removeFx, F_mc);
    NodeId fx = s != NONE16 ? node_new_sym(s) : 0;
    int i = 0;
    while (i < FX_MAX && S->fx[i]) i++;
    if (fx && i < FX_MAX) {
      float x, y;
      ent_local_pos(a->n, &x, &y);
      node_add(S->map, fx);
      ent_set_local_pos(fx, x, y);
      S->fx[i] = fx;
    } else if (fx) node_free(fx);
  }
  dematerialize(a);
  a->flags |= AF_GONE;
}

/* ---------------------------------------------------------------- setting up */
static int proto_of(uint16_t sym) {
  if (S->proto_last < S->nproto && S->proto_sym[S->proto_last] == sym) return S->proto_last;   /* runs of one kind */
  for (int i = 0; i < S->nproto; i++)
    if (S->proto_sym[i] == sym) return S->proto_last = i;
  if (S->nproto >= PROTO_MAX) return -1;
  NodeId n = node_new_sym(sym);
  if (!n) return -1;
  int i = S->nproto++;
  S->proto[i] = n;
  S->proto_sym[i] = sym;
  Rect b;
  if (sym_bounds(sym, 0, 0, &b)) {
    S->proto_nb[i][0] = (int16_t)floorf(b.x);
    S->proto_nb[i][1] = (int16_t)floorf(b.y);
    S->proto_nb[i][2] = (int16_t)ceilf(b.w + 1);
    S->proto_nb[i][3] = (int16_t)ceilf(b.h + 1);
  }
  return i;
}

/* bounds (in the map) of a symbol placed by a key, through a node made for it */
static bool bounds_via(NodeId n, const Key *k, Rect *r) {
  node_add(S->map, n);
  place(n, k);
  bool ok = bounds_of(n, r);
  node_remove(n);
  return ok;
}

static void set_grid(int x, int y, bool free_) {
  int i = x * S->gh + y;
  if (i < 0 || i >= GRID_BYTES * 8) return;
  if (free_) S->grid[i >> 3] |= (uint8_t)(1 << (i & 7));
  else S->grid[i >> 3] &= (uint8_t)~(1 << (i & 7));
}
static bool grid_free(int x, int y) {
  int i = x * S->gh + y;
  return i >= 0 && i < GRID_BYTES * 8 && (S->grid[i >> 3] >> (i & 7) & 1);
}
/* Mi: the cell of a point */
static void cell_of(float x, float y, int *cx, int *cy) {
  *cx = (int)jround((x - S->pa.x) / S->pa.w * S->gw);
  *cy = (int)jround((y - S->pa.y) / S->pa.h * S->gh);
}

static void build_grid(void) {
  /* DN: every cell is free but those under a prop's bounds grown by 2 */
  S->gw = (int)jround(S->pa.w / 10);
  S->gh = (int)jround(S->pa.h / 10);
  if (S->gw < 1 || S->gh < 1 || S->gw > 255 || S->gh > 255 || S->gw * S->gh > GRID_BYTES * 8) { S->gw = S->gh = 0; return; }
  memset(S->grid, 0xFF, sizeof S->grid);
  for (int i = 0; i < S->nob; i++) {
    float x = S->ob_r[i][0] * .25f - 2, y = S->ob_r[i][1] * .25f - 2, w = S->ob_r[i][2] * .25f + 4, h = S->ob_r[i][3] * .25f + 4;
    int x0, y0, x1, y1;
    cell_of(x, y, &x0, &y0);
    cell_of(x + w, y + h, &x1, &y1);
    for (int cx = x0; cx <= x1; cx++)
      for (int cy = y0; cy <= y1; cy++)
        if (cx >= 0 && cx < S->gw && cy >= 0 && cy < S->gh) set_grid(cx, cy, false);
  }
}

static void origin_of(const Key *k, float *x, float *y) {
  Mat m = key_mat(k);
  *x = m.tx;
  *y = m.ty;
}

static void start(void) {
  S = scene_state(sizeof(State));
  phys_reset();
  /* Ft: the body types of this field */
  phys_body_type("character", "bodyMaterial", 10, 4, 3, .97f);
  phys_body_type("prop", "bodyMaterial", 0, 2, 20, 0);
  phys_body_type("enemy", "bodyMaterial", 40, 16, 19, .5f);
  S->bg_slot = -1;
  S->pl = -1;
  S->ax = 0;
  S->ay = 1;        /* Aaa */
  S->ball_held = true;   /* LT */
  for (int i = 0; i < OB_MAX; i++) S->ob_body[i] = -1;
  memset(S->ob_vis, 0xFF, sizeof S->ob_vis);

  S->root = node_new_sym_lazy(S_rugby_vU);
  if (!S->root) return;
  node_add(game.root, S->root);
  Key k;
  if (!slot_key(S_rugby_vU, 0, &k)) return;
  S->map = node_new_sym_lazy(k.ref);
  if (!S->map) return;
  node_add(S->root, S->map);
  place(S->map, &k);
  nodes[S->map].alpha = 0;      /* drawn by draw_under, in the doodle's order */
  S->cam_ease = comp_float(nodes[S->map].T, C_camera, F_ease, .6f);
  S->cam_speed = comp_float(nodes[S->map].T, C_camera, F_speed, 60);
  if (slot_key(S_rugby_vU, 1, &k)) {
    S->hud = node_new_sym(k.ref);
    if (S->hud) {
      node_add(S->root, S->hud);
      place(S->hud, &k);
      S->hud_text = node_find(S->hud, "text");
      S->hud_dist = node_find(S->hud, "distance");
      S->hud_arrows = node_find(S->hud, "arrows");
    }
  }

  /* the map's children */
  Slots it;
  bool pr;
  memset(S->slot_ob, 0, sizeof S->slot_ob);
  slots_begin(&it, nodes[S->map].sym, 0);
  while (slots_next(&it, &k, &pr)) {
    unsigned slot = it.i - 1;
    if (!pr || k.kind != CK_SYM || slot > 255) continue;
    Clip c;
    if (!clip_get(k.ref, &c) || c.T == NONE16) continue;
    uint16_t T = c.T;
    float ox, oy;
    origin_of(&k, &ox, &oy);
    if (comp_has(T, C_tileBackground)) { S->bg_slot = (int)slot; continue; }
    if (comp_has(T, C_pathMap)) {
      NodeId n = node_new_sym(k.ref);
      if (n) { bounds_via(n, &k, &S->pa); node_free(n); }
      continue;
    }
    int kind = -1, type = 0;
    if (comp_has(T, C_rugbyPlayer) || comp_has(T, C_rugbyAlly)) kind = A_CHAR;
    else if (comp_has(T, C_rugbyEnemy)) {
      kind = A_ENEMY;
      uint16_t s = comp_str(T, C_rugbyEnemy, F_type);
      const char *t = s != NONE16 ? str(s) : "";
      type = !strcmp(t, "blue") ? EN_BLUE : !strcmp(t, "charge") ? EN_CHARGE : !strcmp(t, "heavy") ? EN_HEAVY : EN_RED;
    } else if (comp_has(T, C_rugbyPowerup)) {
      kind = A_POWER;
      uint16_t s = comp_str(T, C_rugbyPowerup, F_name);
      const char *t = s != NONE16 ? str(s) : "";
      type = !strcmp(t, "speed") ? PW_SPEED : !strcmp(t, "pacman") ? PW_PACMAN : PW_NOCLIP;
    } else if (comp_has(T, C_rugbyEnd)) {
      kind = A_END;
      NodeId n = node_new_sym(k.ref);
      if (n) { bounds_via(n, &k, &S->end_r); node_free(n); }
    } else if (comp_has(T, C_boundable) && comp_has(T, C_collidable) && S->nob < OB_MAX) {
      int p = proto_of(k.ref);
      Rect r;
      if (p < 0 || !bounds_via(S->proto[p], &k, &r)) continue;
      int i = S->nob++;
      S->ob_r[i][0] = (int16_t)floorf(r.x * 4);
      S->ob_r[i][1] = (int16_t)floorf(r.y * 4);
      S->ob_r[i][2] = (int16_t)ceilf(r.w * 4);
      S->ob_r[i][3] = (int16_t)ceilf(r.h * 4);
      S->ob_slot[i] = (uint8_t)slot;
      S->slot_ob[slot] = (uint8_t)(i + 1);
      continue;
    }
    if (kind < 0 || S->nact >= ACT_MAX) continue;
    Actor *a = &S->act[S->nact];
    memset(a, 0, sizeof *a);
    a->sym = k.ref;
    a->slot = (uint8_t)slot;
    a->kind = (uint8_t)kind;
    a->type = (uint8_t)type;
    a->flags = AF_VIS | AF_PF;
    a->body = -1;
    a->x = ox;
    a->y = oy;
    a->dir = DIR_S;
    if (comp_has(T, C_direction)) {
      uint16_t d = comp_str(T, C_direction, F_direction);
      a->dir = (int8_t)(d != NONE16 ? dir_parse(str(d)) : DIR_S);
    }
    if (kind == A_CHAR) {
      if (comp_has(T, C_rugbyPlayer)) S->pl = S->nact;
      else a->flags |= AF_ALLY;
    }
    S->order[S->nact] = (int8_t)S->nact;
    S->nact++;
  }
  build_grid();

  /* the camera's view at the start (Ls is off during the countdown: what is near shows) */
  {
    Mat g = node_global(S->map);
    float det = g.a * g.d - g.b * g.c;
    if (fabsf(det) > 1e-9f) {
      float x0 = (g.d * -g.tx + g.c * g.ty) / det, y0 = (g.b * g.tx - g.a * g.ty) / det;
      S->view = (Rect){x0, y0, 960 / g.a, 540 / g.d};
    }
  }
  for (int i = 0; i < S->nact; i++) {
    Actor *a = &S->act[i];
    Rect v = rect_pad(S->view, 240, 216, 240, 432);
    if (a->kind == A_CHAR || rect_contains(v, a->x, a->y)) materialize(a);
    else a->flags &= (uint8_t)~AF_VIS;
  }
  sys_countdown_start(&S->cd);
}

/* ---------------------------------------------------------------- Ls: what is near the camera shows */
static void visibility(void) {
  Rect v = rect_pad(S->view, 240, 216, 240, 432);
  uint32_t t = S->vis_t++;
  for (int i = 0; i < S->nact; i++) {
    Actor *a = &S->act[i];
    if (a->flags & AF_GONE || (uint32_t)(i + 1) % 4 != t % 4) continue;
    float x, y;
    act_pos(a, &x, &y);
    bool vis = rect_contains(v, x, y);
    if (vis) a->flags |= AF_VIS; else a->flags &= (uint8_t)~AF_VIS;
    if (a->kind == A_CHAR) {
      if (a->n) node_set_visible(a->n, vis);
      continue;
    }
    if (vis) materialize(a); else dematerialize(a);
  }
  for (int i = 0; i < S->nob; i++) {
    if ((uint32_t)(i + 3) % 4 != t % 4) continue;
    float x = S->ob_r[i][0] * .25f + S->ob_r[i][2] * .125f, y = S->ob_r[i][1] * .25f + S->ob_r[i][3] * .125f;
    if (rect_contains(v, x, y)) S->ob_vis[i >> 3] |= (uint8_t)(1 << (i & 7));
    else S->ob_vis[i >> 3] &= (uint8_t)~(1 << (i & 7));
  }
}

/* Pp: the map's order, by y (what does not show first) */
static float order_key(int i) {
  Actor *a = &S->act[i];
  if (!(a->flags & AF_VIS) || !a->n) return -1e15f;
  float x, y;
  act_pos(a, &x, &y);
  return y;
}

static void sort_order(void) {
  float keys[ACT_MAX];
  for (int i = 0; i < S->nact; i++) keys[S->order[i]] = order_key(S->order[i]);
  for (int i = 1; i < S->nact; i++)
    for (int j = i; j > 0 && keys[S->order[j - 1]] > keys[S->order[j]]; j--) {
      int8_t t = S->order[j];
      S->order[j] = S->order[j - 1];
      S->order[j - 1] = t;
    }
}

/* ---------------------------------------------------------------- Vp: effects that end */
static void fx_tick(void) {
  for (int i = 0; i < FX_MAX; i++) {
    NodeId n = S->fx[i];
    if (!n) continue;
    int df = comp_int(nodes[n].T, C_ephemeral, F_deathFrame, -1);
    if (nodes[n].frame == (df >= 0 ? df : node_frames(n) - 1)) { node_free(n); S->fx[i] = 0; }
  }
}

/* ---------------------------------------------------------------- the carrier (wt) and the pass (DJ) */
static void remove_from(int8_t *list, int *n, int who) {
  for (int i = 0; i < *n; i++)
    if (list[i] == who) {
      for (int j = i; j + 1 < *n; j++) list[j] = list[j + 1];
      (*n)--;
      return;
    }
}

static void pass(int to) {
  Actor *b = &S->act[S->pl], *g = &S->act[to];
  float cx, cy;
  act_pos(b, &cx, &cy);
  g->flags &= (uint8_t)~AF_ALLY;
  b->flags |= AF_ALLY;
  S->pl = to;
  S->ball_held = false;
  remove_from(S->below, &S->nbelow, to);
  remove_from(S->above, &S->nabove, to);
  /* the ball flies from the passer */
  if (S->ball) node_free(S->ball);
  S->ball = node_new_sym_lazy(S_rugby_H0a);
  if (S->ball) {
    node_add(S->map, S->ball);
    lazy_frame(S->ball, 0);
    ent_set_pos(S->ball, cx, cy);
    S->ball_nc = false;
    S->bz = 0;
    S->bdir = DIR_S;
  }
  SOUND(d_a);
}

static void player_tick(void) {
  if (S->pl < 0) return;
  Actor *b = &S->act[S->pl];
  S->tick++;
  b->flags &= (uint8_t)~AF_PF;
  if (S->power_t > 0 && --S->power_t == 0) S->power = PW_NONE;
  float mx, my;
  act_pos(b, &mx, &my);
  if (!S->has_nc) { S->has_nc = true; S->ncx = mx; S->ncy = my; }
  if (mx - S->ncx > S->distance) S->distance = mx - S->ncx;
  float cx = in.jx, cy = in.jy, cl = len2(cx, cy);
  if (cl > 0) {
    float sp = S->power == PW_SPEED ? 4 : 2.5f;
    b->vx = cx / cl * sp;
    b->vy = cy / cl * sp;
    b->dir = (int8_t)dir_of(cx, cy, false);
  } else b->vx = b->vy = 0;
  char label[16];
  snprintf(label, sizeof label, "%s", len2(b->vx, b->vy) > 0 ? "walk" : "idle");
  if (S->ball_held)
    strcat(label, S->power == PW_SPEED ? "Speed" : S->power == PW_PACMAN ? "Pacman" : S->power == PW_NOCLIP ? "Noclip" : "Ball");
  act_label(b, label);
  if (b->body >= 0) bodies[b->body].mask = S->ball_held && S->power == PW_NOCLIP ? 1 : 3;
  cursor(b->n, "pass", false);
  cursor(b->n, "user", true);
  cursor(b->n, "ally", false);
  for (int i = 0; i < S->nact; i++)
    if (S->act[i].kind == A_CHAR && (S->act[i].flags & AF_ALLY)) cursor(S->act[i].n, "pass", false);
  if (!S->ball_held) return;
  if (cy != 0) { S->ax = cx; S->ay = cy; }
  /* teammates behind within 110, nearest first */
  int cand[ACT_MAX], nc = 0;
  float dist[ACT_MAX];
  for (int q = 0; q < S->nact; q++) {
    int i = S->order[q];
    Actor *a = &S->act[i];
    if (a->kind != A_CHAR || !(a->flags & AF_ALLY)) continue;
    float x, y;
    act_pos(a, &x, &y);
    float dx = x - mx, dy = y - my, d = len2(dx, dy);
    if (d < 110 && dx <= 0) {
      int j = nc++;
      while (j > 0 && dist[j - 1] > d) { cand[j] = cand[j - 1]; dist[j] = dist[j - 1]; j--; }
      cand[j] = i;
      dist[j] = d;
    }
  }
  int n = nc ? cand[0] : -1;
  /* on the side last pressed */
  int k = 0;
  for (int q = 0; q < nc; q++) {
    float x, y;
    act_pos(&S->act[cand[q]], &x, &y);
    if (S->ay > 0 ? y > my : y < my) cand[k++] = cand[q];
  }
  nc = k;
  if (nc) n = cand[0];
  /* pressing left: the one that way */
  if (cx < 0) {
    float a = dir_angle(b->dir);
    k = 0;
    for (int q = 0; q < nc; q++) {
      float x, y;
      act_pos(&S->act[cand[q]], &x, &y);
      float dx = x - mx, dy = y - my;
      float d = (dx == 0 && dy == 0) ? NAN : angle_diff(a, angle_of(dx, dy));
      if (fabsf(d) < 70) cand[k++] = cand[q];
    }
    nc = k;
    if (nc) n = cand[0];
  }
  if (n >= 0) {
    cursor(S->act[n].n, "pass", true);
    if (in.pressed[A_ACTION]) pass(n);
  }
}

/* ---------------------------------------------------------------- the ball in flight (xt) */
static void ball_tick(void) {
  if (!S->ball || S->pl < 0) return;
  Actor *g = &S->act[S->pl];
  float kx, ky, bx, by;
  act_pos(g, &kx, &ky);
  ent_pos(S->ball, &bx, &by);
  if (!S->ball_nc) { S->ball_nc = true; S->bsx = bx; S->bsy = by; }
  float nx = kx - S->bsx, ny = ky - S->bsy, n = len2(nx, ny);
  float cx = kx - bx, cy = ky - by;
  float h = n - len2(cx, cy) + 7;
  int d = dir_of(cx, cy, false);
  if (d >= 0) S->bdir = (int8_t)d;
  float c = n > 0 ? h / n : INFINITY;
  const char *l = S->power == PW_SPEED ? "speed" : S->power == PW_PACMAN ? "pacman" : S->power == PW_NOCLIP ? "noclip" : "normal";
  lazy_label(S->ball, l);
  if (c > 1) {
    S->ball_held = true;
    node_free(S->ball);
    S->ball = 0;
    SOUND(a_a);
  } else {
    ent_set_pos(S->ball, S->bsx + nx * c, S->bsy + ny * c);
    S->bz = 8 + sinf(c * 3.14159265f) * n / 10;
  }
}

/* ---------------------------------------------------------------- powerups (Dt) */
static void powerups(void) {
  if (S->pl < 0) return;
  float mx, my;
  act_pos(&S->act[S->pl], &mx, &my);
  for (int q = 0; q < S->nact; q++) {
    Actor *k = &S->act[S->order[q]];
    if (k->kind != A_POWER || (k->flags & AF_GONE)) continue;
    float x, y;
    act_pos(k, &x, &y);
    if (len2(x - mx, y - my) < 11) {
      S->power = k->type;
      S->power_t = 150;
      act_remove(k);
      SOUND(c_a);
    }
  }
}

/* ---------------------------------------------------------------- oni (zt) */
static void enemies(void) {
  if (S->pl < 0) return;
  float mx, my;
  act_pos(&S->act[S->pl], &mx, &my);
  for (int q = 0; q < S->nact; q++) {
    Actor *k = &S->act[S->order[q]];
    if (k->kind != A_ENEMY || (k->flags & AF_GONE)) continue;
    float nx, ny;
    act_pos(k, &nx, &ny);
    float dx = mx - nx, dy = my - ny, d = len2(dx, dy);
    int t = k->type;
    float sp = en_speed[t];
    if (t == EN_CHARGE) {
      if (k->timer == 0) {
        k->timer = 60;
        if (d < EN_RANGE) {
          SOUND(b_a);
          k->vx = d > 0 ? dx / d * sp : 0;
          k->vy = d > 0 ? dy / d * sp : 0;
          act_label(k, "walk");
        }
      } else {
        k->timer--;
        if (k->timer < 30) {
          k->vx *= .9f;
          k->vy *= .9f;
          act_label(k, "idle");
        }
      }
    } else {
      float cx = 0, cy = 0;
      if (d < EN_RANGE) {
        if (d > 0) { cx = dx / d * sp; cy = dy / d * sp; }
        if (S->power == PW_PACMAN) { cx = -cx; cy = -cy; act_label(k, "scared"); }
        else act_label(k, "walk");
      }
      float vl = len2(k->vx, k->vy), cl = len2(cx, cy);
      if (vl > 0 && cl > 0) {
        /* they turn at most vN degrees a tick, and speed up by a fifth of the way */
        float cur = angle_of(k->vx, k->vy), diff = angle_diff(cur, angle_of(cx, cy)), a = cur, turn = en_turn[t];
        if (diff > turn) a += turn;
        else if (diff < -turn) a -= turn;
        a = fmodf(a, 360) * (3.14159265f / 180);
        float s = vl + (sp - vl) * .2f;
        cx = cosf(a) * s;
        cy = sinf(a) * s;
      }
      k->vx = cx;
      k->vy = cy;
    }
    k->dir = (int8_t)dir_of(k->vx, k->vy, false);
  }
}

/* ---------------------------------------------------------------- teammates (yt) */
static void allies(void) {
  if (S->pl < 0) return;
  Actor *b = &S->act[S->pl];
  float mx, my;
  act_pos(b, &mx, &my);
  for (int q = 0; q < S->nact; q++) {
    int i = S->order[q];
    Actor *a = &S->act[i];
    if (a->kind != A_CHAR || !(a->flags & AF_ALLY)) continue;
    float nx, ny;
    act_pos(a, &nx, &ny);
    cursor(a->n, "user", false);
    cursor(a->n, "ally", true);
    if (nx - 100 > mx) { a->flags &= (uint8_t)~AF_PF; continue; }
    a->flags |= AF_PF;
    int ia = -1, ib = -1;
    for (int j = 0; j < S->nabove; j++) if (S->above[j] == i) ia = j;
    for (int j = 0; j < S->nbelow; j++) if (S->below[j] == i) ib = j;
    if (ia < 0 && ib < 0) {
      /* joins the line: above or below the carrier, by y */
      int e[12], ne = 0;
      float ey[12];
      e[ne++] = i;
      for (int j = 0; j < S->nabove && ne < 11; j++) e[ne++] = S->above[j];
      for (int j = 0; j < S->nbelow && ne < 11; j++) e[ne++] = S->below[j];
      e[ne++] = S->pl;
      for (int j = 0; j < ne; j++) { float x; act_pos(&S->act[e[j]], &x, &ey[j]); }
      for (int j = 1; j < ne; j++)
        for (int l = j; l > 0 && ey[l - 1] > ey[l]; l--) {
          float ty = ey[l]; ey[l] = ey[l - 1]; ey[l - 1] = ty;
          int ti = e[l]; e[l] = e[l - 1]; e[l - 1] = ti;
        }
      int f = 0;
      while (f < ne && e[f] != S->pl) f++;
      S->nabove = S->nbelow = 0;
      for (int j = f - 1; j >= 0 && S->nabove < 4; j--) S->above[S->nabove++] = (int8_t)e[j];
      for (int j = f + 1; j < ne && S->nbelow < 4; j++) S->below[S->nbelow++] = (int8_t)e[j];
      for (int j = 0; j < S->nabove; j++) if (S->above[j] == i) ia = j;
      for (int j = 0; j < S->nbelow; j++) if (S->below[j] == i) ib = j;
    }
    if (ia >= 0) { a->tx = mx - 20 * (1 + ia); a->ty = my - 55 * (1 + ia); }
    else if (ib >= 0) { a->tx = mx - 20 * (1 + ib); a->ty = my + 55 * (1 + ib); }
    else continue;
    a->flags |= AF_TARGET;
    a->vx = a->vy = 0;
    if (a->npath > 0) {
      float e = S->power == PW_SPEED ? 4 : 2.5f;
      float fx = a->p0x, fy = a->p0y, vx = fx - nx, vy = fy - ny;
      if (len2(vx, vy) < e && a->npath > 1) { fx = a->p1x; fy = a->p1y; vx = fx - nx; vy = fy - ny; }
      float d = len2(vx, vy);
      if (d > e) {
        a->vx = vx / d * e;
        a->vy = vy / d * e;
        a->dir = (int8_t)dir_of(a->vx, a->vy, false);
      }
    }
    act_label(a, len2(a->vx, a->vy) > 0 ? "walk" : "idle");
  }
  if (S->nabove > 1 && S->nbelow < 2 && S->act[S->above[S->nabove - 1]].ty < S->pa.y) S->below[S->nbelow++] = S->above[--S->nabove];
  if (S->nbelow > 1 && S->nabove < 2 && S->act[S->below[S->nbelow - 1]].ty > S->pa.y + S->pa.h)
    S->above[S->nabove++] = S->below[--S->nbelow];
}

/* ---------------------------------------------------------------- tackles (Bt) */
static Rect act_rect(Actor *a) {
  Rect r;
  if (act_bounds(a, &r)) return r;
  float x, y;
  act_pos(a, &x, &y);
  return (Rect){x, y, 0, 0};
}

static void tackles(void) {
  if (S->pl < 0) return;
  Actor *b = &S->act[S->pl];
  Rect br = act_rect(b);
  for (int q = 0; q < S->nact; q++) {
    Actor *k = &S->act[S->order[q]];
    if (k->kind != A_ENEMY || (k->flags & AF_GONE)) continue;
    if (touch(act_rect(k), br)) {
      if (S->power == PW_PACMAN) {
        act_remove(k);
        S->eaten++;
      } else if (S->power != PW_NOCLIP) {
        if (k->hits < 255) k->hits++;
        act_label(k, "attack");
        if (k->hits == 6 && S->ball_held) {
          S->tackled = true;
          b->vx = b->vy = 0;
          act_label(b, "dead");
          k->vx = k->vy = 0;
          act_label(k, "ball");
        }
      }
    } else k->hits = 0;
  }
}

/* ---------------------------------------------------------------- the ball's height (dq) */
static void zsprite(void) {
  if (!S->ball) return;
  for (NodeId c = nodes[S->ball].first; c; c = nodes[c].next)
    if ((nodes[c].flags & (NF_ONSTAGE | NF_VISIBLE)) == (NF_ONSTAGE | NF_VISIBLE) && has(c, C_zSprite)) ent_set_local_pos(c, 0, -S->bz);
}

/* ---------------------------------------------------------------- physics (yr) */
static bool ob_visible(int i) { return S->ob_vis[i >> 3] >> (i & 7) & 1; }

static void physics(void) {
  Rect mv[ACT_MAX];
  int nmv = 0;
  int tc = phys_type("character"), te = phys_type("enemy"), tp = phys_type("prop");
  for (int q = 0; q < S->nact; q++) {
    Actor *a = &S->act[S->order[q]];
    if ((a->kind != A_CHAR && a->kind != A_ENEMY) || !a->n) continue;
    Rect r;
    if (!act_bounds(a, &r)) continue;
    bool vis = a->flags & AF_VIS;
    if (a->body < 0) {
      a->body = (int16_t)phys_add(a->kind == A_CHAR ? tc : te, SH_BOX, r.x + r.w / 2, r.y + r.h / 2, 50, r.w / 2, r.h / 2, 50);
      if (a->body >= 0) bodies[a->body].user = a->n;
    }
    if (a->body < 0) continue;
    Body *bd = &bodies[a->body];
    bd->active = vis;
    if (!vis) continue;
    bd->x = r.x + r.w / 2;
    bd->y = r.y + r.h / 2;
    bd->z = 50;
    bd->vx = 30 * a->vx;
    bd->vy = 30 * a->vy;
    bd->vz = 0;
    if (nmv < ACT_MAX) mv[nmv++] = rect_pad(r, 16, 16, 16, 16);
  }
  /* rocks: bodies only where something moves near them */
  for (int i = 0; i < S->nob; i++) {
    Rect r = {S->ob_r[i][0] * .25f, S->ob_r[i][1] * .25f, S->ob_r[i][2] * .25f, S->ob_r[i][3] * .25f};
    bool want = false;
    if (ob_visible(i))
      for (int k = 0; k < nmv && !want; k++) want = rect_intersects(r, mv[k]);
    if (want && S->ob_body[i] < 0) {
      int b = phys_add(tp, SH_BOX, r.x + r.w / 2, r.y + r.h / 2, 50, r.w / 2, r.h / 2, 50);
      S->ob_body[i] = (int8_t)b;
    } else if (!want && S->ob_body[i] >= 0) {
      phys_remove(S->ob_body[i]);
      S->ob_body[i] = -1;
    }
  }
  phys_step(1.0f / FPS);
  for (int q = 0; q < S->nact; q++) {
    Actor *a = &S->act[S->order[q]];
    if (a->body < 0 || !a->n) continue;
    Body *bd = &bodies[a->body];
    a->vx = bd->vx / 30;
    a->vy = bd->vy / 30;
    Rect r;
    if (!act_bounds(a, &r)) continue;
    float px, py;
    act_pos(a, &px, &py);
    act_set_pos(a, bd->x + (px - (r.x + r.w / 2)), bd->y + (py - (r.y + r.h / 2)));
  }
}

/* ---------------------------------------------------------------- the teammates' paths (Gt, Ht: A*, Pi) */
#define AS_MAX 320
#define AS_HASH 512
typedef struct { uint8_t x, y, a, b; int16_t parent; uint16_t h; uint8_t fl; } ANode;   /* g = a + b sqrt 2 */
enum { AN_OPEN = 1, AN_CLOSED = 2 };
typedef struct {
  ANode n[AS_MAX];
  int nn, nopen;
  int16_t hash[AS_HASH];
  int16_t open[AS_MAX];
} AStar;

static float an_g(const ANode *e) { return e->a + e->b * SQRT2; }
/* $a: f(p) - f(q) (0 exactly when the sums are the same) */
static int an_cmp(const ANode *p, const ANode *q) {
  if (p->b == q->b && p->a + p->h == q->a + q->h) return 0;
  return an_g(p) + p->h > an_g(q) + q->h ? 1 : -1;
}
/* Za: goog's binary search (the first with f >= e's, and whether it is equal) */
static int za(const AStar *s, const ANode *e, bool *found) {
  int k = 0, c = s->nopen;
  bool a = false;
  while (k < c) {
    int m = k + ((c - k) >> 1);
    int h = an_cmp(e, &s->n[s->open[m]]);
    if (h > 0) k = m + 1;
    else { c = m; a = h == 0; }
  }
  *found = a;
  return k;
}
static unsigned an_hash(int x, int y) { return ((unsigned)x * 73856093u ^ (unsigned)y * 19349663u) & (AS_HASH - 1); }
static int an_find(const AStar *s, int x, int y) {
  unsigned h = an_hash(x, y);
  for (int i = 0; i < AS_HASH; i++, h = (h + 1) & (AS_HASH - 1)) {
    int v = s->hash[h];
    if (v < 0) return -1;
    if (s->n[v].x == x && s->n[v].y == y) return v;
  }
  return -1;
}
static int an_new(AStar *s, int x, int y) {
  if (s->nn >= AS_MAX) return -1;
  unsigned h = an_hash(x, y);
  while (s->hash[h] >= 0) h = (h + 1) & (AS_HASH - 1);
  int i = s->nn++;
  s->hash[h] = (int16_t)i;
  s->n[i] = (ANode){(uint8_t)x, (uint8_t)y, 0, 0, -1, 0, 0};
  return i;
}
static uint16_t an_h(int x, int y, int gx, int gy) { return (uint16_t)((x - gx) * (x - gx) + (y - gy) * (y - gy)); }

/* Pi: the path to the goal, or towards it, in at most 100 steps; returns its length and first two cells */
static int astar(AStar *s, int sx, int sy, int gx, int gy, int *c0x, int *c0y, int *c1x, int *c1y) {
  static const int8_t nb[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
  memset(s->hash, 0xFF, sizeof s->hash);
  s->nn = s->nopen = 0;
  int st = an_new(s, sx, sy);
  s->n[st].h = an_h(sx, sy, gx, gy);
  s->open[s->nopen++] = (int16_t)st;
  int best = st;
  for (int it = 0; s->nopen > 0 && it <= 100;) {
    it++;
    int ci = s->open[0];
    memmove(s->open, s->open + 1, (size_t)(s->nopen - 1) * sizeof s->open[0]);
    s->nopen--;
    if (s->n[ci].x == gx && s->n[ci].y == gy) { best = ci; break; }
    s->n[ci].fl |= AN_CLOSED;
    for (int q = 0; q < 8; q++) {
      int x = s->n[ci].x + nb[q][0], y = s->n[ci].y + nb[q][1];
      if (x < 0 || y < 0 || x >= S->gw || y >= S->gh) continue;
      int ei = an_find(s, x, y);
      if ((ei >= 0 && (s->n[ei].fl & AN_CLOSED)) || !grid_free(x, y)) continue;
      int na = s->n[ci].a + (q < 4), nbb = s->n[ci].b + (q >= 4);
      if (na > 255 || nbb > 255) continue;
      float ng = na + nbb * SQRT2;
      bool opened = ei >= 0 && (s->n[ei].fl & AN_OPEN);
      if (opened && !(ng < an_g(&s->n[ei]))) continue;
      bool found;
      if (opened) {
        int v = za(s, &s->n[ei], &found);
        if (found) { memmove(s->open + v, s->open + v + 1, (size_t)(s->nopen - v - 1) * sizeof s->open[0]); s->nopen--; }
      } else if (ei < 0 && (ei = an_new(s, x, y)) < 0) continue;
      ANode *e = &s->n[ei];
      e->fl |= AN_OPEN;
      e->parent = (int16_t)ci;
      if (!e->h) e->h = an_h(x, y, gx, gy);
      e->a = (uint8_t)na;
      e->b = (uint8_t)nbb;
      const ANode *bn = &s->n[best];
      if (e->h < bn->h || (e->h == bn->h && an_g(e) < an_g(bn))) best = ei;
      int v = za(s, e, &found);
      if (!found && s->nopen < AS_MAX) {
        memmove(s->open + v + 1, s->open + v, (size_t)(s->nopen - v) * sizeof s->open[0]);
        s->open[v] = (int16_t)ei;
        s->nopen++;
      }
    }
  }
  int len = 0, n0 = -1, n1 = -1;
  for (int i = best; i >= 0 && s->n[i].parent >= 0 && len < AS_MAX; i = s->n[i].parent) { n1 = n0; n0 = i; len++; }
  if (n0 >= 0) { *c0x = s->n[n0].x; *c0y = s->n[n0].y; }
  if (n1 >= 0) { *c1x = s->n[n1].x; *c1y = s->n[n1].y; }
  return len;
}

static void cell_pos(int cx, int cy, float *x, float *y) {   /* Ni */
  *x = S->pa.x + (float)cx / S->gw * S->pa.w;
  *y = S->pa.y + (float)cy / S->gh * S->pa.h;
}

static void paths(void) {
  if (!S->gw) return;
  AStar as;
  for (int q = 0; q < S->nact; q++) {
    Actor *a = &S->act[S->order[q]];
    if (a->kind != A_CHAR || !(a->flags & AF_PF) || !(a->flags & AF_TARGET)) continue;
    float x, y;
    act_pos(a, &x, &y);
    int kx, ky, cx, cy;
    cell_of(x, y, &kx, &ky);
    cell_of(a->tx, a->ty, &cx, &cy);
    cx = clampi(cx, 0, S->gw - 1);
    cy = clampi(cy, 0, S->gh - 1);
    if (kx < 0 || kx >= S->gw) return;      /* the doodle throws here: the rest wait for the next tick */
    if (ky < 0 || ky >= S->gh) continue;    /* no start cell: the old path stays */
    int c0x = 0, c0y = 0, c1x = 0, c1y = 0;
    int len = astar(&as, kx, ky, cx, cy, &c0x, &c0y, &c1x, &c1y);
    a->npath = (uint8_t)(len > 2 ? 2 : len);
    if (len > 0) cell_pos(c0x, c0y, &a->p0x, &a->p0y);
    if (len > 1) cell_pos(c1x, c1y, &a->p1x, &a->p1y);
  }
}

/* ---------------------------------------------------------------- camera (Cq, Dq) */
static void camera(void) {
  NodeId cam = S->map;
#ifdef HOST
  extern bool host_cam(float *x, float *y);
  bool fixed = host_cam(&nodes[cam].x, &nodes[cam].y);   /* tests: the camera where they say */
#else
  bool fixed = false;
#endif
  if (S->pl >= 0 && !fixed) {
    float px, py;
    act_pos(&S->act[S->pl], &px, &py);
    Mat cg = node_global(cam);
    float det = cg.a * cg.d - cg.b * cg.c;
    if (fabsf(det) > 1e-9f) {
      float cx = (cg.d * (480 - cg.tx) - cg.c * (270 - cg.ty)) / det, cy = (-cg.b * (480 - cg.tx) + cg.a * (270 - cg.ty)) / det;
      float dx = px - cx, dy = py - cy;
      Mat cl = node_local(cam);
      float ox = cl.a * dx + cl.c * dy, oy = cl.b * dx + cl.d * dy;
      if (fabsf(ox) < .1f) ox = 0;
      if (fabsf(oy) < .1f) oy = 0;
      S->cam_tx = nodes[cam].x - ox;
      S->cam_ty = nodes[cam].y - oy;
      float mx = S->cam_tx - nodes[cam].x, my = S->cam_ty - nodes[cam].y, m = len2(mx, my);
      if (m > 0) {
        float st = fminf(S->cam_ease * m, S->cam_speed) / m;
        nodes[cam].x += mx * st;
        nodes[cam].y += my * st;
      }
    }
  }
  Mat g = node_global(cam);
  float det = g.a * g.d - g.b * g.c;
  if (fabsf(det) < 1e-9f) return;
  float x0 = (g.d * -g.tx + g.c * g.ty) / det, y0 = (g.b * g.tx - g.a * g.ty) / det;
  float x1 = (g.d * (960 - g.tx) - g.c * (540 - g.ty)) / det, y1 = (-g.b * (960 - g.tx) + g.a * (540 - g.ty)) / det;
  S->view = (Rect){x0, y0, x1 - x0, y1 - y0};
}

/* ---------------------------------------------------------------- sprite directions (aq) */
static void sprite_dirs(void) {
  for (int i = 0; i < S->nact; i++)
    if (S->act[i].n) act_dirs(S->act[i].n, S->act[i].dir);
  if (S->ball) act_dirs(S->ball, S->bdir);
}

/* ---------------------------------------------------------------- HUD (Ct) */
static void rhud_tick(void) {
  if (S->tick == 1 && S->hud_arrows && !(nodes[S->hud_arrows].flags & NF_VISIBLE)) {
    node_set_visible(S->hud_arrows, true);
    node_goto(S->hud_arrows, NULL, 0, true);
  }
  S->score = (int)(10 * jround(S->distance / 10)) + 100 * S->eaten;
  snprintf(S->score_buf, sizeof S->score_buf, "%d", S->score);
  if (S->hud_text) node_set_text(S->hud_text, S->score_buf);
  if (S->r0 != 0) {
    float m = 100 / S->r0;
    snprintf(S->dist_buf, sizeof S->dist_buf, "%dm/%dm", (int)jround(S->distance * m), (int)jround(S->r0 * m));
  } else snprintf(S->dist_buf, sizeof S->dist_buf, "NaNm/NaNm");
  if (S->hud_dist) node_set_text(S->hud_dist, S->dist_buf);
}

/* ---------------------------------------------------------------- the end zone (Hr) and the end (Et) */
static bool in_end_zone(void) {
  if (S->pl < 0) return false;
  NodeId p = S->act[S->pl].n;
  for (int i = 0; i < S->nact; i++) {
    Actor *e = &S->act[i];
    if (e->kind != A_END || !e->n || !(e->flags & AF_VIS) || !p) continue;
    Mat og = node_global(p);
    for (NodeId c = nodes[e->n].first; c; c = nodes[c].next) {
      if (!has(c, C_triggerArea)) continue;
      Mat ag = node_global(c);
      float det = ag.a * ag.d - ag.b * ag.c;
      if (fabsf(det) < 1e-9f) continue;
      float dx = og.tx - ag.tx, dy = og.ty - ag.ty;
      if (node_hit(c, (ag.d * dx - ag.c * dy) / det, (-ag.b * dx + ag.a * dy) / det)) return true;
    }
  }
  return false;
}

static void finish(int rating) {
  S->ended = true;
  S->end_t = END_DELAY;
  S->rating = rating;
  SOUND($Za);
}

static void the_end(bool zone) {
  if (S->ended) {
    if (S->win_t > 0 && --S->win_t == 0 && S->pl >= 0) act_label(&S->act[S->pl], "win");
    if (S->end_t > 0 && --S->end_t == 0) menus_game_over_rated((float)S->score, S->rating);
    return;
  }
  if (S->pl < 0) return;
  Actor *g = &S->act[S->pl];
  if (S->tackled) {
    toast_countdown(msg("ONIS_WIN"));
    finish(-1);
    for (int i = 0; i < S->nact; i++) {
      Actor *a = &S->act[i];
      if (a->kind != A_CHAR || !(a->flags & AF_ALLY)) continue;
      a->vx = a->vy = 0;
      act_label(a, "dead");
    }
    return;
  }
  if (S->r0 == 0) {
    float x, y;
    act_pos(g, &x, &y);
    S->r0 = S->end_r.x - x;
  }
  if (zone) {
    S->win_t = 5;
    for (int i = 0; i < S->nact; i++)
      if (S->act[i].kind == A_CHAR && (S->act[i].flags & AF_ALLY)) act_label(&S->act[i], "idle");
    finish(3);
    SOUND(e_a);
    toast_countdown(msg("YOU_WIN"));
  }
}

/* ---------------------------------------------------------------- the scene */
static void tick(void) {
  sys_back_pauses();                    /* Ep */
  if (!S || !S->map || menus_active()) return;   /* a menu pauses the scene at once */
  if (!S->started) {                    /* Uo */
    S->started = true;
    sys_tutorial_once();
    if (menus_active()) return;
  }
  bool go = sys_countdown(&S->cd);      /* Wo */
  if (go) fx_tick();                    /* Vp */
  bool zone = go && in_end_zone();      /* Hr */
  S->tile_t++;                          /* Os */
  if (go) visibility();                 /* Ls */
  sort_order();                         /* Pp */
  bool on = go && !S->ended;
  if (on) player_tick();                /* wt */
  if (go) ball_tick();                  /* xt */
  if (go) powerups();                   /* Dt */
  if (on) enemies();                    /* zt */
  if (on) allies();                     /* yt */
  if (on) tackles();                    /* Bt */
  if (go) zsprite();                    /* dq */
  physics();                            /* yr */
  paths();                              /* Gt */
  camera();                             /* Cq, Dq */
  if (go) sprite_dirs();                /* aq */
  rhud_tick();                           /* Ct */
  the_end(zone);                        /* Et */
}

/* ---------------------------------------------------------------- drawing: the map in the doodle's order */
static bool on_screen(Mat m, const int16_t nb[4], float margin) {
  if (!nb[2] && !nb[3]) return true;
  float xs[4] = {nb[0], nb[0] + nb[2], nb[0], nb[0] + nb[2]}, ys[4] = {nb[1], nb[1], nb[1] + nb[3], nb[1] + nb[3]};
  float lx = 1e9f, ly = 1e9f, hx = -1e9f, hy = -1e9f;
  for (int i = 0; i < 4; i++) {
    float X = m.a * xs[i] + m.c * ys[i] + m.tx, Y = m.b * xs[i] + m.d * ys[i] + m.ty;
    lx = fminf(lx, X); hx = fmaxf(hx, X); ly = fminf(ly, Y); hy = fmaxf(hy, Y);
  }
  return hx > -margin && lx < VIEW_W + margin && hy > -margin && ly < VIEW_H + margin;
}

static void draw_bg(Mat m) {
  Key wk, k;
  bool pr;
  if (S->bg_slot < 0 || !slot_key(nodes[S->map].sym, (unsigned)S->bg_slot, &wk)) return;
  Mat wm = mat_mul(m, key_mat(&wk));
  Clip wc;
  if (!clip_get(wk.ref, &wc)) return;
  int fc = comp_int(wc.T, C_tileBackground, F_frameCount, 0), fd = comp_int(wc.T, C_tileBackground, F_frameDuration, 1);
  if (fd < 1) fd = 1;
  int f = fc > 0 ? (S->tile_t / fd) % fc * fd : 0;
  Slots it;
  slots_begin(&it, wk.ref, 0);
  while (slots_next(&it, &k, &pr)) {
    if (!pr || (k.flags & K_HIDDEN) || k.kind != CK_SYM || !k.alpha) continue;
    int p = proto_of(k.ref);
    if (p < 0) continue;
    if (!on_screen(mat_mul(wm, key_mat(&k)), S->proto_nb[p], 0)) continue;
    NodeId n = S->proto[p];
    place(n, &k);
    int nf = node_frames(n);
    if (nf > 1) nodes[n].frame = (uint16_t)(f % nf);
    node_draw(n, wm);
  }
}

typedef struct { float key; NodeId n; int16_t ob; } DrawItem;
#define DRAW_MAX (ACT_MAX + FX_MAX + 72)
#define DRAW_OB 64

static void draw_under(void) {
  if (!S || !S->map) return;
  Mat m = mat_mul((Mat){1.0f / 3, 0, 0, 1.0f / 3, 0, 0}, node_global(S->map));
  draw_bg(m);
  DrawItem list[DRAW_MAX];
  Key obk[DRAW_OB];
  int n = 0, nk = 0;
  for (int i = 0; i < S->nact; i++) {
    Actor *a = &S->act[i];
    if (!a->n || !(a->flags & AF_VIS) || n >= DRAW_MAX) continue;
    float x, y;
    act_pos(a, &x, &y);
    list[n++] = (DrawItem){y, a->n, -1};
  }
  if (S->ball && n < DRAW_MAX) { float x, y; ent_pos(S->ball, &x, &y); list[n++] = (DrawItem){y, S->ball, -1}; }
  for (int i = 0; i < FX_MAX; i++)
    if (S->fx[i] && n < DRAW_MAX) { float x, y; ent_pos(S->fx[i], &x, &y); list[n++] = (DrawItem){y, S->fx[i], -1}; }
  /* rocks near the screen */
  Rect v = rect_pad(S->view, 48, 64, 48, 16);
  Slots it;
  Key k;
  bool pr;
  slots_begin(&it, nodes[S->map].sym, 0);
  while (slots_next(&it, &k, &pr)) {
    unsigned slot = it.i - 1;
    int o = slot < 256 ? S->slot_ob[slot] - 1 : -1;
    if (o < 0 || !pr || nk >= DRAW_OB || n >= DRAW_MAX || !ob_visible(o)) continue;
    Rect r = {S->ob_r[o][0] * .25f, S->ob_r[o][1] * .25f, S->ob_r[o][2] * .25f, S->ob_r[o][3] * .25f};
    if (!rect_intersects(r, v)) continue;
    float x, y;
    origin_of(&k, &x, &y);
    obk[nk] = k;
    list[n++] = (DrawItem){y, 0, (int16_t)nk++};
  }
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && list[j - 1].key > list[j].key; j--) {
      DrawItem t = list[j];
      list[j] = list[j - 1];
      list[j - 1] = t;
    }
  for (int i = 0; i < n; i++) {
    if (list[i].n) { node_draw(list[i].n, m); continue; }
    const Key *ok = &obk[list[i].ob];
    int p = proto_of(ok->ref);
    if (p < 0) continue;
    place(S->proto[p], ok);
    node_draw(S->proto[p], m);
  }
}

static void end(void) {
  if (S)
    for (int i = 0; i < S->nproto; i++) node_free(S->proto[i]);   /* not in the scene's tree */
  phys_reset();
  S = NULL;
}

const SceneDef scene_rugby = {"rugby", start, tick, end, draw_under, NULL, NULL};
