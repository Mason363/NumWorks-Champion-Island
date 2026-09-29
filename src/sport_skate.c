/* Skateboarding: the doodle's scene eu (library 8AB206B0C8EC450BA137F5272C53C5F0).
 *
 * Lucky skates around a park for 90 seconds: the arrows push the board
 * (it turns less the faster it goes), OK jumps (twice in the air, holding it
 * floats), arrows and OK in the air are tricks, landing on a rail grinds it.
 * Tricks chain into a combo that is banked 8 ticks after a clean landing:
 * (sum of the tricks' points) x (number of tricks), plus one point per tick
 * of grinding. Landing while a trick is still playing is a fall and loses
 * the combo. Speed boosts push the board along their arrows, and one of the
 * hidden champions shows at a time: touching it is "Hide and Seek" (5000).
 * Variants park1, park2, park3 (and tutorial) are the four maps of the root
 * clip ycb.
 *
 * Systems, in the doodle's order: Ep (pause), Uo (rules), Wo (countdown), Hr
 * (triggers: speed boosts bu, champions), Yt (champions), yr (physics), Ar
 * (ground height), Ot (the skater), Zt (HUD), Cq/Dq (camera), dq/tt (sprite
 * heights), Qp (draw order), Os (animated tiles), $t (timer and the end).
 *
 * The parks have up to 639 children (and 1601 ground tiles): far more than
 * the node pool. So the map is read from the data: its children are indexed
 * by 64 px bands at start, and each frame the ones in view are drawn in the
 * doodle's draw order through one prototype node per symbol, placed at each
 * child's keyframe. Only the skater and the champions near the view are
 * real nodes. The physics (the doodle's cannon world, where only the skater
 * moves) is a small box/ramp solver against the collidable children near
 * the skater. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/ent.h"
#include "../src/font.h"
#include "../src/spr.h"

#define SYM_MAX 64
#define PROTO_RESERVE 72        /* nodes kept free for menus, prompt icons, champions */
#define MENU_RESERVE 40         /* nodes always kept free for the pause and results menus */
#define STARTS_MAX 480
#define ENT_CAP 2300
#define VIS_MAX 128
#define CH_MAX 24
#define KK_MAX 40
#define ACT_KEEP 6
#define ICON_MAX 8
#define COL_MAX 40
#define TIME_START 2700         /* CN */
#define END_DELAY 80            /* Yo */
#define PI_F 3.14159265f
#define GRAD_STRIPS 12

/* ---------------------------------------------------------------- the data's timeline keys (as node.c reads them) */
typedef struct {
  uint16_t start;
  uint8_t flags, kind;
  uint16_t ref;
  float x, y;
  int16_t rx4, ry4;
  uint16_t mat, name, sp;
  uint8_t alpha, mode, loop;
} Key;

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

static const uint8_t *key_read(const uint8_t *p, Key *k) {
  unsigned start;
  p = varint(p, &start);
  k->start = (uint16_t)start;
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
  k->mode = MODE_INDEPENDENT; k->sp = 0; k->loop = 1;
  if (k->flags & K_CLIP) { k->mode = p[0]; k->sp = rd16(p + 1); k->loop = p[3]; p += 4; }
  k->name = NONE16;
  if (k->flags & K_NAME) { k->name = rd16(p); p += 2; }
  return p;
}

/* the key a slot shows at `frame` (false if none); *next: the following slot */
static bool slot_at(const uint8_t *p, unsigned frame, Key *out, const uint8_t **next) {
  unsigned n;
  p = varint(p, &n);
  bool has = false;
  for (unsigned i = 0; i < n; i++) {
    Key k;
    p = key_read(p, &k);
    if (k.start <= frame) { *out = k; has = true; }
  }
  if (next) *next = p;
  return has && !(out->flags & K_ABSENT);
}

static Mat key_mat(const Key *k) {
  float a = 1, b = 0, c = 0, d = 1;
  if (k->mat) { const float *m = mat(k->mat); a = m[0]; b = m[1]; c = m[2]; d = m[3]; }
  float rx = k->rx4 * 0.25f, ry = k->ry4 * 0.25f;
  return (Mat){a, b, c, d, k->x - (rx * a + ry * c), k->y - (rx * b + ry * d)};
}

static void mat_apply(Mat m, float x, float y, float *ox, float *oy) {
  *ox = m.a * x + m.c * y + m.tx;
  *oy = m.b * x + m.d * y + m.ty;
}

static bool mat_inv_apply(Mat m, float x, float y, float *ox, float *oy) {
  float det = m.a * m.d - m.b * m.c;
  if (fabsf(det) < 1e-9f) return false;
  float dx = x - m.tx, dy = y - m.ty;
  *ox = (m.d * dx - m.c * dy) / det;
  *oy = (-m.b * dx + m.a * dy) / det;
  return true;
}

/* the axis-aligned box of rect r placed by m */
static Rect mat_rect(Mat m, Rect r) {
  if (m.b == 0 && m.c == 0) {
    float x0 = m.a * r.x + m.tx, x1 = m.a * (r.x + r.w) + m.tx, y0 = m.d * r.y + m.ty, y1 = m.d * (r.y + r.h) + m.ty;
    return (Rect){fminf(x0, x1), fminf(y0, y1), fabsf(x1 - x0), fabsf(y1 - y0)};
  }
  float xs[4] = {r.x, r.x + r.w, r.x, r.x + r.w}, ys[4] = {r.y, r.y, r.y + r.h, r.y + r.h};
  float lx = 1e9f, ly = 1e9f, hx = -1e9f, hy = -1e9f;
  for (int i = 0; i < 4; i++) {
    float X, Y;
    mat_apply(m, xs[i], ys[i], &X, &Y);
    lx = X < lx ? X : lx; hx = X > hx ? X : hx; ly = Y < ly ? Y : ly; hy = Y > hy ? Y : hy;
  }
  return (Rect){lx, ly, hx - lx, hy - ly};
}

/* CreateJS runs the frame scripts (this.T = {...}) of independent clips only:
 * a synched or single-frame child never gets its components */
static bool has_comp(NodeId n, int comp) {
  return nodes[n].T != NONE16 && node_mode(&nodes[n]) == MODE_INDEPENDENT && comp_has(nodes[n].T, comp);
}

/* ---------------------------------------------------------------- vectors and directions, as kitsune has them */
static float vlen(float x, float y) { return sqrtf(x * x + y * y); }
/* hh: the angle of a vector in degrees [0, 360), NaN for a zero vector */
static float hh(float x, float y) {
  if (x == 0 && y == 0) return NAN;
  return fmodf(atan2f(y, x) + 2 * PI_F, 2 * PI_F) * 180 / PI_F;
}
static bool bh(float lo, float v, float hi) { return (v >= lo && v <= hi) || (v <= lo && v >= hi); }
/* lh / mh: the 8-way direction (0 e, 1 se ... 7 ne) of an angle / a vector, -1 for none */
static int lh(float a) {
  if (isnan(a)) return -1;
  a = fmodf(a, 360);
  if (bh(0, a, 22.5f) || bh(337.5f, a, 360)) return DIR_E;
  for (int d = 1; d < 8; d++)
    if (bh(d * 45 - 22.5f, a, d * 45 + 22.5f)) return d;
  return -1;
}
static int mh(float x, float y) { return lh(hh(x, y)); }
static int rg(int d) { return d < 0 ? -1 : (d + 4) & 7; }
/* ih: from angle a to angle b, in (-180, 180] (NaN counts as 0) */
static float ih(float a, float b) {
  if (isnan(a)) a = 0;
  if (isnan(b)) b = 0;
  float d = b - a;
  if (d > 180) d -= 360;
  else if (d < -180) d += 360;
  return d;
}
/* lg: v scaled to length l (zero if either is) */
static void lg(float x, float y, float l, float *ox, float *oy) {
  float c = vlen(x, y);
  if (c == 0 || l == 0) { *ox = *oy = 0; return; }
  *ox = x * l / c;
  *oy = y * l / c;
}

/* ---------------------------------------------------------------- tricks (Kt, Wj, Xj) */
enum { AC_N, AC_S, AC_E, AC_W, AC_ACTION, AC_AIR, AC_LAND, AC_GSTART, AC_GEND };
typedef struct {
  const char *text, *frame;
  uint16_t hf;
  uint8_t n, dur, name;       /* number of actions, duration, trick name (texts are kept per name) */
  uint8_t acts[4];
} Trick;
enum { TR_NOSE, TR_FAKIE = 13, TR_HIDE = 14, TR_COUNT };
static const Trick tricks[TR_COUNT] = {
  {"Nose Grab", "nose_grab", 100, 1, 10, 0, {AC_ACTION}},
  {"Kick Flip", "kick_flip_e", 200, 2, 15, 1, {AC_E, AC_ACTION}},
  {"Kick Flip", "kick_flip_w", 200, 2, 15, 1, {AC_W, AC_ACTION}},
  {"Barrel Roll", "barrel_roll_e", 300, 3, 24, 2, {AC_W, AC_N, AC_ACTION}},
  {"Barrel Roll", "barrel_roll_w", 300, 3, 24, 2, {AC_E, AC_N, AC_ACTION}},
  {"BS 180", NULL, 300, 3, 1, 3, {AC_E, AC_N, AC_W}},
  {"BS 180", NULL, 300, 3, 1, 3, {AC_W, AC_N, AC_E}},
  {"FS 180", NULL, 300, 3, 1, 4, {AC_E, AC_S, AC_W}},
  {"FS 180", NULL, 300, 3, 1, 4, {AC_W, AC_S, AC_E}},
  {"Headstand", "super_trick", 500, 4, 35, 5, {AC_S, AC_W, AC_N, AC_ACTION}},
  {"Headstand", "super_trick", 500, 4, 35, 5, {AC_S, AC_E, AC_N, AC_ACTION}},
  {"Helicopter", "helicopter", 500, 4, 35, 6, {AC_E, AC_W, AC_E, AC_ACTION}},
  {"Helicopter", "helicopter", 500, 4, 35, 6, {AC_W, AC_E, AC_W, AC_ACTION}},
  {"FAKIE", NULL, 100, 0, 0, 7, {0}},
  {"Hide and Seek", NULL, 5000, 0, 0, 8, {0}},
};
#define NAME_COUNT 9
static const char *const icon_labels[5] = {"n", "s", "e", "w", "action"};
static const char *const player_labels[6] = {"stop", "slow", "fast", "jump", "grind", "fall"};

/* ---------------------------------------------------------------- state */
enum {
  SF_COLL = 1, SF_ZOBJ = 2, SF_ZB = 4, SF_DORD = 8, SF_RAIL = 16, SF_CHAMP = 32, SF_BOOST = 64, SF_PLAYER = 128,
  SF_GROUND = 256, SF_BG = 512, SF_NODRAW = 1024, SF_TILEANIM = 2048, SF_BOUNDS = 4096, SF_MARKERS = 8192,
  SF_KEYB = 16384,           /* draw order from the bounds (zObject, boundable, zBoundable) */
  SF_SCREENS = 32768         /* has billboard screens (a colour filter the renderer lacks) */
};
typedef struct {
  uint16_t sym, T, fl, stamp;
  NodeId proto;
  uint8_t shape, fc;          /* body shape (0 box, 1 left ramp, 2 right ramp; | SH_BITMAP: one bitmap); tile frameCount */
  int8_t bdir;                /* speed boost direction */
  uint8_t bspeed;
  float z, h, dord;           /* zObject z, zBoundable height (for the draw order), drawOrderOverride */
  int16_t tz0, tz1;           /* trigger zMin, zMax */
  int16_t vis[4];             /* visual box (nominal bounds) x0 y0 x1 y1 */
  int16_t b[4];               /* bounds child rect x0 y0 x1 y1, 1/8 px */
  int16_t r[6];               /* rail markers: x y (1/8 px), z; twice */
} Sym;
#define SH_BITMAP 0x80              /* a static clip showing one bitmap: drawn straight */
#define SH_OPAQUE 0x40              /* ... fully opaque: hides what is under it */
#define Q8(v) ((int16_t)lrintf((v) * 8))
#define F8(v) ((v) * 0.125f)

typedef struct { float x0, y0, maxw, maxh; uint16_t nx, ny, s0, cell; } Index;
typedef struct { float key; uint16_t off; uint16_t what; } Vis;   /* what: 0 static, 1 player, 2 + champion */
typedef struct { uint16_t off; NodeId n; uint8_t alpha, active, out; uint16_t out_t; } Champ;
typedef struct { uint8_t type; int8_t dir; float vx, vy; } Action;
typedef struct {
  NodeId n;
  float x0, x1, a0, a1;
  uint8_t tx, ta;             /* tween ticks done (5 = over) */
  bool used, listed, fading, green;
} Icon;

typedef struct {
  /* the map */
  const uint8_t *mslots, *bslots;   /* timeline slots of the map and of its ground entity */
  uint16_t map_sym, bg_sym, player_off;
  Mat bgm;                          /* the ground entity in map space */
  Index mi, bi;
  uint16_t nent, nstarts;
  uint16_t starts[STARTS_MAX];
  uint16_t ent[ENT_CAP];
  uint16_t nsym, stamp;
  Sym sym[SYM_MAX];
  uint8_t symh[128];                /* symbol -> index + 1, open addressing */
  uint8_t nkk;
  int16_t kk[KK_MAX][2];            /* park 1: origins of the 288 px ground tiles (Kk) */
  uint16_t kk_sprite;
  uint8_t nch;
  Champ ch[CH_MAX];
  int8_t champ;                     /* Yt's champion, -1 none yet */
  uint16_t inside[4];               /* speed boosts the skater is in (Hr's ex) */
  uint8_t ninside;
  uint16_t nvis;
  Vis vis[VIS_MAX];
  /* nodes */
  NodeId map, player, hud, prompt, score, score_sh, time, time_sh, tname, tscore, spark_l, spark_r;
  float mrx, mry;                   /* the map's registration point */
  float qx, qy;                     /* Cq's goal for the map */
  Mat mapm;                         /* map to screen, this frame */
  /* the skater */
  float px, py, z, vx, vy, vz, kt;  /* position (map), z; velocity (px per tick); KT */
  float bcx, bcy, bhx, bhy;         /* bounds centre from the position, half sizes */
  int8_t dir, isc_dir;
  uint16_t isc;                     /* ticks the joystick has kept its direction (Kh.Sc) */
  float gravity;
  /* skatePlayer (ak) */
  int32_t cn, hf, iy;
  bool tb, flat, a7, grinding, markers_hidden, go, over, done, tut_done, time_red, trick_red;
  int16_t bb, ca, sk, kbt, oc;
  float rail[6];
  uint16_t rail_off;
  uint8_t nact;
  uint16_t act_total;
  Action act[ACT_KEEP];
  uint16_t ck_n;
  uint32_t ck_sum;
  const char *ck_frame;
  uint8_t nnames, name_order[NAME_COUNT];
  uint16_t name_cnt[NAME_COUNT];
  /* HUD */
  Icon icon[ICON_MAX];
  uint8_t nlist, list[ICON_MAX];
  float tr_a0, tr_a1;               /* trick texts' alpha tween */
  uint8_t tr_t;
  uint8_t tr_alpha;
  int16_t end_t;
  Countdown cd;
  uint32_t ticks;
  char score_buf[12], time_buf[12], tscore_buf[24], tname_buf[120];
  uint8_t grad[1 + GRAD_STRIPS * 23];
} State;

static State *S;
_Static_assert(sizeof(State) <= SCENE_STATE_MAX, "skate state too big");

/* the box of a shape's polygons */
static bool shape_rect(const uint8_t *sh, Rect *r) {
  float lx = 1e9f, ly = 1e9f, hx = -1e9f, hy = -1e9f;
  const uint8_t *p = sh + 1;
  for (int s = 0; s < sh[0]; s++) {
    int np = p[4];
    p += 5;
    for (int k = 0; k < np; k++) {
      int n = rd16(p);
      p += 2;
      for (int i = 0; i < n; i++, p += 4) {
        float x = rds16(p) * .25f, y = rds16(p + 2) * .25f;
        lx = fminf(lx, x); hx = fmaxf(hx, x); ly = fminf(ly, y); hy = fmaxf(hy, y);
      }
    }
  }
  if (lx > hx) return false;
  *r = (Rect){lx, ly, hx - lx, hy - ly};
  return true;
}

/* CreateJS's getBounds from the data: what a symbol shows at a frame (visible children), placed by m */
typedef struct { float lx, ly, hx, hy; bool any; } Acc;
static void acc_rect(Acc *a, Rect r) {
  if (!a->any) { a->lx = r.x; a->ly = r.y; a->hx = r.x + r.w; a->hy = r.y + r.h; a->any = true; return; }
  a->lx = fminf(a->lx, r.x); a->ly = fminf(a->ly, r.y); a->hx = fmaxf(a->hx, r.x + r.w); a->hy = fmaxf(a->hy, r.y + r.h);
}
static void content(uint16_t sym, unsigned frame, Mat m, Acc *a, int depth) {
  if (sym >= SYM_COUNT || depth > 8) return;
  SymInfo si;
  sym_info(sym, &si);
  if (si.type == SYM_BITMAP) {
    Sprite sp;
    sprite_info((uint16_t)si.v, &sp);
    acc_rect(a, mat_rect(m, (Rect){0, 0, sp.w, sp.h}));
    return;
  }
  Rect r;
  if (si.type == SYM_SHAPE) { if (shape_rect(payload((uint16_t)si.v), &r)) acc_rect(a, mat_rect(m, r)); return; }
  Clip c;
  if (si.type != SYM_CLIP || !clip_get(sym, &c) || !c.nframes) return;
  frame %= c.nframes;
  const uint8_t *p = c.slots;
  for (unsigned i = 0; i < c.nslots; i++) {
    Key k;
    const uint8_t *nx;
    bool on = slot_at(p, frame, &k, &nx);
    p = nx;
    if (!on || (k.flags & K_HIDDEN)) continue;
    Mat km = mat_mul(m, key_mat(&k));
    if (k.kind == CK_SHAPE) { if (shape_rect(payload(k.ref), &r)) acc_rect(a, mat_rect(km, r)); }
    else if (k.kind == CK_SYM) {
      unsigned f = k.mode == MODE_SYNCHED ? k.sp + frame - k.start : k.mode == MODE_SINGLE ? k.sp : 0;
      content(k.ref, f, km, a, depth + 1);
    }
  }
}

/* is every pixel of a sprite opaque? (its cached run-length rows) */
static bool sprite_opaque(uint16_t sp) {
  const uint8_t *r = spr_get(sp);
  if (!r) return false;
  Sprite si;
  sprite_info(sp, &si);
  const uint8_t *al = palalpha(si.sheet);
  int w = rd16(r), h = rd16(r + 2);
  for (int y = 0; y < h; y++) {
    const uint8_t *p = r + rd16(r + 4 + 2 * y);
    int n = rd16(p), cx = 0;
    p += 2;
    for (int k = 0; k < n; k++) {
      if (*p++) return false;          /* a gap */
      int code = *p++;
      bool fill = code & 0x80;
      int len = fill ? (code & 0x7F) + 1 : code;
      for (int i = 0; i < (fill ? 1 : len); i++) if (al[p[i]] != 255) return false;
      p += fill ? 1 : len;
      cx += len;
    }
    if (cx < w) return false;
  }
  return true;
}

/* ---------------------------------------------------------------- symbols */
/* the billboards' screens: their first child is drawn in one colour (ColorFilter(0, 0, 0, 1, r, g, b)) */
static const struct { uint16_t sym; uint8_t r, g, b; } screens[6] = {
  {S_skate_Mla, 109, 101, 223}, {S_skate_Nla, 0, 219, 176}, {S_skate_Ola, 241, 108, 235},
  {S_skate_Pla, 243, 39, 98}, {S_skate_Qla, 170, 218, 78}, {S_skate_Rla, 55, 190, 236}};
static int screen_of(uint16_t sym) {
  for (int i = 0; i < 6; i++) if (screens[i].sym == sym) return i;
  return -1;
}
static void clip_frame0(uint16_t sym, Clip *c) { if (!clip_get(sym, c)) memset(c, 0, sizeof *c); }

static Sym *sym_get(uint16_t ref) {
  unsigned h = (ref * 2654435761u) >> 25;
  while (S->symh[h]) {
    Sym *o = &S->sym[S->symh[h] - 1];
    if (o->sym == ref) return o;
    h = (h + 1) & 127;
  }
  if (S->nsym >= SYM_MAX) return NULL;
  S->symh[h] = (uint8_t)(S->nsym + 1);
  SymInfo si;
  if (ref >= SYM_COUNT) return NULL;
  sym_info(ref, &si);
  Sym *s = &S->sym[S->nsym++];
  memset(s, 0, sizeof *s);
  s->sym = ref;
  s->T = NONE16;
  s->bdir = -1;
  if (si.type != SYM_CLIP) {
    s->fl = SF_NODRAW;
    return s;
  }
  Clip c;
  clip_frame0(ref, &c);
  uint16_t T = s->T = c.T;
  if (comp_has(T, C_zObject)) { s->fl |= SF_ZOBJ; s->z = comp_float(T, C_zObject, F_z, 0); }
  if (comp_has(T, C_zBoundable)) { s->fl |= SF_ZB; s->h = comp_float(T, C_zBoundable, F_height, 100); }
  if (comp_has(T, C_drawOrderOverride)) { s->fl |= SF_DORD; s->dord = comp_float(T, C_drawOrderOverride, F_drawOrder, 0); }
  if (comp_has(T, C_skateRail)) s->fl |= SF_RAIL;
  if (comp_has(T, C_skateChamp)) s->fl |= SF_CHAMP;
  if (comp_has(T, C_skatePlayer)) s->fl |= SF_PLAYER;
  if (comp_has(T, C_speedBoost)) {
    s->fl |= SF_BOOST;
    uint16_t d = comp_str(T, C_speedBoost, F_direction);
    s->bdir = (int8_t)(d != NONE16 ? dir_parse(str(d)) : DIR_W);
    s->bspeed = (uint8_t)comp_float(T, C_speedBoost, F_speed, 0);
  }
  if (comp_has(T, C_trigger)) {
    s->tz0 = (int16_t)clampf(comp_float(T, C_trigger, F_zMin, -30000), -30000, 30000);
    s->tz1 = (int16_t)clampf(comp_float(T, C_trigger, F_zMax, 30000), -30000, 30000);
  }
  if (comp_has(T, C_tileBackground)) {
    int fc = comp_int(T, C_tileBackground, F_frameCount, 0);
    if (s->fl & SF_DORD && s->dord <= -1e5f) s->fl |= ref == S_skate_Kk ? SF_GROUND : SF_BG;
    else if (fc > 0) { s->fl |= SF_TILEANIM; s->fc = (uint8_t)fc; }
  }
  if ((s->fl & SF_ZOBJ) && (s->fl & SF_ZB)) {
    uint16_t sh = comp_str(T, C_zBoundable, F_shape);
    if (sh != NONE16 && !strcmp(str(sh), "leftRamp")) s->shape = 1;
    if (sh != NONE16 && !strcmp(str(sh), "rightRamp")) s->shape = 2;
  }
  /* the children of the first frame: the bounds child (Oj), rail markers (Mt), what is drawn */
  bool draws = false;
  int nmark = 0;
  const uint8_t *p = c.slots;
  for (unsigned i = 0; i < c.nslots; i++) {
    Key k;
    const uint8_t *nx;
    bool on = slot_at(p, 0, &k, &nx);
    p = nx;
    if (!on) continue;
    if (!(k.flags & K_HIDDEN) && k.alpha) draws = true;
    if (k.kind != CK_SYM || k.ref >= SYM_COUNT) continue;
    Clip cc;
    SymInfo ci;
    sym_info(k.ref, &ci);
    if (ci.type != SYM_CLIP) continue;
    clip_frame0(k.ref, &cc);
    Mat m = key_mat(&k);
    if (k.mode != MODE_INDEPENDENT) continue;
    if (screen_of(k.ref) >= 0) s->fl |= SF_SCREENS;
    Acc ba = {0, 0, 0, 0, false};
    if (!(s->fl & SF_BOUNDS) && comp_has(cc.T, C_bounds) && (content(k.ref, 0, m, &ba, 0), ba.any)) {
      Rect r = {ba.lx, ba.ly, ba.hx - ba.lx, ba.hy - ba.ly};
      s->b[0] = Q8(r.x); s->b[1] = Q8(r.y); s->b[2] = Q8(r.x + r.w); s->b[3] = Q8(r.y + r.h);
      s->fl |= SF_BOUNDS;
    }
    if (comp_has(cc.T, C_skateRailMarker) && nmark < 2) {
      s->r[nmark * 3] = Q8(m.tx);
      s->r[nmark * 3 + 1] = Q8(m.ty);
      s->r[nmark * 3 + 2] = (int16_t)comp_float(cc.T, C_skateRailMarker, F_z, 0);
      nmark++;
      s->fl |= SF_MARKERS;
    }
  }
  if (nmark < 2) s->fl &= (uint16_t)~(SF_MARKERS | SF_RAIL);
  if (T == NONE16 && c.nframes == 1 && c.nslots == 1) {
    Key k;
    SymInfo ci;
    if (slot_at(c.slots, 0, &k, NULL) && k.kind == CK_SYM && k.ref < SYM_COUNT && (sym_info(k.ref, &ci), ci.type == SYM_BITMAP)) {
      s->shape |= SH_BITMAP;
      if (k.alpha == 255 && !(k.flags & K_HIDDEN) && sprite_opaque((uint16_t)ci.v)) s->shape |= SH_OPAQUE;
    }
  }
  if (!draws && c.nframes <= 1) s->fl |= SF_NODRAW;
  if (comp_has(T, C_collidable) && comp_has(T, C_boundable) && (s->fl & SF_BOUNDS)) s->fl |= SF_COLL;
  if ((s->fl & SF_ZOBJ) && (s->fl & SF_ZB) && (s->fl & SF_BOUNDS) && comp_has(T, C_boundable)) s->fl |= SF_KEYB;
  /* what it may draw: nominal bounds, up by its height (dq) and some room for effects */
  Acc va = {0, 0, 0, 0, false};
  content(ref, 0, MAT_ID, &va, 0);
  float x0 = va.lx, y0 = va.ly, x1 = va.hx, y1 = va.hy;
  if (!va.any) { x0 = -64; y0 = -128; x1 = 64; y1 = 32; }
  if (s->fl & SF_ZOBJ) y0 -= s->z;
  if (s->fl & SF_CHAMP) y0 -= 200;
  s->vis[0] = (int16_t)floorf(x0); s->vis[1] = (int16_t)floorf(y0); s->vis[2] = (int16_t)ceilf(x1); s->vis[3] = (int16_t)ceilf(y1);
  return s;
}

/* a map (or ground) child: its key, symbol and placement */
typedef struct { Key k; Sym *s; Mat m; Rect box; } Item;

/* grow: room around the nominal bounds (map children), 0 for ground tiles */
static bool item_at(const uint8_t *slots, uint16_t off, Mat parent, Item *it, float grow) {
  if (!slot_at(slots + off, 0, &it->k, NULL) || it->k.kind != CK_SYM) return false;
  it->s = sym_get(it->k.ref);
  if (!it->s) return false;
  it->m = mat_mul(parent, key_mat(&it->k));
  const int16_t *v = it->s->vis;
  it->box = mat_rect(it->m, (Rect){v[0] - grow, v[1] - grow, v[2] - v[0] + 2 * grow, v[3] - v[1] + 2 * grow});
  return true;
}

/* a text or a shape of the map's own timeline (no symbol) */
static bool loose_at(const uint8_t *slots, uint16_t off, Mat parent, Key *k, Mat *m, Rect *box) {
  if (!slot_at(slots + off, 0, k, NULL) || (k->kind != CK_TEXT && k->kind != CK_SHAPE)) return false;
  *m = mat_mul(parent, key_mat(k));
  Rect r;
  if (k->kind == CK_SHAPE) {
    if (!shape_rect(payload(k->ref), &r)) return false;
  } else {
    const uint8_t *t = payload(k->ref);
    float lw = rds16(t + 8) * .25f, lh = rds16(t + 10) * .25f;
    if (lw <= 0) lw = 600;
    if (lh <= 0) lh = t[2] * 1.2f;
    r = (Rect){t[6] == 1 ? -lw / 2 : t[6] == 2 ? -lw : 0, -lh, lw, lh * 6};
  }
  *box = mat_rect(*m, r);
  return true;
}

static Rect item_bounds(const Item *it) {
  const int16_t *b = it->s->b;
  return mat_rect(it->m, (Rect){F8(b[0]), F8(b[1]), F8(b[2] - b[0]), F8(b[3] - b[1])});
}

/* the cannon body: zObject and zBoundable give its bottom and height, else 0 and 100 */
static float body_z0(const Sym *s) { return (s->fl & SF_ZOBJ) && (s->fl & SF_ZB) ? s->z : 0; }
static float body_h(const Sym *s) { return (s->fl & SF_ZOBJ) && (s->fl & SF_ZB) ? fminf(s->h, 20000) : 100; }

/* ---------------------------------------------------------------- the index: children by grid cell
 * Each child is listed once, in the cell of its box's top left corner, in
 * timeline order; a query also reads the cells up to the index's widest and
 * tallest child to the left and above. Bigger children are in one more list
 * that every query reads. Queries merge the lists back into timeline order. */
#define TALL 160
#define QMAX 48
static void index_build(Index *ix, const uint8_t *slots, unsigned nslots, Mat parent, uint16_t cell, bool ground) {
  float lx = 1e9f, ly = 1e9f, hx = -1e9f, hy = -1e9f, maxw = 0, maxh = 0;
  ix->nx = ix->ny = 0;
  for (int pass = 0; pass < 3; pass++) {
    const uint8_t *p = slots;
    for (unsigned i = 0; i < nslots; i++) {
      uint16_t off = (uint16_t)(p - slots);
      Key k;
      const uint8_t *nx;
      slot_at(p, 0, &k, &nx);
      p = nx;
      Item it;
      Rect r;
      uint16_t fl = 0;
      if (!item_at(slots, off, parent, &it, ground ? 0 : 8)) {
        Key lk;
        Mat lm;
        if (!loose_at(slots, off, parent, &lk, &lm, &r)) continue;
      } else {
        fl = it.s->fl;
        r = it.box;
      }
      if (!ground && (fl & (SF_GROUND | SF_BG | SF_PLAYER))) continue;
      if ((fl & SF_NODRAW) && !(fl & (SF_COLL | SF_RAIL | SF_BOOST))) continue;
      if (fl & SF_COLL) {
        Rect c = item_bounds(&it);
        float x0 = fminf(r.x, c.x), y0 = fminf(r.y, c.y);
        r = (Rect){x0, y0, fmaxf(r.x + r.w, c.x + c.w) - x0, fmaxf(r.y + r.h, c.y + c.h) - y0};
      }
      if (fl & SF_RAIL) {
        float ax, ay, bx, by;
        mat_apply(it.m, F8(it.s->r[0]), F8(it.s->r[1]), &ax, &ay);
        mat_apply(it.m, F8(it.s->r[3]), F8(it.s->r[4]), &bx, &by);
        float x0 = fminf(r.x, fminf(ax, bx)), y0 = fminf(r.y, fminf(ay, by));
        r = (Rect){x0, y0, fmaxf(r.x + r.w, fmaxf(ax, bx)) - x0, fmaxf(r.y + r.h, fmaxf(ay, by)) - y0};
      }
      bool big = r.w > TALL || r.h > TALL;
      if (pass == 0) {
        lx = fminf(lx, r.x); ly = fminf(ly, r.y); hx = fmaxf(hx, r.x); hy = fmaxf(hy, r.y);
        if (!big) { maxw = fmaxf(maxw, r.w); maxh = fmaxf(maxh, r.h); }
        continue;
      }
      int c = big ? ix->nx * ix->ny
                  : clampi((int)floorf((r.y - ix->y0) / cell), 0, ix->ny - 1) * ix->nx + clampi((int)floorf((r.x - ix->x0) / cell), 0, ix->nx - 1);
      if (pass == 1) S->starts[ix->s0 + c + 1]++;
      else if (S->starts[ix->s0 + c] < ENT_CAP) S->ent[S->starts[ix->s0 + c]++] = off;
    }
    if (pass == 0) {
      if (lx > hx) { lx = ly = 0; hx = hy = 1; }
      ix->x0 = floorf(lx);
      ix->y0 = floorf(ly);
      ix->cell = cell;
      ix->maxw = maxw;
      ix->maxh = maxh;
      int nx = (int)((hx - ix->x0) / cell) + 1, ny = (int)((hy - ix->y0) / cell) + 1;
      int room = STARTS_MAX - 2 - S->nstarts;
      nx = clampi(nx, 1, room > 1 ? room : 1);
      ny = clampi(ny, 1, room / nx > 1 ? room / nx : 1);
      ix->nx = (uint16_t)nx;
      ix->ny = (uint16_t)ny;
      int n = nx * ny;
      ix->s0 = S->nstarts;
      S->nstarts = (uint16_t)(S->nstarts + n + 2);
      S->starts[ix->s0] = S->nent;
      for (int b = 1; b <= n + 1; b++) S->starts[ix->s0 + b] = 0;
    } else if (pass == 1) {
      for (int b = 1; b <= ix->nx * ix->ny + 1; b++) {
        uint32_t v = (uint32_t)S->starts[ix->s0 + b] + S->starts[ix->s0 + b - 1];
#ifdef HOST
        if (v > ENT_CAP) fprintf(stderr, "skate: index needs %u entries\n", v);
#endif
        S->starts[ix->s0 + b] = (uint16_t)(v > ENT_CAP ? ENT_CAP : v);
      }
    }
  }
  /* pass 2 left each list's start at the next one's: shift back */
  int n = ix->nx * ix->ny;
  for (int b = n + 1; b >= 1; b--) S->starts[ix->s0 + b] = S->starts[ix->s0 + b - 1];
  S->starts[ix->s0] = S->nent;
  S->nent = S->starts[ix->s0 + n + 1];
  /* the parks' ground has layers: an opaque tile hides the tiles placed just like it before it */
  if (!ground) return;
  for (int c = 0; c < n; c++)
    for (int i = S->starts[ix->s0 + c]; i < S->starts[ix->s0 + c + 1]; i++) {
      Item a, b;
      if (!item_at(slots, S->ent[i], parent, &a, 0) || !(a.s->shape & SH_OPAQUE)) continue;
      for (int j = S->starts[ix->s0 + c]; j < i; j++)
        if (S->ent[j] != 0xFFFF && item_at(slots, S->ent[j], parent, &b, 0) && fabsf(a.box.x - b.box.x) < .5f &&
            fabsf(a.box.y - b.box.y) < .5f && fabsf(a.box.w - b.box.w) < .5f && fabsf(a.box.h - b.box.h) < .5f)
          S->ent[j] = 0xFFFF;
    }
}

/* the children that may reach the rectangle, each once, in timeline order */
typedef void (*ItemFn)(uint16_t off, void *ctx);
static void index_query(const Index *ix, float x0, float y0, float x1, float y1, ItemFn fn, void *ctx) {
  if (!ix->nx) return;
  int cx0 = (int)floorf((x0 - ix->maxw - ix->x0) / ix->cell), cx1 = (int)floorf((x1 - ix->x0) / ix->cell);
  int cy0 = (int)floorf((y0 - ix->maxh - ix->y0) / ix->cell), cy1 = (int)floorf((y1 - ix->y0) / ix->cell);
  uint16_t pos[QMAX], end[QMAX];
  int k = 0;
  if (cx1 >= 0 && cy1 >= 0 && cx0 < ix->nx && cy0 < ix->ny) {
    cx0 = clampi(cx0, 0, ix->nx - 1); cx1 = clampi(cx1, 0, ix->nx - 1);
    cy0 = clampi(cy0, 0, ix->ny - 1); cy1 = clampi(cy1, 0, ix->ny - 1);
    for (int cy = cy0; cy <= cy1; cy++)
      for (int cx = cx0; cx <= cx1 && k < QMAX - 1; cx++) {
        int c = ix->s0 + cy * ix->nx + cx;
        if (S->starts[c] == S->starts[c + 1]) continue;
        pos[k] = S->starts[c];
        end[k++] = S->starts[c + 1];
      }
  }
  int t = ix->s0 + ix->nx * ix->ny;    /* the big ones */
  pos[k] = S->starts[t];
  end[k++] = S->starts[t + 1];
  for (;;) {
    int best = -1;
    uint16_t bo = 0xFFFF;
    for (int i = 0; i < k; i++) {
      while (pos[i] < end[i] && S->ent[pos[i]] == 0xFFFF) pos[i]++;   /* hidden ground tiles */
      if (pos[i] < end[i] && S->ent[pos[i]] < bo) { best = i; bo = S->ent[pos[i]]; }
    }
    if (best < 0) break;
    pos[best]++;
    fn(bo, ctx);
  }
}

/* ---------------------------------------------------------------- prototypes: one node per symbol, placed per child */
/* frame-0 scripts that stop: CreateJS runs them before the first tick */
static void first_stops(NodeId n, int depth) {
  if (depth > 12) return;
  if (nodes[n].kind == NK_CLIP) {
    Clip c;
    if (clip_get(nodes[n].sym, &c))
      for (int i = 0; i < c.nacts; i++)
        if (rd16(c.acts + 5 * i) == nodes[n].frame && c.acts[5 * i + 2] == ACT_STOP) node_stop(n);
  }
  for (NodeId c = nodes[n].first; c; c = nodes[c].next) first_stops(c, depth + 1);
}

/* where a clip created at the scene's start would be after t ticks */
static void advance(NodeId n, uint32_t t, int depth) {
  if (depth > 12) return;
  Node *p = &nodes[n];
  if (p->kind == NK_CLIP && node_mode(p) == MODE_INDEPENDENT && (p->flags & NF_PLAYING) && (p->flags & NF_TICK)) {
    Clip c;
    if (clip_get(p->sym, &c) && c.nframes > 1) {
      int stop = -1;
      for (int i = 0; i < c.nacts; i++)
        if (c.acts[5 * i + 2] == ACT_STOP && (stop < 0 || rd16(c.acts + 5 * i) < stop)) stop = rd16(c.acts + 5 * i);
      uint32_t f = p->frame + t;
      if (stop >= 0 && f >= (uint32_t)stop) { f = (uint32_t)stop; node_stop(n); }
      else if (!node_loops(p) && f >= c.nframes) f = c.nframes - 1u;
      else f %= c.nframes;
      p->frame = (uint16_t)f;
      node_update_one(n);
    }
  }
  for (NodeId c = nodes[n].first; c; c = nodes[c].next) advance(c, t, depth + 1);
}

/* dq: zSprite children sit at (0, -z) of the zObject within three parents; tt: shadows at -KT */
static void set_local(NodeId n, float x, float y) {
  Mat l = node_local(n);
  nodes[n].x = x + nodes[n].rx4 * 0.25f * l.a;
  nodes[n].y = y + nodes[n].ry4 * 0.25f * l.d;
}

static void zsprites(NodeId n, NodeId root, float root_z, int depth) {
  if (depth > 10) return;
  for (NodeId c = nodes[n].first; c; c = nodes[c].next) {
    if (has_comp(c, C_zSprite)) {
      NodeId a = n;
      for (int up = 0; up < 3 && a; up++, a = nodes[a].parent) {
        if (a == root) { set_local(c, 0, -root_z); break; }
        if (has_comp(a, C_zObject)) { set_local(c, 0, -comp_float(nodes[a].T, C_zObject, F_z, 0)); break; }
      }
    }
    zsprites(c, root, root_z, depth + 1);
  }
}

static void hide_markers(NodeId n) {
  for (NodeId c = nodes[n].first; c; c = nodes[c].next)
    if (has_comp(c, C_skateRailMarker)) nodes[c].alpha = 0;
}

static void proto_setup(Sym *s) {
  NodeId p = s->proto;
  if (s->fl & SF_TILEANIM)
    for (NodeId c = nodes[p].first; c; c = nodes[c].next) nodes[c].flags &= (uint8_t)~NF_TICK;   /* Ps caches them */
  else {
    first_stops(p, 0);
    advance(p, S->ticks, 0);
  }
  if (S->go) zsprites(p, p, s->z, 0);
  if (S->markers_hidden && (s->fl & SF_MARKERS)) hide_markers(p);
}

/* make room for `need` more nodes: forget the prototypes not drawn lately */
static bool room(unsigned need) {
  while (node_count() + need >= NODE_MAX) {
    Sym *old = NULL;
    for (int i = 0; i < S->nsym; i++) {
      Sym *o = &S->sym[i];
      if (o->proto && o->stamp != S->stamp && (!old || (uint16_t)(S->stamp - o->stamp) > (uint16_t)(S->stamp - old->stamp))) old = o;
    }
    if (!old) return false;
    node_free(old->proto);
    old->proto = 0;
  }
  return true;
}

static NodeId proto(Sym *s) {
  s->stamp = S->stamp;
  if (s->proto) return s->proto;
  if (!room(PROTO_RESERVE)) return 0;
  NodeId n = node_new_sym(s->sym);
  if (!n) return 0;
  node_add(S->map, n);
  s->proto = n;
  proto_setup(s);
  return n;
}

static void place(NodeId p, const Key *k) {
  Node *n = &nodes[p];
  n->x = k->x;
  n->y = k->y;
  n->rx4 = k->rx4;
  n->ry4 = k->ry4;
  n->mat = k->mat;
  n->alpha = k->alpha;
  if (k->flags & K_HIDDEN) n->flags &= (uint8_t)~NF_VISIBLE; else n->flags |= NF_VISIBLE;
}

/* Os: tiles with frameCount show frame floor(tick / duration) % count */
static void tile_frames(Sym *s) {
  int f = (int)(S->ticks % (s->fc ? s->fc : 1));
  for (NodeId c = nodes[s->proto].first; c; c = nodes[c].next)
    if (nodes[c].kind == NK_CLIP && nodes[c].frame != f) {
      nodes[c].frame = (uint16_t)(f % node_frames(c));
      node_update_one(c);
    }
}

/* a billboard: first without its screens, then each screen: its colour, then the rest of it */
static void draw_screens(NodeId p, Mat parent) {
  NodeId sc[6];
  int n = 0;
  for (NodeId c = nodes[p].first; c && n < 6; c = nodes[c].next)
    if (screen_of(nodes[c].sym) >= 0 && node_visible(c)) {
      sc[n++] = c;
      nodes[c].flags &= (uint8_t)~NF_VISIBLE;
    }
  node_draw(p, parent);
  Mat pm = mat_mul(parent, node_local(p));
  for (int i = 0; i < n; i++) {
    NodeId c = sc[i];
    nodes[c].flags |= NF_VISIBLE;
    Mat cm = mat_mul(pm, node_local(c));
    uint8_t al = (uint8_t)(nodes[p].alpha * nodes[c].alpha / 255);
    Clip cc;
    Key k0;
    if (clip_get(nodes[c].sym, &cc) && cc.nslots && slot_at(cc.slots, nodes[c].frame, &k0, NULL) && k0.kind == CK_SYM) {
      Acc sa = {0, 0, 0, 0, false};
      content(k0.ref, 0, mat_mul(cm, key_mat(&k0)), &sa, 0);
      if (sa.any) {
        Rect r = {sa.lx, sa.ly, sa.hx - sa.lx, sa.hy - sa.ly};
        int x0 = (int)floorf(r.x + .5f), y0 = (int)floorf(r.y + .5f), x1 = (int)floorf(r.x + r.w + .5f), y1 = (int)floorf(r.y + r.h + .5f);
        int k = screen_of(nodes[c].sym);
        gfx_rect(x0, y0, x1 - x0, y1 - y0, rgb565(screens[k].r, screens[k].g, screens[k].b), al);
      }
    }
    for (NodeId g = nodes[c].first; g; g = nodes[g].next)
      if (nodes[g].slot != 0) node_draw_in(g, cm, al);
  }
}

static void draw_item(const Item *it, Mat parent) {
  if (it->s->shape & SH_BITMAP) {
    Clip c;
    Key k;
    SymInfo si;
    if ((it->k.flags & K_HIDDEN) || !clip_get(it->s->sym, &c) || !slot_at(c.slots, 0, &k, NULL) || (k.flags & K_HIDDEN)) return;
    sym_info(k.ref, &si);
    gfx_sprite((uint16_t)si.v, mat_mul(mat_mul(parent, key_mat(&it->k)), key_mat(&k)), (uint8_t)(it->k.alpha * k.alpha / 255));
    return;
  }
  NodeId p = proto(it->s);
  if (!p) return;
  if (it->s->fl & SF_TILEANIM) tile_frames(it->s);
  place(p, &it->k);
  if (it->s->fl & SF_SCREENS) draw_screens(p, parent);
  else node_draw(p, parent);
}

static void draw_loose(const Key *k, Mat m) {
  if ((k->flags & K_HIDDEN) || !k->alpha) return;
  if (k->kind == CK_SHAPE) { gfx_shape(payload(k->ref), mat_mul(m, key_mat(k)), k->alpha); return; }
  NodeId t = node_new(NK_TEXT);
  if (!t) return;
  nodes[t].ref = k->ref;
  place(t, k);
  node_draw(t, m);
  node_free(t);
}

/* ---------------------------------------------------------------- the HUD (ck): texts, trick prompt icons, tweens */
static float cubic_out(float t) { t -= 1; return t * t * t + 1; }

static void icon_x(Icon *ic, float x) {
  ic->x0 = nodes[ic->n].x;
  ic->x1 = x;
  ic->tx = 0;
}
static void icon_fade(Icon *ic) {
  ic->a0 = nodes[ic->n].alpha / 255.0f;
  ic->a1 = 0;
  ic->ta = 0;
  ic->fading = true;
}

static void icon_free(int i) {
  if (S->icon[i].used && S->icon[i].n) node_free(S->icon[i].n);
  memset(&S->icon[i], 0, sizeof S->icon[i]);
}

static int icon_new(void) {
  for (int i = 0; i < ICON_MAX; i++)
    if (!S->icon[i].used) return i;
  /* the one fading longest */
  int best = -1;
  for (int i = 0; i < ICON_MAX; i++)
    if (!S->icon[i].listed && (best < 0 || S->icon[i].ta > S->icon[best].ta)) best = i;
  if (best >= 0) icon_free(best);
  return best;
}

static float lerp(float a, float b, float t) { return a + (b - a) * t; }

/* Ut: the prompt's icons fade out */
static void ut(void) {
  for (int i = 0; i < S->nlist; i++) {
    Icon *ic = &S->icon[S->list[i]];
    icon_fade(ic);
    ic->listed = false;
  }
  S->nlist = 0;
}

/* Vt: the trick texts show again, the sparkles play */
static void vt(void) {
  if (S->spark_l) { node_goto(S->spark_l, NULL, 1, true); node_update_one(S->spark_l); }
  if (S->spark_r) { node_goto(S->spark_r, NULL, 1, true); node_update_one(S->spark_r); }
  S->trick_red = false;
  S->tr_t = 255;
  S->tr_alpha = 255;
}

static void trick_texts_fade(void) {
  S->tr_a0 = S->tr_alpha / 255.0f;
  S->tr_a1 = 0;
  S->tr_t = 0;
}

static int32_t combo_score(void) { return S->iy + (int32_t)(S->ck_sum * S->ck_n); }   /* IY + Lt(CK) */

static void combo_clear(void) {
  S->ck_n = 0;
  S->ck_sum = 0;
  S->nnames = 0;
  memset(S->name_cnt, 0, sizeof S->name_cnt);
}

/* Wt: the combo is banked */
static void wt(void) {
  int32_t g = combo_score();
  S->hf += g;
  S->iy = 0;
  snprintf(S->score_buf, sizeof S->score_buf, "%d", (int)S->hf);
  trick_texts_fade();
  /* (the score also swells to 1.2 for a moment: the pixel font has no size between 1 and 2) */
  combo_clear();
}

/* Pt: an action, and its icon on the prompt for directions and OK */
static void pt(uint8_t type) {
  if (S->nact == ACT_KEEP) memmove(S->act, S->act + 1, sizeof S->act[0] * (ACT_KEEP - 1)), S->nact--;
  S->act[S->nact++] = (Action){type, S->dir, S->vx, S->vy};
  if (S->act_total < 60000) S->act_total++;
  if (type > AC_ACTION || !S->prompt) return;
  int i = icon_new();
  if (i < 0) return;
  if (!room(MENU_RESERVE + 8)) return;
  NodeId n = node_new_sym(S_skate_Lqa);
  if (!n) return;
  node_add(S->prompt, n);
  node_goto(n, icon_labels[type], 0, false);
  node_update_one(n);
  Icon *ic = &S->icon[i];
  *ic = (Icon){n, 0, 0, 1, 1, 5, 5, true, true, false, false};
  if (S->nlist < ICON_MAX) S->list[S->nlist++] = (uint8_t)i;
  int g = S->nlist;
  bool k = g > 4;
  for (int a = 0; a < g; a++) {
    Icon *c = &S->icon[S->list[a]];
    float h = k ? lerp(-60, 60, (a - 1) / 3.0f) : g > 1 ? lerp(-20.0f * (g - 1), 20.0f * (g - 1), a / (float)(g - 1)) : 0;
    if (c == ic) { nodes[n].x = h; c->x0 = c->x1 = h; }
    else {
      c->ta = 5;                      /* removeTweens */
      c->fading = false;
      icon_x(c, h);
      if (k && a == 0) { icon_fade(c); c->listed = false; }
    }
  }
  if (k) {
    memmove(S->list, S->list + 1, (size_t)(S->nlist - 1));
    S->nlist--;
  }
}

/* Xt: a trick joins the combo */
static void xt(int t, int nacts) {
  const Trick *tr = &tricks[t];
  S->ck_n++;
  S->ck_sum += tr->hf;
  S->ck_frame = tr->frame;
  if (t == TR_NOSE) S->ck_frame = S->dir == DIR_E || S->dir == DIR_N ? "nose_grab_e" : "nose_grab_w";
  if (!S->name_cnt[tr->name] && S->nnames < NAME_COUNT) S->name_order[S->nnames++] = tr->name;
  S->name_cnt[tr->name]++;
  if (tr->dur > 0) S->kbt = tr->dur;
  S->nact = 0;
  S->act_total = 0;
  /* uh(name + "_COMPLETE", true): never read back by the doodle, not kept here */
  vt();
  (void)nacts;
  if (S->nlist) {
    int m = S->nlist, g = tr->n, c = m - g;
    for (int a = 0; a < m; a++) {
      Icon *ic = &S->icon[S->list[a]];
      float x = g > 1 ? lerp(-20.0f * (g - 1), 20.0f * (g - 1), (a - c) / (float)(g - 1)) : 0;
      ic->ta = 5;
      ic->fading = false;
      icon_x(ic, x);
      if (a < c) { icon_fade(ic); ic->listed = false; }
      else ic->green = true;   /* ColorFilter(.2, 1, .2): drawn by draw_over */
    }
    if (c > 0) {
      memmove(S->list, S->list + c, (size_t)(m - c));
      S->nlist = (uint8_t)(m - c);
    }
  }
}

static void tweens(void) {
  for (int i = 0; i < ICON_MAX; i++) {
    Icon *ic = &S->icon[i];
    if (!ic->used) continue;
    if (ic->tx < 5) {
      ic->tx++;
      nodes[ic->n].x = lerp(ic->x0, ic->x1, cubic_out(ic->tx / 5.0f));
    }
    if (ic->fading && ic->ta < 5) {
      ic->ta++;
      nodes[ic->n].alpha = (uint8_t)(255 * lerp(ic->a0, ic->a1, ic->ta / 5.0f) + .5f);
    }
    if (!ic->listed && ic->fading && ic->ta >= 5 && ic->tx >= 5) icon_free(i);
  }
  if (S->tr_t < 30) {
    S->tr_t++;
    S->tr_alpha = (uint8_t)(255 * lerp(S->tr_a0, S->tr_a1, S->tr_t / 30.0f) + .5f);
  }
  for (int i = 0; i < ICON_MAX; i++)
    if (S->icon[i].used && S->icon[i].n) {
      if (S->icon[i].green) nodes[S->icon[i].n].flags &= (uint8_t)~NF_VISIBLE;
      else nodes[S->icon[i].n].flags |= NF_VISIBLE;
    }
  if (S->tname) nodes[S->tname].alpha = S->tr_alpha;
  if (S->tscore) nodes[S->tscore].alpha = S->tr_alpha;
}

static void time_text(char *b, size_t n, int32_t cn) {
  snprintf(b, n, "%d:%02d.%02d", (int)(cn / 1800 % 60), (int)(cn / 30 % 60), (int)(cn % 30 * 100 / 30));
}

/* Zt */
static void hud_update(void) {
  time_text(S->time_buf, sizeof S->time_buf, S->cn);
  snprintf(S->score_buf, sizeof S->score_buf, "%d", (int)S->hf);
  int32_t m = combo_score();
  if (m) {
    int k = S->ck_n;
    if (k > 1) snprintf(S->tscore_buf, sizeof S->tscore_buf, "x%d! %d", k, (int)m);
    else snprintf(S->tscore_buf, sizeof S->tscore_buf, "%d", (int)m);
    size_t len = 0;
    S->tname_buf[0] = 0;
    for (int i = 0; i < S->nnames; i++) {
      int nm = S->name_order[i];
      const char *text = "";
      for (int t = 0; t < TR_COUNT; t++) if (tricks[t].name == nm) { text = tricks[t].text; break; }
      len += (size_t)snprintf(S->tname_buf + len, len < sizeof S->tname_buf ? sizeof S->tname_buf - len : 0, "%s%s", i ? " + " : "", text);
      if (S->name_cnt[nm] > 1 && len < sizeof S->tname_buf)
        len += (size_t)snprintf(S->tname_buf + len, sizeof S->tname_buf - len, " x %d", S->name_cnt[nm]);
      if (len >= sizeof S->tname_buf) break;
    }
    /* the sparkles either side of the trick's score */
    if (S->tscore && S->spark_l && S->spark_r) {
      int w, h;
      font_scale(1);
      font_measure(S->tscore_buf, 0, 0, &w, &h);
      float cx = nodes[S->tscore].x, x0 = cx - w * 1.5f;
      nodes[S->spark_l].x = x0 - 45;
      nodes[S->spark_r].x = x0 + w * 3 + 30;
    }
  }
  S->time_red = S->cn < 450 && (S->cn / 15) % 2 == 0;
}

/* ---------------------------------------------------------------- the skater's world: boxes and ramps near it (yr, Ar) */
typedef struct { float x0, y0, x1, y1, z0, z1; uint8_t shape; } Col;
typedef struct { Col c[COL_MAX]; int n; float x0, y0, x1, y1; } ColSet;

static void col_add(uint16_t off, void *ctx) {
  ColSet *cs = ctx;
  Item it;
  if (cs->n >= COL_MAX || !item_at(S->mslots, off, MAT_ID, &it, 0) || !(it.s->fl & SF_COLL)) return;
  Rect b = item_bounds(&it);
  if (b.x > cs->x1 || b.y > cs->y1 || b.x + b.w < cs->x0 || b.y + b.h < cs->y0) return;
  float z0 = body_z0(it.s);
  cs->c[cs->n++] = (Col){b.x, b.y, b.x + b.w, b.y + b.h, z0, z0 + body_h(it.s), (uint8_t)(it.s->shape & 3)};
}

/* the top of a collider at x (a ramp's slope) */
static float col_top(const Col *c, float x) {
  if (!c->shape) return c->z1;
  float t = (x - c->x0) / (c->x1 - c->x0);
  t = clampf(t, 0, 1);
  if (c->shape == 2) t = 1 - t;
  return c->z0 + (c->z1 - c->z0) * t;
}

typedef struct { float x, y, z, vx, vy, vz, hx, hy, hz; } Body;

/* a contact of cannon's narrowphase: normal (towards the skater), depth (< 0 inside), points */
typedef struct { float nx, ny, nz, g; uint8_t np; bool flat; } Contact;
#define CONTACT_MAX 24

/* SAT between the skater's box and a box or ramp (cannon's convexConvex over the face axes): the
 * axis of least overlap is the contact's normal */
static bool contact_with(const Body *b, const Col *c, Contact *o) {
  float ox = fminf(b->x + b->hx, c->x1) - fmaxf(b->x - b->hx, c->x0), oy = fminf(b->y + b->hy, c->y1) - fmaxf(b->y - b->hy, c->y0);
  float oz = fminf(b->z + b->hz, c->z1) - fmaxf(b->z - b->hz, c->z0);
  if (ox < 0 || oy < 0 || oz < 0) return false;
  float best = ox;
  *o = (Contact){b->x < (c->x0 + c->x1) / 2 ? -1.0f : 1.0f, 0, 0, 0, 4, false};
  if (oy < best) { best = oy; *o = (Contact){0, b->y < (c->y0 + c->y1) / 2 ? -1.0f : 1.0f, 0, 0, 4, false}; }
  if (oz < best) { best = oz; *o = (Contact){0, 0, b->z > (c->z0 + c->z1) / 2 ? 1.0f : -1.0f, 0, 4, true}; }
  if (c->shape) {
    /* the slope: the ramp reaches its plane, the box down to its lowest corner */
    bool left = c->shape == 1;
    float span = c->x1 - c->x0, s = span > 0 ? (c->z1 - c->z0) / span : 0, l = sqrtf(1 + s * s);
    float nx = (left ? -s : s) / l, nz = 1 / l;
    float top = nx * (left ? c->x0 : c->x1) + nz * c->z0, bottom = nx * (left ? c->x1 : c->x0) + nz * c->z0;
    float ext = fabsf(nx) * b->hx + nz * b->hz, mid = nx * b->x + nz * b->z;
    float on = fminf(top, mid + ext) - fmaxf(bottom, mid - ext);
    if (on < 0) return false;   /* a separating axis */
    if (on < best) { best = on; *o = (Contact){nx, 0, nz, 0, 2, true}; }
  }
  o->g = -best;
  return true;
}

static int contacts_at(const Body *b, const ColSet *cs, Contact *out) {
  int n = 0;
  if (b->z - b->hz <= 0) out[n++] = (Contact){0, 0, 1, b->z - b->hz, 4, true};   /* the ground plane */
  for (int i = 0; i < cs->n && n < CONTACT_MAX; i++)
    if (contact_with(b, &cs->c[i], &out[n])) n++;
  return n;
}

/* cannon's GSSolver with SPOOK contact equations (stiffness 1e7, relaxation 3), the skater's side only */
static void solve(Body *b, const Contact *c, int n, float h, float gz) {
  const float k = 1e7f, d = 3, im = 1.0f / 10;   /* mass 10 */
  const float sa = 4 / (h * (1 + 4 * d)), sb = 4 * d / (1 + 4 * d), eps = 4 / (h * h * k * (1 + 4 * d));
  float lam[CONTACT_MAX * 4], rhs[CONTACT_MAX * 4];
  const Contact *eq[CONTACT_MAX * 4];
  int m = 0;
  for (int i = 0; i < n; i++)
    for (int p = 0; p < c[i].np && m < CONTACT_MAX * 4; p++, m++) {
      eq[m] = &c[i];
      lam[m] = 0;
      float gw = b->vx * c[i].nx + b->vy * c[i].ny + b->vz * c[i].nz;
      rhs[m] = -c[i].g * sa - gw * sb - gz * c[i].nz * h;
    }
  float wx = 0, wy = 0, wz = 0;
  const float cc = im + eps;
  for (int it = 0; it < 40 && m; it++) {
    float tot = 0;
    for (int i = 0; i < m; i++) {
      const Contact *e = eq[i];
      float gwl = wx * e->nx + wy * e->ny + wz * e->nz;
      float dl = (rhs[i] - gwl - eps * lam[i]) / cc;
      if (lam[i] + dl < 0) dl = -lam[i];
      lam[i] += dl;
      tot += fabsf(dl);
      wx += im * dl * e->nx;
      wy += im * dl * e->ny;
      wz += im * dl * e->nz;
    }
    if (tot * tot < 1e-14f) break;
  }
  b->vx += wx;
  b->vy += wy;
  b->vz += wz;
}

/* yr: one frame of the cannon world (4 substeps), then Ar (the ground under the skater) */
static void physics(void) {
  ColSet cs;
  cs.n = 0;
  cs.x0 = S->px + S->bcx - S->bhx - 40;
  cs.x1 = S->px + S->bcx + S->bhx + 40;
  cs.y0 = S->py + S->bcy - S->bhy - 40;
  cs.y1 = S->py + S->bcy + S->bhy + 40;
  index_query(&S->mi, cs.x0, cs.y0, cs.x1, cs.y1, col_add, &cs);
  Body b = {S->px + S->bcx, S->py + S->bcy, S->z + 10, S->vx * 30, S->vy * 30, S->vz * 30, S->bhx, S->bhy, 10};
  float h = 1.0f / (FPS * 4), damp = powf(1 - .4f, h);
  bool flat = false;
  Contact ct[CONTACT_MAX];
  for (int st = 0; st < 4; st++) {
    int n = contacts_at(&b, &cs, ct);
    if (st == 3) {
      flat = false;
      for (int i = 0; i < n; i++) if (ct[i].flat) flat = true;
    }
    solve(&b, ct, n, h, S->gravity);
    b.vx *= damp;
    b.vy *= damp;
    b.vz *= damp;
    b.vz += S->gravity * h;
    b.x += b.vx * h;
    b.y += b.vy * h;
    b.z += b.vz * h;
  }
  S->vx = b.vx / 30;
  S->vy = b.vy / 30;
  S->vz = b.vz / 30;
  S->px = b.x - S->bcx;
  S->py = b.y - S->bcy;
  S->z = b.z - b.hz;
  S->flat = flat;
  if (!S->go) return;
  /* Ar: rays down from 9 above at the corners of its bounds */
  float g = 0;
  for (int k = 0; k < 4; k++) {
    float x = b.x + (k & 1 ? b.hx : -b.hx), y = b.y + (k & 2 ? b.hy : -b.hy), hit = 0;
    for (int i = 0; i < cs.n; i++) {
      const Col *c = &cs.c[i];
      if (x < c->x0 || x > c->x1 || y < c->y0 || y > c->y1) continue;
      float t = col_top(c, x);
      if (t <= S->z + 9 && t > hit) hit = t;
    }
    float v = fminf(hit, S->z);
    if (v > g) g = v;
  }
  S->kt = g;
}

/* ---------------------------------------------------------------- the skater (Ot and its helpers St, Rt, Qt, Tt) */
/* Nt: the point of line (a, b) nearest to p, within the segment unless `line` */
static void nt(const float *a, const float *b, const float *p, bool line, float *o) {
  float c[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
  float len = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
  if (len < 1e-6f) { memcpy(o, a, sizeof(float) * 3); return; }
  for (int i = 0; i < 3; i++) c[i] /= len;
  float d = (p[0] - a[0]) * c[0] + (p[1] - a[1]) * c[1] + (p[2] - a[2]) * c[2];
  if (!line) d = clampf(d, 0, len);
  for (int i = 0; i < 3; i++) o[i] = a[i] + c[i] * d;
}
static float dist3(const float *a, const float *b) {
  float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
  return sqrtf(x * x + y * y + z * z);
}

/* St: on the rail's line, 2 above it, at 5 px a tick along it */
static void st(void) {
  float p[3] = {S->px, S->py, S->z}, m[3];
  const float *k0 = S->rail, *k1 = S->rail + 3;
  float c[3] = {k1[0] - k0[0], k1[1] - k0[1], k1[2] - k0[2]};
  nt(k0, k1, p, true, m);
  S->px = m[0];
  S->py = m[1];
  S->z = m[2] + 2;
  S->kt = S->z;
  float len = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
  if (len < 1e-6f) return;
  float u[3] = {c[0] / len * 5, c[1] / len * 5, c[2] / len * 5};
  if (fabsf(ih(hh(c[0], c[1]), hh(S->vx, S->vy))) > 90) for (int i = 0; i < 3; i++) u[i] = -u[i];
  S->vx = u[0];
  S->vy = u[1];
  S->vz = u[2];
}

static void qt(void) {
  SOUND(grind_stop);
  if (S->grinding) {
    S->grinding = false;
    pt(AC_GEND);
  }
}

static void rt(const float *m, uint16_t off) {
  SOUND(grind_loop);
  ut();
  S->grinding = true;
  memcpy(S->rail, m, sizeof S->rail);
  S->rail_off = off;
  S->bb = S->ca = 0;
  S->iy++;
  pt(AC_GSTART);
  vt();
  st();
}

/* Tt: the trick timer, the landing timer (then the combo is banked), and tricks */
static void tricks_check(void) {
  if (S->kbt > 0 && --S->kbt == 0) ut();
  if (S->tb && S->oc > 0 && --S->oc == 0) { wt(); ut(); }
  if (!S->act_total || !S->nact) return;
  const Action *m = &S->act[S->nact - 1];
  if (m->type == AC_LAND && m->dir == rg(mh(m->vx, m->vy)) && vlen(m->vx, m->vy) > 1) xt(TR_FAKIE, 1);
  if (S->a7) { S->a7 = false; xt(TR_HIDE, 0); }
  int best = -1;
  for (int t = 0; t < TR_FAKIE; t++) {
    int c = tricks[t].n;
    if (S->nact < c || (best >= 0 && tricks[best].hf >= tricks[t].hf)) continue;
    bool same = true;
    for (int i = 0; i < c; i++)
      if (S->act[S->nact - c + i].type != tricks[t].acts[i]) same = false;
    if (same) best = t;
  }
  if (best >= 0) xt(best, tricks[best].n);
}

/* rails (Mt: their two markers, in the map) near the skater */
typedef struct { float p[3]; float v[2]; } RailCtx;
static void rail_markers(const Item *it, float *m) {
  mat_apply(it->m, F8(it->s->r[0]), F8(it->s->r[1]), &m[0], &m[1]);
  m[2] = it->s->r[2];
  mat_apply(it->m, F8(it->s->r[3]), F8(it->s->r[4]), &m[3], &m[4]);
  m[5] = it->s->r[5];
}

static void rail_check(uint16_t off, void *ctx) {
  RailCtx *rc = ctx;
  Item it;
  if (!item_at(S->mslots, off, MAT_ID, &it, 0) || !(it.s->fl & SF_RAIL)) return;
  float c[6], h[3];
  rail_markers(&it, c);
  const float *b = rc->p;
  float a[2] = {c[3] - c[0], c[4] - c[1]};
  nt(c, c + 3, b, false, h);
  float n = dist3(b, h);
  float av = hh(a[0], a[1]), bv = hh(-a[0], -a[1]), g = hh(S->vx, S->vy);
  if (S->grinding) {
    float jx = in.jx, jy = in.jy, j = hh(jx, jy);
    if (S->rail_off == off) return;
    if (dist3(c, b) < 5) {
      if (fabsf(ih(av, g)) < 30) rt(c, off);
      else if (vlen(jx, jy) > 0 && fabsf(ih(av, j)) < 30 && fabsf(ih(av, g)) < 120) { S->vx = a[0]; S->vy = a[1]; rt(c, off); }
    } else if (dist3(c + 3, b) < 5) {
      if (fabsf(ih(bv, g)) < 30) rt(c, off);
      else if (vlen(jx, jy) > 0 && fabsf(ih(bv, j)) < 30 && fabsf(ih(bv, g)) < 120) { S->vx = -a[0]; S->vy = -a[1]; rt(c, off); }
    }
  } else if (!S->tb && S->vz <= 0 && vlen(S->vx, S->vy) > 0 && n < 14 && S->z - 3 <= h[2] &&
             (fabsf(ih(av, g)) < 60 || fabsf(ih(bv, g)) < 60))
    rt(c, off);
}

static void markers_hide_all(void) {
  S->markers_hidden = true;
  for (int i = 0; i < S->nsym; i++)
    if (S->sym[i].proto && (S->sym[i].fl & SF_MARKERS)) hide_markers(S->sym[i].proto);
}

/* Kj: gotoAndStop(label); false (and paused) when it is already there */
static bool kj(NodeId n, const char *label) {
  if (!strcmp(node_label(n), label)) { node_stop(n); return false; }
  if (!node_goto(n, label, 0, false)) return false;
  node_update_one(n);
  return true;
}

static void skater(void) {
  NodeId P = S->player;
  float jx = in.jx, jy = in.jy, jl = vlen(jx, jy);
  if (!S->grinding) {
    bool was = S->tb;
    S->tb = S->flat;
    if (S->tb) S->bb = S->ca = 0;
    if (!was && S->tb) {
      S->nact = 0;
      S->act_total = 0;
      if (S->kbt > 0) {           /* landed during a trick: a fall */
        SOUND(fall);
        S->sk = 30;
        S->grinding = false;
        combo_clear();
        S->trick_red = true;
        trick_texts_fade();
      } else {
        pt(AC_LAND);
        S->oc = 8;
      }
    } else if (was && !S->tb) {
      qt();
      pt(AC_AIR);
    }
  }
  if (!(S->sk > 0 || S->kbt > 0)) {
    if (!S->markers_hidden) markers_hide_all();
    RailCtx rc = {{S->px, S->py, S->z}, {S->vx, S->vy}};
    index_query(&S->mi, S->px - 48, S->py - 48, S->px + 48, S->py + 48, rail_check, &rc);
    if (S->grinding) {
      float h[3];
      nt(S->rail, S->rail + 3, rc.p, false, h);
      float d = dist3(rc.p, h);
      S->iy++;
      st();
      if (d > 14) qt();
    }
  }
  if (!S->grinding) {
    if (S->sk > 0) {
      S->sk--;
      S->vx *= .8f;
      S->vy *= .8f;
    } else if (!(S->kbt > 0)) {
      if (S->isc == 1 && !S->tb) {
        int d = mh(jx, jy);
        if (d == DIR_N) pt(AC_N);
        else if (d == DIR_S) pt(AC_S);
        else if (d == DIR_E) pt(AC_E);
        else if (d == DIR_W) pt(AC_W);
      }
      float b = vlen(S->vx, S->vy), c = hh(S->vx, S->vy);
      float gx = jx * 5, gy = jy * 5, k = vlen(gx, gy);
      if (jl > 0) {
        gx -= S->vx;
        gy -= S->vy;
        float ma = ih(c, hh(jx, jy));
        k = fmaxf(0, k - b);
        if (S->tb) {
          float c2 = fminf(.75f, k), k2 = (25 - fminf(3 + b, 25)) / 25, a = 30 * k2;
          a = c + clampf(ma, -a, a);
          float lx, ly;
          lg(gx, gy, c2, &lx, &ly);
          float g2x = S->vx + lx, g2y = S->vy + ly;
          float m2 = k2 * clampf((180 - fabsf(ma)) / 80, 0, 1);
          if (isnan(a)) { S->vx = g2x; S->vy = g2y; }
          else {
            float bx, by;
            lg(cosf(a * PI_F / 180), sinf(a * PI_F / 180), b + fmaxf(c2, 0), &bx, &by);
            S->vx = g2x + (bx - g2x) * m2;
            S->vy = g2y + (by - g2y) * m2;
          }
        } else {
          float b3 = fminf(.35f, k);
          if (b3 > 0) {
            float lx, ly;
            lg(gx, gy, b3, &lx, &ly);
            S->vx += lx;
            S->vy += ly;
          }
        }
      }
    }
  }
  /* gravity for the next step: lighter while a jump is held */
  S->gravity = -300;
  if (S->bb > 0) { S->bb++; if (in.held[A_ACTION]) S->gravity = -210; }
  if (S->ca > 0) S->ca++;
  if (in.pressed[A_ACTION] && !(S->kbt > 0) && !(S->sk > 0)) {
    pt(AC_ACTION);
    if (S->tb) pt(AC_AIR);
    if (!(S->bb != 0 && S->ca != 0)) {
      S->vz = 6;
      S->z += 1;
      if (S->ca && jl > 0) { S->vx = jx * 4; S->vy = jy * 4; }
      if (S->grinding) qt();
      if (S->bb == 0) { S->bb = 1; SOUND(jump); }
      else if (!S->ca) { S->ca = 1; SOUND(jump2); }
    }
  }
  tricks_check();
  /* which way it faces */
  int b = mh(jx, jy);
  if (b < 0) b = S->dir;
  if (S->tb && vlen(S->vx, S->vy) > 1) {
    int g = mh(S->vx, S->vy);
    S->dir = (int8_t)(g >= 0 && g != rg(b) && g != b ? g : b);
  } else if (jl > 0 && b >= 0) S->dir = (int8_t)b;
  /* its pose */
  if (S->sk > 0) kj(P, player_labels[5]);
  else if (S->kbt > 0) { if (S->ck_frame) kj(P, S->ck_frame); }
  else if (S->grinding) kj(P, player_labels[4]);
  else if (S->bb > 0 || S->ca > 0) {
    if (S->bb == 1 || S->ca == 1)
      for (NodeId c = nodes[P].first; c; c = nodes[c].next)
        if (node_visible(c) && has_comp(c, C_sprite))
          for (NodeId g = nodes[c].first; g; g = nodes[g].next)
            if (nodes[g].kind == NK_CLIP) node_goto(g, NULL, 0, true);
    kj(P, player_labels[3]);
  } else {
    float v = vlen(S->vx, S->vy);
    kj(P, player_labels[jl == 0 && v < .3f ? 0 : v < 2 ? 1 : 2]);
  }
  for (NodeId c = nodes[P].first; c; c = nodes[c].next)
    if (node_visible(c) && has_comp(c, C_sprite)) ent_dir_label(c, S->dir);
#ifdef HOST
  if (getenv("SKATE_TRACE"))
    fprintf(stderr, "T %u %g %g %d %.3f %.3f %.3f %.4f %.4f %.4f %d %s %s %d %d %d %d %d\n", game.ticks, in.jx, in.jy, in.held[A_ACTION], S->px, S->py, S->z,
            S->vx, S->vy, S->vz, S->tb, dir_names[S->dir], node_label(P), (int)S->hf, S->bb, S->ca, S->kbt, S->grinding);
#endif
}

/* dq and tt for the skater: its sprites at -z, its shadow on the ground (-KT) */
static void heights(void) {
  NodeId P = S->player;
  for (NodeId c = nodes[P].first; c; c = nodes[c].next) {
    uint16_t T = nodes[c].T;
    if (T == NONE16 || !node_visible(c)) continue;
    if (comp_has(T, C_zSprite)) set_local(c, 0, -S->z);
    else if (comp_has(T, C_shadow) && comp_has(T, C_sprite)) set_local(c, 0, -S->kt);
  }
}

static void player_place(void) {
  nodes[S->player].x = S->px + nodes[S->player].rx4 * .25f;
  nodes[S->player].y = S->py + nodes[S->player].ry4 * .25f;
}

/* ---------------------------------------------------------------- triggers (Hr): speed boosts (bu) and the champion */
/* is the skater (its position and z) in one of e's trigger areas? m: e in the map */
static bool in_trigger(NodeId e, Mat m, int z0, int z1) {
  if (!bh(z0 - .1f, S->z, z1 + .1f)) return false;
  for (NodeId a = nodes[e].first; a; a = nodes[a].next) {
    if (!has_comp(a, C_triggerArea) || !(nodes[a].flags & NF_ONSTAGE)) continue;
    float u, v;
    if (mat_inv_apply(mat_mul(m, node_local(a)), S->px, S->py, &u, &v) && node_hit(a, u, v)) return true;
  }
  return false;
}

typedef struct { uint16_t hit[4]; int n; } HitSet;
static void boost_check(uint16_t off, void *ctx) {
  HitSet *hs = ctx;
  Item it;
  if (hs->n >= 4 || !item_at(S->mslots, off, MAT_ID, &it, 0) || !(it.s->fl & SF_BOOST)) return;
  if (!rect_contains(it.box, S->px, S->py)) return;
  NodeId p = proto(it.s);
  if (!p) return;
  place(p, &it.k);
  if (in_trigger(p, it.m, it.s->tz0, it.s->tz1)) hs->hit[hs->n++] = off;
}

static void boost_fire(uint16_t off) {
  Item it;
  if (!item_at(S->mslots, off, MAT_ID, &it, 0)) return;
  if (S->dir == it.s->bdir && vlen(S->vx, S->vy) > 2) {
    float a = it.s->bdir * 45 * PI_F / 180;
    S->vx += cosf(a) * it.s->bspeed / 3;
    S->vy += sinf(a) * it.s->bspeed / 3;
    SOUND(boost);
  }
}

static void triggers(void) {
  HitSet hs = {{0}, 0};
  index_query(&S->mi, S->px - 64, S->py - 64, S->px + 64, S->py + 64, boost_check, &hs);
  for (int i = 0; i < S->ninside; i++) {
    bool still = false;
    for (int j = 0; j < hs.n; j++) if (hs.hit[j] == S->inside[i]) still = true;
    if (!still) { S->inside[i] = S->inside[--S->ninside]; i--; }
  }
  for (int j = 0; j < hs.n; j++) {
    bool was = false;
    for (int i = 0; i < S->ninside; i++) if (S->inside[i] == hs.hit[j]) was = true;
    if (was || S->ninside >= 4) continue;
    S->inside[S->ninside++] = hs.hit[j];
    boost_fire(hs.hit[j]);
  }
}

/* ---------------------------------------------------------------- champions (Yt): one hides somewhere at a time */
static void champ_node_state(int i) {
  Champ *c = &S->ch[i];
  NodeId n = c->n;
  if (!n) return;
  nodes[n].alpha = c->alpha ? 255 : 0;
}

/* nodes for the champions near the view */
static void champs_sync(Rect view) {
  Rect near = rect_pad(view, 64, 64, 64, 64);
  for (int i = 0; i < S->nch; i++) {
    Champ *c = &S->ch[i];
    Item it;
    if (!item_at(S->mslots, c->off, MAT_ID, &it, 0)) continue;
    bool want = rect_intersects(it.box, near);
    if (want && !c->n && room(MENU_RESERVE + 40)) {
      NodeId n = node_new_sym(it.s->sym);
      if (!n) continue;
      node_add(S->map, n);
      place(n, &it.k);
      first_stops(n, 0);
      if (c->out) {
        node_goto(n, "out", 0, false);
        node_update_one(n);
        for (NodeId ch = nodes[n].first; ch; ch = nodes[ch].next) advance(ch, S->ticks - c->out_t, 1);
      } else advance(n, S->ticks, 0);
      if (S->go) zsprites(n, n, it.s->z, 0);
      c->n = n;
      champ_node_state(i);
    } else if (!want && c->n) {
      node_free(c->n);
      c->n = 0;
    }
  }
}

static int champ_pick(bool inactive_only) {
  int cand[CH_MAX], n = 0;
  for (int i = 0; i < S->nch; i++)
    if (!inactive_only || !S->ch[i].active) cand[n++] = i;
  if (!n) return -1;
  int k = (int)(frand() * n);
  return cand[k < n ? k : n - 1];
}

static void champion(void) {
  if (S->champ >= 0) {
    Champ *c = &S->ch[S->champ];
    Item it;
    if (!c->n || !item_at(S->mslots, c->off, MAT_ID, &it, 0) || !in_trigger(c->n, it.m, it.s->tz0, it.s->tz1)) return;
    S->a7 = true;
    int pick = champ_pick(true);
    c->active = 0;
    c->out = 1;
    c->out_t = (uint16_t)S->ticks;
    node_goto(c->n, "out", 0, false);
    node_update_one(c->n);
    S->champ = -1;
    if (pick >= 0) {
      S->ch[pick].alpha = 1;
      S->ch[pick].active = 1;
      S->champ = (int8_t)pick;
      champ_node_state(pick);
    }
  } else if (S->nch) {
    S->champ = (int8_t)champ_pick(false);
#ifdef HOST
    Item it;
    if (getenv("SKATE_DEBUG") && S->champ >= 0 && item_at(S->mslots, S->ch[S->champ].off, MAT_ID, &it, 0))
      fprintf(stderr, "skate: champion %d of %d at %.1f %.1f z %.0f\n", S->champ, S->nch, it.m.tx, it.m.ty, it.s->z);
#endif
    for (int i = 0; i < S->nch; i++) {
      S->ch[i].alpha = S->ch[i].active = i == S->champ;
      champ_node_state(i);
    }
  }
}

/* ---------------------------------------------------------------- camera (Cq, Dq): the map follows the skater */
static void camera(void) {
  NodeId map = S->map;
  Mat l = node_local(map);
  float cx, cy;
  if (!mat_inv_apply(l, 480, 270, &cx, &cy)) return;
  float ox = S->px - cx, oy = S->py - cy;
  float gx = l.a * ox + l.c * oy, gy = l.b * ox + l.d * oy - .6f * S->z * 3;
  if (fabsf(gx) < .1f) gx = 0;
  if (fabsf(gy) < .1f) gy = 0;
  S->qx = nodes[map].x - gx;
  S->qy = nodes[map].y - gy + S->z / 2;   /* minus the target's offset (0, -z / 2) */
  uint16_t T = nodes[map].T;
  float ease = comp_float(T, C_camera, F_ease, .6f), speed = comp_float(T, C_camera, F_speed, 60);
  float dx = S->qx - nodes[map].x, dy = S->qy - nodes[map].y, d = vlen(dx, dy);
  if (d > 0) {
    float step = fminf(ease * d, speed) / d;
    nodes[map].x += dx * step;
    nodes[map].y += dy * step;
  }
}

static Mat map_screen(void) {
  Mat m = mat_mul((Mat){1.0f / 3, 0, 0, 1.0f / 3, 0, 0}, node_local(S->map));
  m.tx = floorf(m.tx + .5f);
  m.ty = floorf(m.ty + .5f);
  return m;
}

static Rect view_rect(Mat m) {
  float x0, y0, x1, y1;
  mat_inv_apply(m, 0, 0, &x0, &y0);
  mat_inv_apply(m, VIEW_W, VIEW_H, &x1, &y1);
  return (Rect){fminf(x0, x1), fminf(y0, y1), fabsf(x1 - x0), fabsf(y1 - y0)};
}

/* ---------------------------------------------------------------- the end ($t, Yo) */
static void timer(void) {
  if (S->over && --S->end_t <= 0 && !S->done) {
    S->done = true;
    menus_game_over((float)S->hf);
    return;
  }
  if (S->cn > 0) S->cn--;
  if (!S->over && S->cn == 0) {
    time_text(S->time_buf, sizeof S->time_buf, 0);
    toast(msg("TIMES_UP"));
    SOUND(whistle);
    S->over = true;
    S->end_t = END_DELAY;
  }
}

static void on_go(void) {
#ifdef HOST
  const char *tp = getenv("SKATE_TP");   /* tests: start somewhere else */
  float x, y, z = 0;
  if (tp && sscanf(tp, "%f,%f,%f", &x, &y, &z) >= 2) { S->px = x; S->py = y; S->z = z; }
#endif
  for (int i = 0; i < S->nsym; i++)
    if (S->sym[i].proto) zsprites(S->sym[i].proto, S->sym[i].proto, S->sym[i].z, 0);
  for (int i = 0; i < S->nch; i++)
    if (S->ch[i].n) {
      Item it;
      if (item_at(S->mslots, S->ch[i].off, MAT_ID, &it, 0)) zsprites(S->ch[i].n, S->ch[i].n, it.s->z, 0);
    }
}

static void tick(void) {
  S->ticks++;
  int d = mh(in.jx, in.jy);
  if (d != S->isc_dir) S->isc = 0;
  if (S->isc < 60000) S->isc++;
  S->isc_dir = (int8_t)d;
  sys_back_pauses();
  if (!S->tut_done) { S->tut_done = true; sys_tutorial_once(); }
  bool was = S->go;
  S->go = sys_countdown(&S->cd);
  if (S->go && !was) on_go();
  champs_sync(view_rect(map_screen()));
  triggers();
  if (!S->over) champion();
  physics();
  if (S->go) skater();
  player_place();
  if (!S->over) hud_update();
  tweens();
  camera();
  if (S->go) heights();
  if (S->go) timer();
  /* texts in another colour than their own are drawn by draw_over */
  if (S->time) nodes[S->time].flags = (uint8_t)(S->time_red ? nodes[S->time].flags & ~NF_VISIBLE : nodes[S->time].flags | NF_VISIBLE);
  if (S->tname) nodes[S->tname].flags = (uint8_t)(S->trick_red ? nodes[S->tname].flags & ~NF_VISIBLE : nodes[S->tname].flags | NF_VISIBLE);
  if (S->tscore) nodes[S->tscore].flags = (uint8_t)(S->trick_red ? nodes[S->tscore].flags & ~NF_VISIBLE : nodes[S->tscore].flags | NF_VISIBLE);
}

/* ---------------------------------------------------------------- drawing: the ground, then the children in view by Qp */
typedef struct { Rect view; Mat parent; } DrawCtx;

static void ground_item(uint16_t off, void *ctx) {
  DrawCtx *dc = ctx;
  Item it;
  if (!item_at(S->bslots, off, S->bgm, &it, 0) || (it.s->fl & SF_NODRAW) || !rect_intersects(it.box, dc->view)) return;
  draw_item(&it, dc->parent);
}

static float item_key(const Item *it) {
  const Sym *s = it->s;
  if (s->fl & SF_DORD) return s->dord;
  if (s->fl & SF_KEYB) {
    Rect b = item_bounds(it);
    return b.y + s->h + s->z;
  }
  return it->m.ty;
}

static void vis_add(float key, uint16_t off, uint16_t what) {
  if (S->nvis >= VIS_MAX) return;
  int i = S->nvis++;
  while (i > 0 && (S->vis[i - 1].key > key || (S->vis[i - 1].key == key && S->vis[i - 1].off > off))) {
    S->vis[i] = S->vis[i - 1];
    i--;
  }
  S->vis[i] = (Vis){key, off, what};
}

static void object_item(uint16_t off, void *ctx) {
  DrawCtx *dc = ctx;
  Item it;
  if (!item_at(S->mslots, off, MAT_ID, &it, 8)) {
    Key k;
    Mat lm;
    Rect r;
    if (loose_at(S->mslots, off, MAT_ID, &k, &lm, &r) && rect_intersects(r, dc->view)) vis_add(lm.ty, off, 0);
    return;
  }
  if (!rect_intersects(it.box, dc->view)) return;
  if (it.s->fl & SF_CHAMP) {
    for (int i = 0; i < S->nch; i++)
      if (S->ch[i].off == off) vis_add(it.m.ty, off, (uint16_t)(2 + i));
    return;
  }
  if (it.s->fl & SF_NODRAW) return;
  vis_add(item_key(&it), off, 0);
}

static void draw_under(void) {
  S->stamp++;
  Mat m = map_screen();
  S->mapm = m;
  Rect view = view_rect(m);
  DrawCtx dc = {view, m};
  /* the ground: park 1's big tiles are a 48 px tile repeated (drawn so, they fit the cache) */
  if (S->nkk) {
    for (int i = 0; i < S->nkk; i++) {
      float ox = S->kk[i][0], oy = S->kk[i][1];
      if (ox > view.x + view.w || oy > view.y + view.h || ox + 288 < view.x || oy + 288 < view.y) continue;
      for (int ty = 0; ty < 6; ty++)
        for (int tx = 0; tx < 6; tx++) {
          float x = ox + 48 * tx, y = oy + 48 * ty;
          if (x > view.x + view.w || y > view.y + view.h || x + 48 < view.x || y + 48 < view.y) continue;
          gfx_sprite(S->kk_sprite, (Mat){1, 0, 0, 1, m.tx + x, m.ty + y}, 255);
        }
    }
  }
  if (S->bslots) {
    DrawCtx g = {view, mat_mul(m, S->bgm)};
    index_query(&S->bi, view.x, view.y, view.x + view.w, view.y + view.h, ground_item, &g);
  }
  /* everything else, in draw order */
  S->nvis = 0;
  index_query(&S->mi, view.x - 8, view.y - 8, view.x + view.w + 8, view.y + view.h + 8, object_item, &dc);
  if (S->player) {
    vis_add(S->py + S->bcy - S->bhy + 20 + S->kt, S->player_off, 1);
  }
  for (int i = 0; i < S->nvis; i++) {
    const Vis *v = &S->vis[i];
    if (v->what == 1) node_draw(S->player, m);
    else if (v->what >= 2) {
      int c = v->what - 2;
      if (c < S->nch && S->ch[c].n) node_draw(S->ch[c].n, m);
    } else {
      Item it;
      Key k;
      Mat lm;
      Rect r;
      if (item_at(S->mslots, v->off, MAT_ID, &it, 0)) draw_item(&it, m);
      else if (loose_at(S->mslots, v->off, MAT_ID, &k, &lm, &r)) draw_loose(&k, m);
    }
  }
  /* the prompt's bar is a gradient, which the data does not keep: drawn in strips */
  if (S->prompt && node_visible_chain(S->prompt)) {
    Mat g = mat_mul(mat_mul((Mat){1.0f / 3, 0, 0, 1.0f / 3, 0, 0}, node_global(S->prompt)), (Mat){1.178f, 0, 0, 1, 0, 0});
    gfx_shape(S->grad, g, nodes[S->prompt].alpha);
  }
}

/* a HUD text in a colour of the code's (the time's red, a fall's red) */
static void text_in(NodeId n, uint16_t color) {
  if (!n || nodes[n].kind != NK_TEXT || nodes[n].ref == NONE16 || !nodes[n].alpha) return;
  node_text_draw(n, mat_mul((Mat){1.0f / 3, 0, 0, 1.0f / 3, 0, 0}, node_global(n)), NULL, color, nodes[n].alpha);
}

/* a sprite as a shape of one colour (the icons tinted green): a rectangle per run of pixels, built in
 * buf; returns its size, 0 if it does not fit */
static int sprite_shape(uint16_t sp, uint8_t *buf, int room, float fr, float fg, float fb) {
  const uint8_t *r = spr_get(sp);
  if (!r || room < 6) return 0;
  Sprite si;
  sprite_info(sp, &si);
  const uint16_t *pal = pal565(si.sheet);
  int w = rd16(r), h = rd16(r + 2), n = 6;
  uint16_t col = 0xFFFF;
  buf[0] = 1;
  buf[5] = 0;
  (void)w;
  for (int y = 0; y < h; y++) {
    const uint8_t *p = r + rd16(r + 4 + 2 * y);
    int runs = rd16(p), cx = 0;
    p += 2;
    for (int k = 0; k < runs; k++) {
      cx += *p++;
      int code = *p++;
      bool fill = code & 0x80;
      int len = fill ? (code & 0x7F) + 1 : code;
      if (col == 0xFFFF) col = pal[p[0]];
      p += fill ? 1 : len;
      if (n + 18 > room || buf[5] == 255) return 0;
      int16_t pts[8] = {(int16_t)(cx * 4), (int16_t)(y * 4), (int16_t)((cx + len) * 4), (int16_t)(y * 4),
                        (int16_t)((cx + len) * 4), (int16_t)((y + 1) * 4), (int16_t)(cx * 4), (int16_t)((y + 1) * 4)};
      buf[n++] = 4;
      buf[n++] = 0;
      for (int i = 0; i < 8; i++) { buf[n++] = (uint8_t)(pts[i] & 0xFF); buf[n++] = (uint8_t)((uint16_t)pts[i] >> 8); }
      buf[5]++;
      cx += len;
    }
  }
  /* RGB565 to 8 bits, times the filter */
  float cr = (col >> 11) * 255.0f / 31, cg = ((col >> 5) & 63) * 255.0f / 63, cb = (col & 31) * 255.0f / 31;
  buf[1] = (uint8_t)(cr * fr + .5f);
  buf[2] = (uint8_t)(cg * fg + .5f);
  buf[3] = (uint8_t)(cb * fb + .5f);
  buf[4] = 255;
  return n;
}

static void draw_over(void) {
  /* the green icons of a trick (the vis list is free once draw_under is done) */
  uint8_t *buf = (uint8_t *)S->vis;
  int used = 0;
  uint16_t made[2] = {NONE16, NONE16}, at[2] = {0, 0};
  for (int i = 0; i < ICON_MAX; i++) {
    Icon *ic = &S->icon[i];
    if (!ic->used || !ic->green || !ic->n || !nodes[ic->n].alpha) continue;
    Clip c;
    Key k;
    SymInfo si;
    if (!clip_get(nodes[ic->n].sym, &c)) continue;
    const uint8_t *q = c.slots, *nx;
    bool found = false;
    for (unsigned sl = 0; sl < c.nslots && !found; sl++, q = nx)
      if (slot_at(q, nodes[ic->n].frame, &k, &nx) && k.kind == CK_SYM) found = true;
    if (!found || (sym_info(k.ref, &si), si.type != SYM_BITMAP)) continue;
    uint16_t sp = (uint16_t)si.v;
    int m = made[0] == sp ? 0 : made[1] == sp ? 1 : -1;
    if (m < 0) {
      m = made[0] == NONE16 ? 0 : made[1] == NONE16 ? 1 : -1;
      int n = m < 0 ? 0 : sprite_shape(sp, buf + used, (int)sizeof S->vis - used, .2f, 1, .2f);
      if (!n) continue;
      made[m] = sp;
      at[m] = (uint16_t)used;
      used += (n + 3) & ~3;
    }
    Mat g = mat_mul(mat_mul((Mat){1.0f / 3, 0, 0, 1.0f / 3, 0, 0}, node_global(ic->n)), key_mat(&k));
    gfx_shape(buf + at[m], g, (uint8_t)(nodes[ic->n].alpha * k.alpha / 255));
  }
  if (S->time_red) text_in(S->time, rgb565(0xff, 0x30, 0x30));
  if (S->trick_red) {
    text_in(S->tscore, rgb565(0xff, 0x11, 0x11));
    text_in(S->tname, rgb565(0xff, 0x11, 0x11));
  }
}

/* ---------------------------------------------------------------- start */
static NodeId hud_text(const char *name, const char *s) {
  NodeId n = node_child(S->hud, name);
  if (n && s) node_set_text(n, s);
  return n;
}

static void start(void) {
  S = scene_state(sizeof(State));
  node_draw_hook_id = 0;
  gfx_clear_color(0);
  S->champ = -1;
  S->isc_dir = -1;
  S->cn = TIME_START;
  S->tb = true;
  S->dir = DIR_S;
  S->gravity = -300;   /* au: the cannon world of this scene */
  S->tr_alpha = 0;
  S->tr_t = 255;
  sys_countdown_start(&S->cd);
  /* iba: lf(["rgba(0,0,0,0)", "rgba(0,0,0,0.427)", "rgba(0,0,0,0)"], [0, .518, 1]) from x -118 to 118, y -24 to 24 */
  {
    uint8_t *g = S->grad;
    *g++ = GRAD_STRIPS;
    for (int i = 0; i < GRAD_STRIPS; i++) {
      float x0 = -118 + 236.0f * i / GRAD_STRIPS, x1 = -118 + 236.0f * (i + 1) / GRAD_STRIPS, t = (i + .5f) / GRAD_STRIPS;
      float a = t < .518f ? .427f * t / .518f : .427f * (1 - t) / (1 - .518f);
      *g++ = 0; *g++ = 0; *g++ = 0; *g++ = (uint8_t)(a * 255 + .5f); *g++ = 1;
      *g++ = 4; *g++ = 0;
      int16_t pts[8] = {(int16_t)(x0 * 4), -96, (int16_t)(x1 * 4), -96, (int16_t)(x1 * 4), 96, (int16_t)(x0 * 4), 96};
      for (int k = 0; k < 8; k++) { *g++ = (uint8_t)(pts[k] & 0xFF); *g++ = (uint8_t)((uint16_t)pts[k] >> 8); }
    }
  }
  /* the root clip ycb: one frame per park, each with its map and the HUD */
  Clip rc;
  clip_frame0(S_skate_ycb, &rc);
  int frame = game.variant[0] ? clip_label(&rc, game.variant) : 0;
  if (frame < 0) frame = 0;
  Key mk = {0}, hk = {0};
  bool has_map = false, has_hud = false;
  const uint8_t *p = rc.slots;
  for (unsigned s = 0; s < rc.nslots; s++) {
    Key k;
    const uint8_t *nx;
    bool on = slot_at(p, (unsigned)frame, &k, &nx);
    p = nx;
    if (!on || k.kind != CK_SYM) continue;
    if (k.ref == S_skate_lma) { hk = k; has_hud = true; }
    else {
      Clip c;
      clip_frame0(k.ref, &c);
      if (comp_has(c.T, C_map)) { mk = k; has_map = true; }
    }
  }
  if (!has_map) return;
  S->map_sym = mk.ref;
  S->map = node_new_sym_lazy(mk.ref);
  if (!S->map) return;
  node_add(game.root, S->map);
  place(S->map, &mk);
  nodes[S->map].flags &= (uint8_t)~NF_VISIBLE;   /* drawn by draw_under */
  Clip mc;
  clip_frame0(mk.ref, &mc);
  S->mslots = mc.slots;
  /* the map's children: the skater, the ground, champions; then the index */
  p = mc.slots;
  for (unsigned s = 0; s < mc.nslots; s++) {
    uint16_t off = (uint16_t)(p - mc.slots);
    Key k;
    const uint8_t *nx;
    bool on = slot_at(p, 0, &k, &nx);
    p = nx;
    if (!on || k.kind != CK_SYM) continue;
    Sym *sy = sym_get(k.ref);
    if (!sy) continue;
    Mat km = key_mat(&k);
    if ((sy->fl & SF_PLAYER) && !S->player) {
      S->player_off = off;
      S->player = node_new_sym(k.ref);
      if (!S->player) continue;
      node_add(S->map, S->player);
      place(S->player, &k);
      first_stops(S->player, 0);
      S->px = km.tx;
      S->py = km.ty;
      S->z = sy->z;
      S->bcx = F8(sy->b[0] + sy->b[2]) / 2;
      S->bcy = F8(sy->b[1] + sy->b[3]) / 2;
      S->bhx = F8(sy->b[2] - sy->b[0]) / 2;
      S->bhy = F8(sy->b[3] - sy->b[1]) / 2;
    } else if (sy->fl & SF_GROUND) {
      /* Kk: its one bitmap, 288 px of the 48 px tile eGa */
      Clip kc;
      clip_frame0(k.ref, &kc);
      Key bk;
      if (S->nkk < KK_MAX && kc.nslots && slot_at(kc.slots, 0, &bk, NULL)) {
        S->kk[S->nkk][0] = (int16_t)floorf(km.tx + bk.x + .5f);
        S->kk[S->nkk][1] = (int16_t)floorf(km.ty + bk.y + .5f);
        S->nkk++;
      }
    } else if ((sy->fl & SF_BG) && !S->bslots) {
      Clip bc;
      clip_frame0(k.ref, &bc);
      S->bg_sym = k.ref;
      S->bslots = bc.slots;
      S->bgm = km;
      index_build(&S->bi, bc.slots, bc.nslots, km, 96, true);
    } else if ((sy->fl & SF_CHAMP) && S->nch < CH_MAX) {
      S->ch[S->nch++] = (Champ){off, 0, 0, 0, 0, 0};
    }
  }
  if (S->nkk) {
    SymInfo si;
    sym_info(S_skate_eGa, &si);
    S->kk_sprite = (uint16_t)si.v;
  }
  index_build(&S->mi, mc.slots, mc.nslots, MAT_ID, 128, false);
#ifdef HOST
  if (getenv("SKATE_DEBUG"))
    fprintf(stderr, "skate: %u slots, %u syms, %u entries, cells %ux%u+%ux%u (max %.0fx%.0f, %.0fx%.0f), nodes %u, state %u\n", mc.nslots, S->nsym, S->nent,
            S->mi.nx, S->mi.ny, S->bi.nx, S->bi.ny, S->mi.maxw, S->mi.maxh, S->bi.maxw, S->bi.maxh, node_count(), (unsigned)sizeof(State));
#endif
  if (S->player) ent_register_tree(S->player);
  /* the HUD */
  if (has_hud) {
    S->hud = node_new_sym(S_skate_lma);
    if (S->hud) {
      node_add(game.root, S->hud);
      place(S->hud, &hk);
      first_stops(S->hud, 0);
      S->score = hud_text("score", S->score_buf);
      S->score_sh = hud_text("scoreShadow", S->score_buf);
      S->time = hud_text("time", S->time_buf);
      S->time_sh = hud_text("timeShadow", S->time_buf);
      S->tname = hud_text("trickName", S->tname_buf);
      S->tscore = hud_text("trickScore", S->tscore_buf);
      S->spark_l = node_child(S->hud, "sparkleLeft");
      S->spark_r = node_child(S->hud, "sparkleRight");
      S->prompt = node_child(S->hud, "prompt");
      snprintf(S->score_buf, sizeof S->score_buf, "0");
      time_text(S->time_buf, sizeof S->time_buf, S->cn);
    }
  }
  /* the camera starts where the map is placed and eases to the skater (no Eq here) */
}

const SceneDef scene_skate = {"skate", start, tick, NULL, draw_under, draw_over, NULL};
