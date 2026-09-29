/* The island (overworld) and the insides of buildings (interior).
 *
 * The island is the doodle's map "jqa": 24 regions of scenery and the people,
 * doors and signs in them, some 9000 display objects in all. pack.py split it
 * in two. Its static scenery (the "statics": ground, trees, houses, fences...,
 * in the doodle's draw order) is painted into the background layer as the
 * camera uncovers it. The rest, what moves, talks or reacts, became the map's
 * own children, instantiated only near the camera (node_stream). A static
 * standing in front of someone (lower on screen) is drawn again over them.
 *
 * The systems are the doodle's overworld scene ($s) and interior scene (Ir). */
#include <math.h>
#include <stdio.h>
#ifdef HOST
#include <stdlib.h>
#endif
#include "ent.h"
#include "phys.h"
#include "spr.h"

/* ---------------------------------------------------------------- the world table (pack.py) */
typedef struct {
  const uint8_t *cells, *list, *wcells, *wlist, *stoffs, *stbase, *walls, *regions, *lut;
  uint16_t map_sym, nstatic, nwalls, nregions, sheet, cell, gw, gh;
  int16_t gx0, gy0;
} WorldData;
static WorldData wd;

static bool world_load(uint16_t map_sym) {
  memset(&wd, 0, sizeof wd);
  uint32_t off = HDR(H_WORLDS);
  if (!off) return false;
  const uint8_t *t = ci_data + off;
  for (uint32_t i = 0; i < rd32(t); i++) {
    const uint8_t *w = ci_data + rd32(t + 4 + 4 * i);
    if (rd16(w) != map_sym) continue;
    wd.map_sym = map_sym;
    wd.nstatic = rd16(w + 2);
    wd.nwalls = rd16(w + 4);
    wd.nregions = rd16(w + 6);
    wd.sheet = rd16(w + 8);
    wd.cell = rd16(w + 10);
    wd.gx0 = rds16(w + 12);
    wd.gy0 = rds16(w + 14);
    wd.gw = rd16(w + 16);
    wd.gh = rd16(w + 18);
    const uint8_t **ptrs[] = {&wd.cells, &wd.list, &wd.wcells, &wd.wlist, &wd.stoffs, &wd.stbase, &wd.walls, &wd.regions, &wd.lut};
    for (int k = 0; k < 9; k++) *ptrs[k] = ci_data + rd32(w + 20 + 4 * k);
    return true;
  }
  return false;
}

/* a static: f32 key, i16 x0 y0, u16 w h (its box), u8 draws, u8 flags (1: solid), [i16 solid box], draws */
typedef struct { float key; int16_t x0, y0, x1, y1; uint8_t ndraws, flags; int16_t c[4]; const uint8_t *draws; } Static;
static void static_get(unsigned i, Static *s) {
  const uint8_t *p = wd.stbase + 4u * rd16(wd.stoffs + 2 * i);
  s->key = rdf(p);
  s->x0 = rds16(p + 4); s->y0 = rds16(p + 6);
  s->x1 = (int16_t)(s->x0 + rd16(p + 8)); s->y1 = (int16_t)(s->y0 + rd16(p + 10));
  s->ndraws = p[12];
  s->flags = p[13];
  p += 14;
  if (s->flags & 1) {
    for (int k = 0; k < 4; k++) s->c[k] = rds16(p + 2 * k);
    p += 8;
  }
  s->draws = p;
}

/* a draw of a static: u16 sprite, u8 flags (0-2 BD_*, 8: alpha follows, 16: 16-bit offsets), offsets from the box, [alpha] */
typedef struct { uint16_t sp; int16_t x, y; uint8_t flags, alpha; } Draw;
static const uint8_t *draw_next(const uint8_t *p, const Static *s, Draw *d) {
  d->sp = rd16(p);
  uint8_t f = p[2];
  p += 3;
  if (f & 16) { d->x = (int16_t)(s->x0 + rd16(p)); d->y = (int16_t)(s->y0 + rd16(p + 2)); p += 4; }
  else { d->x = (int16_t)(s->x0 + p[0]); d->y = (int16_t)(s->y0 + p[1]); p += 2; }
  d->alpha = 255;
  if (f & 8) d->alpha = *p++;
  d->flags = f & 7;
  return p;
}

static void wall_get(unsigned i, int16_t c[4]) {
  for (int k = 0; k < 4; k++) c[k] = rds16(wd.walls + 8 * i + 2 * k);
}

#define STATIC_BITS 2048
/* marks the ids of a grid (statics or walls) whose cells meet the world rect */
static int cell_at(float v, float o) {
  float c = floorf((v - o) / wd.cell);
  return !(c >= -1) ? -1 : c > 4096 ? 4096 : (int)c;   /* NaN and far away too */
}
static int grid_in(const uint8_t *cells, const uint8_t *list, float x0, float y0, float x1, float y1, uint8_t *bits, int *lo, int *hi) {
  int cx0 = cell_at(x0, wd.gx0), cy0 = cell_at(y0, wd.gy0);
  int cx1 = cell_at(x1, wd.gx0), cy1 = cell_at(y1, wd.gy0);
  if (cx0 < 0) cx0 = 0;
  if (cy0 < 0) cy0 = 0;
  if (cx1 >= wd.gw) cx1 = wd.gw - 1;
  if (cy1 >= wd.gh) cy1 = wd.gh - 1;
  int n = 0;
  *lo = STATIC_BITS;
  *hi = -1;
  if (!wd.cell) return 0;
  for (int cy = cy0; cy <= cy1; cy++)
    for (int cx = cx0; cx <= cx1; cx++) {
      unsigned c = (unsigned)(cy * wd.gw + cx);
      for (unsigned k = rd16(cells + 2 * c); k < rd16(cells + 2 * c + 2); k++) {
        int id = rd16(list + 2 * k);
        if (id >= STATIC_BITS || (bits[id >> 3] & (1 << (id & 7)))) continue;
        bits[id >> 3] |= (uint8_t)(1 << (id & 7));
        if (id < *lo) *lo = id;
        if (id > *hi) *hi = id;
        n++;
      }
    }
  return n;
}

static int statics_in(float x0, float y0, float x1, float y1, uint8_t *bits, int *lo, int *hi) {
  return grid_in(wd.cells, wd.list, x0, y0, x1, y1, bits, lo, hi);
}

/* ---------------------------------------------------------------- scene state */
#define WATER 48
#define FRONT_MAX 40
#define SOLID_MAX 40
#define RAIN_MAX 48
#define PETAL_MAX 80
typedef struct {
  uint8_t water[WATER * WATER / 2];    /* the sea's tile now (w9a: frames 0-15, 16-31), 4 bits a pixel */
  uint16_t water_col[16];              /* their colours (it has 10) */
  int water_look;                      /* which, -1 before the first */
  NodeId map, player, water_clip;
  float rain;                          /* bca's alpha (drawn by code, draw_over) */
  uint16_t rain_cond;                  /* its conditionallyVisible */
  int16_t rain_pos[2 * RAIN_MAX];      /* its tiles */
  int rain_n;
  bool ending;                         /* the outro was seen: the petals and the glow */
  int16_t petal_pos[2 * PETAL_MAX];
  int npetal;
  uint32_t tick;
  int roll;                            /* Bi.Jaa: ticks left in a roll */
  float stream_x, stream_y;            /* camera when the live children were last streamed */
  bool streamed;
  /* solid statics near the player, as physics bodies */
  uint16_t solid_id[SOLID_MAX];
  int16_t solid_body[SOLID_MAX];
  int nsolid;
  float solid_x, solid_y;
  bool solid_valid;
  /* per NPC: the doodle's R_ (talked since the button went up) */
  NodeId talked;
  int region_rain;                     /* index of region bT (the rainy mountain), -1 */
  uint8_t npc_beeps;
} World;
static World *W;
/* the island's layer (the screen and a margin) fits, with its cache */
#define LAYER_W (VIEW_W + 8)
#define LAYER_H (SCREEN_H + 4)
_Static_assert(sizeof(World) + LAYER_W * LAYER_H + MIN_CACHE <= ARENA_BYTES, "the island's layer does not fit");

/* ---------------------------------------------------------------- the layer */
static void paint(int x0, int y0, int w, int h) {
  uint8_t bits[STATIC_BITS / 8];
  memset(bits, 0, sizeof bits);
  int lo, hi;
  if (!statics_in((float)x0, (float)y0, (float)(x0 + w), (float)(y0 + h), bits, &lo, &hi)) return;
  for (int id = lo; id <= hi; id++) {
    if (!(bits[id >> 3] & (1 << (id & 7)))) continue;
    Static s;
    static_get((unsigned)id, &s);
    if (s.x1 <= x0 || s.y1 <= y0 || s.x0 >= x0 + w || s.y0 >= y0 + h) continue;
    const uint8_t *p = s.draws;
    for (int d = 0; d < s.ndraws; d++) {
      Draw dr;
      p = draw_next(p, &s, &dr);
      bg_draw(dr.sp, dr.x, dr.y, dr.flags, dr.alpha);
    }
  }
}

/* the camera's view in map space (the map is the camera, drawn at 3x on the 960x540 stage) */
static void view_origin(float *x, float *y) {
  Mat m = node_global(W->map);
  /* stage (0, 0) in map space */
  float det = m.a * m.d - m.b * m.c;
  *x = (-m.d * m.tx + m.c * m.ty) / det;
  *y = (m.b * m.tx - m.a * m.ty) / det;
}

/* ---------------------------------------------------------------- drawing the map */
static float draw_key(NodeId c) {
  if (nodes[c].ent && comp_has(nodes[c].T, C_drawOrderOverride)) return comp_float(nodes[c].T, C_drawOrderOverride, F_drawOrder, 0);
  float x, y;
  ent_pos(c, &x, &y);
  return y;
}

static void draw_static_over(unsigned id, Mat m, uint8_t alpha) {
  Static s;
  static_get(id, &s);
  const uint8_t *p = s.draws;
  for (int d = 0; d < s.ndraws; d++) {
    Draw dr;
    p = draw_next(p, &s, &dr);
    if (dr.alpha != 255) continue;      /* see-through parts (shadows) are already in the layer */
    Sprite si;
    sprite_info(dr.sp, &si);
    if (si.kind > 1) continue;          /* big ground images never stand in front */
    int x = dr.x, y = dr.y;
    uint8_t f = dr.flags;
    if (!(f & BD_TRANSPOSE) && alpha == 255) { bg_redraw(dr.sp, x, y, f); continue; }
    /* the drawn rectangle's corner, back to a matrix */
    Mat dm;
    if (f & BD_TRANSPOSE) {
      float c = f & BD_FLIPX ? -1.f : 1.f, b = f & BD_FLIPY ? -1.f : 1.f;
      dm = (Mat){0, b, c, 0, c < 0 ? x + si.h : x, b < 0 ? y + si.w : y};
    } else {
      float a = f & BD_FLIPX ? -1.f : 1.f, dd = f & BD_FLIPY ? -1.f : 1.f;
      dm = (Mat){a, 0, 0, dd, a < 0 ? x + si.w : x, dd < 0 ? y + si.h : y};
    }
    gfx_sprite_ex(dr.sp, mat_mul(m, dm), alpha, true);
  }
}

/* the map's children in draw order, with the statics that belong in front of them */
static void draw_map(NodeId map, Mat m, uint8_t alpha) {
  float vx, vy;
  view_origin(&vx, &vy);
  float vx1 = vx + VIEW_W, vy1 = vy + gfx_view_bottom();
  vy += gfx_view_top();
  uint8_t bits[STATIC_BITS / 8];
  memset(bits, 0, sizeof bits);
  uint16_t front[FRONT_MAX];
  int nfront = 0;
  for (NodeId c = nodes[map].first; c; c = nodes[c].next) {
    if (!node_visible(c) || !nodes[c].alpha) continue;
    float bx, by, bw, bh;
    if (!node_bounds_in(c, map, &bx, &by, &bw, &bh) || bw <= 0 || bh <= 0) continue;
    if (bx >= vx1 || by >= vy1 || bx + bw <= vx || by + bh <= vy) continue;
    float key = draw_key(c);
    if (key < -1000) continue;            /* ground things: nothing static is below them */
    uint8_t cb[STATIC_BITS / 8];
    memset(cb, 0, sizeof cb);
    int lo, hi;
    if (!statics_in(bx, by, bx + bw, by + bh, cb, &lo, &hi)) continue;
    for (int id = lo; id <= hi && nfront < FRONT_MAX; id++) {
      if (!(cb[id >> 3] & (1 << (id & 7))) || (bits[id >> 3] & (1 << (id & 7)))) continue;
      Static s;
      static_get((unsigned)id, &s);
      if (s.key <= key || s.x1 <= bx || s.y1 <= by || s.x0 >= bx + bw || s.y0 >= by + bh) continue;
      bits[id >> 3] |= (uint8_t)(1 << (id & 7));
      /* in draw order */
      int j = nfront++;
      while (j > 0 && front[j - 1] > id) { front[j] = front[j - 1]; j--; }
      front[j] = (uint16_t)id;
    }
  }
  /* and what is in front of those (a bamboo before a house before the player) */
#ifndef NO_CLOSURE
  for (int i = 0; i < nfront && nfront < FRONT_MAX; i++) {
    Static fs;
    static_get(front[i], &fs);
    uint8_t cb[STATIC_BITS / 8];
    memset(cb, 0, sizeof cb);
    int lo, hi;
    if (!statics_in(fs.x0, fs.y0, fs.x1, fs.y1, cb, &lo, &hi)) continue;
    for (int id = front[i] + 1; id <= hi && nfront < FRONT_MAX; id++) {
      if (!(cb[id >> 3] & (1 << (id & 7))) || (bits[id >> 3] & (1 << (id & 7)))) continue;
      Static s;
      static_get((unsigned)id, &s);
      if (s.x1 <= fs.x0 || s.y1 <= fs.y0 || s.x0 >= fs.x1 || s.y0 >= fs.y1 || s.x0 >= vx1 || s.y0 >= vy1 || s.x1 <= vx || s.y1 <= vy) continue;
      bits[id >> 3] |= (uint8_t)(1 << (id & 7));
      int j = nfront++;
      while (j > i + 1 && front[j - 1] > id) { front[j] = front[j - 1]; j--; }
      front[j] = (uint16_t)id;
    }
  }
#endif
  int f = 0;
  for (NodeId c = nodes[map].first; c; c = nodes[c].next) {
    if (f < nfront && node_visible(c)) {
      float key = draw_key(c);
      while (f < nfront) {
        Static s;
        static_get(front[f], &s);
        if (s.key >= key) break;
        draw_static_over(front[f++], m, alpha);
      }
    }
    node_draw_in(c, m, alpha);
  }
  while (f < nfront) draw_static_over(front[f++], m, alpha);
}

/* ---------------------------------------------------------------- live children */
static void on_trigger(NodeId t, NodeId other, bool entered);

static void translate_tree(NodeId n) {
  ent_translate(n);
  for (NodeId c = nodes[n].first; c; c = nodes[c].next) translate_tree(c);
}

static void streamed_in(NodeId c) {
  ent_register_tree(c);
  translate_tree(c);
  uint16_t T = nodes[c].T;
  if (T == NONE16) return;
  /* sr: places are markers for the designers */
  if (comp_has(T, C_location) && !comp_has(T, C_boundable)) nodes[c].alpha = 0;
  /* Qs: one-frame children do not need ticking (kept as is) */
}

static void stream_children(bool force) {
  float vx, vy;
  view_origin(&vx, &vy);
  if (!force && W->streamed && fabsf(vx - W->stream_x) < 16 && fabsf(vy - W->stream_y) < 16) return;
  W->stream_x = vx;
  W->stream_y = vy;
  W->streamed = true;
  node_stream(W->map, vx - 40, vy + gfx_view_top() - 40, vx + VIEW_W + 40, vy + gfx_view_bottom() + 40);
}

/* ---------------------------------------------------------------- places (location markers) */
typedef struct { const char *want; float px, py; float best; bool found; char name[32]; } PlaceQuery;

/* the map's timeline children that are places: their name and where (bounds bottom centre, else origin) */
typedef struct { void (*fn)(const char *name, float x, float y, void *ctx); void *ctx; } PlaceIter;
static bool place_slot(unsigned slot, const SlotInfo *si, void *ctx) {
  PlaceIter *it = ctx;
  Clip cc;
  if (!clip_get(si->sym, &cc) || cc.T == NONE16 || !comp_has(cc.T, C_location)) return true;
  uint16_t nm = comp_str(cc.T, C_location, F_name);
  if (nm == NONE16) return true;
  bool box = comp_has(cc.T, C_boundable) && si->has_bounds && si->bw > 0;
  it->fn(str(nm), box ? si->bx + si->bw / 2 : si->x, box ? si->by + si->bh : si->y, it->ctx);
  (void)slot;
  return true;
}
static void each_place(void (*fn)(const char *name, float x, float y, void *ctx), void *ctx) {
  PlaceIter it = {fn, ctx};
  clip_each_slot(nodes[W->map].sym, 0, place_slot, &it);
}

static void find_place(const char *name, float x, float y, void *ctx) {
  PlaceQuery *q = ctx;
  if (q->want) {
    if (!q->found && !strcmp(name, q->want)) { q->px = x; q->py = y; q->found = true; }
    return;
  }
  float d = (x - q->px) * (x - q->px) + (y - q->py) * (y - q->py);
  if (!q->found || d < q->best) { q->best = d; q->found = true; snprintf(q->name, sizeof q->name, "%s", name); }
}

/* fr: to a place */
static void teleport(const char *name) {
  PlaceQuery q = {name, 0, 0, 0, false, ""};
  each_place(find_place, &q);
  if (q.found && W->player) ent_set_pos(W->player, q.px, q.py);
}

/* er: remember the nearest place (where the game starts next time) */
static void save_place(void) {
  if (!W->player) return;
  PlaceQuery q = {NULL, 0, 0, 0, false, ""};
  ent_pos(W->player, &q.px, &q.py);
  float px = q.px, py = q.py;
  each_place(find_place, &q);
  if (q.found) store_set_str("PLAYER_LOC", q.name);
  store_set_num("PLAYER_POS_X", px);
  store_set_num("PLAYER_POS_Y", py);
}

void overworld_teleport(const char *location) {
  if (W && W->map && location && location[0]) teleport(location);
}

/* ---------------------------------------------------------------- physics near the player */
/* the walls and solid statics near the player are bodies (ids: statics as is, walls + 0x8000) */
static bool solid_box(uint16_t id, int16_t c[4]) {
  if (id & 0x8000) { wall_get(id & 0x7FFF, c); return true; }
  Static s;
  static_get(id, &s);
  memcpy(c, s.c, sizeof s.c);
  return s.flags & 1;
}

static void solid_add(uint16_t id, int type) {
  for (int i = 0; i < W->nsolid; i++) if (W->solid_id[i] == id) return;
  if (W->nsolid >= SOLID_MAX) return;
  int16_t c[4];
  if (!solid_box(id, c)) return;
  float hw = (c[2] - c[0]) * .5f, hh = (c[3] - c[1]) * .5f;
  int b = phys_add(type, SH_BOX, c[0] + hw, c[1] + hh, 50, hw, hh, 50);
  if (b < 0) return;
  bodies[b].user = 0;
  W->solid_id[W->nsolid] = id;
  W->solid_body[W->nsolid++] = (int16_t)b;
}

static void solids_update(void) {
  if (!W->player) return;
  float px, py;
  ent_pos(W->player, &px, &py);
  if (W->solid_valid && fabsf(px - W->solid_x) < 16 && fabsf(py - W->solid_y) < 16) return;
  W->solid_valid = true;
  W->solid_x = px;
  W->solid_y = py;
  const float R = 56;
  /* drop the far ones */
  for (int i = 0; i < W->nsolid; i++) {
    int16_t c[4];
    solid_box(W->solid_id[i], c);
    if (c[2] < px - R - 32 || c[0] > px + R + 32 || c[3] < py - R - 32 || c[1] > py + R + 32) {
      phys_remove(W->solid_body[i]);
      W->solid_id[i] = W->solid_id[--W->nsolid];
      W->solid_body[i] = W->solid_body[W->nsolid];
      i--;
    }
  }
  int type = phys_type("prop");
  uint8_t bits[STATIC_BITS / 8];
  int lo, hi;
  for (int pass = 0; pass < 2; pass++) {
    memset(bits, 0, sizeof bits);
    if (!grid_in(pass ? wd.wcells : wd.cells, pass ? wd.wlist : wd.list, px - R, py - R, px + R, py + R, bits, &lo, &hi)) continue;
    for (int id = lo; id <= hi; id++) {
      if (!(bits[id >> 3] & (1 << (id & 7)))) continue;
      uint16_t sid = (uint16_t)(pass ? id | 0x8000 : id);
      int16_t c[4];
      if (!solid_box(sid, c) || c[2] <= px - R || c[0] >= px + R || c[3] <= py - R || c[1] >= py + R) continue;
      solid_add(sid, type);
    }
  }
}

/* ---------------------------------------------------------------- the sea */
static uint16_t sprite_of(uint16_t sym) {
  SymInfo si;
  sym_info(sym, &si);
  return si.type == SYM_BITMAP ? (uint16_t)si.v : NONE16;
}

extern uint8_t spr_rowbuf[SPRITE_W_MAX];

static void water_make(int look) {
  /* w9a: the tile NewLargeWater1Art2 (frames 0-15), with Art1 over it (frames 16-31) */
  const uint16_t spr[2] = {sprite_of(S_overworld_Mf), sprite_of(S_overworld_Uf)};
  const uint16_t *pal = pal565(SHEET_OVERWORLD);
  const uint8_t *al = palalpha(SHEET_OVERWORLD);
  int clear = bg_clear_index();
  /* colour 0 is the clear one, the others come as the tile shows them */
  uint8_t used[16];
  int nused = 1;
  used[0] = (uint8_t)(clear < 0 ? 0 : clear);
  memset(W->water, 0, sizeof W->water);
  /* straight from the decoder: the cache keeps what the frames draw */
  for (int k = 0; k <= look; k++) {
    Sprite si;
    if (spr[k] == NONE16) continue;
    sprite_info(spr[k], &si);
    if (si.w > SPRITE_W_MAX || !spr_rows_open(spr[k], &si)) continue;
    for (int v = 0; v < si.h && v < WATER; v++) {
      if (!z_get(spr_rowbuf, si.w)) break;
      for (int u = 0; u < si.w && u < WATER; u++) {
        uint8_t c = spr_rowbuf[u];
        if (al[c] < 128) continue;
        int j = 0;
        while (j < nused && used[j] != c) j++;
        if (j == nused) {
          if (nused < 16) used[nused++] = c;
          else j = nused - 1;          /* more than 16 colours (never): the last one */
        }
        uint8_t *b = &W->water[(v * WATER + u) >> 1];
        *b = (uint8_t)(u & 1 ? (*b & 0x0F) | j << 4 : (*b & 0xF0) | j);
      }
    }
  }
  for (int j = 0; j < 16; j++) W->water_col[j] = pal[used[j < nused ? j : 0]];
  W->water_look = look;
}

/* ---------------------------------------------------------------- systems */
/* Fp: Back opens the map (the stats menu), or offers to skip the tutorial */
static void sys_back_map(void) {
  if (!in.pressed[A_BACK]) return;
  input_consume(A_BACK);
  if (store_bool("TUTORIAL_DONE", false)) menus_open("stats");
  else menus_open("skip");
}

/* gr: walking and rolling */
static void sys_overworld_player(void) {
  NodeId p = W->player;
  if (!p) return;
  Ent *e = ent_get(p);
  if (!e) return;
  float len = sqrtf(in.jx * in.jx + in.jy * in.jy);
  float h = roundf(8 * len) / 8;
  if (W->roll > 0) W->roll--;
  else if (len > 0) {
    int d = dir_of(in.jx, in.jy, false);
    if (in.pressed[A_ACTION]) {
      ent_label(p, "roll", false);
      W->roll = 9;
      e->vx = in.jx / len * 5;
      e->vy = in.jy / len * 5;
      SOUND(uja);
    } else {
      ent_label(p, "walk", false);
      e->vx = in.jx / len * 3 * h;
      e->vy = in.jy / len * 3 * h;
    }
    if (d >= 0) e->dir = (int8_t)d;
  } else {
    ent_label(p, "idle", false);
    e->vx = e->vy = 0;
  }
}

static const uint8_t COND_F[10] = {F_condition1, F_condition2, F_condition3, F_condition4, F_condition5,
                                   F_condition6, F_condition7, F_condition8, F_condition9, F_condition10};
static const uint8_t FRAME_F[5] = {F_frame1, F_frame2, F_frame3, F_frame4, F_frame5};
static const uint8_t NODE_F[10] = {F_node1, F_node2, F_node3, F_node4, F_node5, F_node6, F_node7, F_node8, F_node9, F_node10};

/* Er: storageSprite shows the frame of its first true condition (a quarter of them per tick) */
static void sys_storage_sprites(void) {
  for (int i = 0; i < ent_count(); i++) {
    NodeId n = ent_at(i);
    if (!n || !comp_has(nodes[n].T, C_storageSprite)) continue;
    if (W->tick && n % 4 != W->tick % 4) continue;
    uint16_t T = nodes[n].T;
    for (int k = 0; k < 5; k++) {
      uint16_t fr = comp_str(T, C_storageSprite, FRAME_F[k]);
      if (fr == NONE16 || !str(fr)[0]) continue;
      uint16_t cond = comp_str(T, C_storageSprite, COND_F[k]);
      if (cond == NONE16 || !str(cond)[0] || cond_eval(cond)) {
        ent_label(n, str(fr), false);
        break;
      }
    }
  }
}

/* Dr: conditionallyVisible */
static void sys_conditional(void) {
  for (int i = 0; i < ent_count(); i++) {
    NodeId n = ent_at(i);
    if (!n || !comp_has(nodes[n].T, C_conditionallyVisible)) continue;
    uint16_t cond = comp_str(nodes[n].T, C_conditionallyVisible, F_condition);
    node_set_visible(n, cond == NONE16 || !str(cond)[0] || cond_eval(cond));
  }
}

static bool triggered(NodeId n) {
  Ent *e = ent_peek(n);                /* a trigger has one (sys_triggers) */
  return e && e->ntrig > 0;
}

/* a whole string as a decimal number (newlib's strtof would pull in abort) */
static bool parse_num(const char *s, float *out) {
  bool neg = *s == '-';
  if (neg || *s == '+') s++;
  if (!*s) return false;
  float v = 0, scale = 1;
  bool dot = false, digits = false;
  for (; *s; s++) {
    if (*s == '.' && !dot) { dot = true; continue; }
    if (*s < '0' || *s > '9') return false;
    digits = true;
    if (dot) { scale /= 10; v += (float)(*s - '0') * scale; }
    else v = v * 10 + (float)(*s - '0');
  }
  if (!digits) return false;
  *out = neg ? -v : v;
  return true;
}

/* a storage value as kitsune's kk() reads it: number, true, false, null, else text */
static void store_parsed(const char *key, const char *v) {
  if (key[0] == '$') key++;
  float f;
  if (parse_num(v, &f)) store_set_num(key, f);
  else if (!strcmp(v, "true") || !strcmp(v, "True")) store_set_bool(key, true);
  else if (!strcmp(v, "false") || !strcmp(v, "False")) store_set_bool(key, false);
  else if (!strcmp(v, "null")) store_set(key, (Value){SV_NULL, false, 0, NONE16});
  else store_set_str(key, v);
}

/* Gr: storageOnAction; kr: menuPortal (the leaderboard) */
static void sys_on_action(void) {
  if (!in.pressed[A_ACTION]) return;
  for (int i = 0; i < ent_count(); i++) {
    NodeId n = ent_at(i);
    if (!n || !triggered(n)) continue;
    uint16_t T = nodes[n].T;
    if (comp_has(T, C_storageOnAction)) {
      uint16_t k = comp_str(T, C_storageOnAction, F_key), v = comp_str(T, C_storageOnAction, F_value);
      if (k != NONE16) store_parsed(str(k), v != NONE16 ? str(v) : "");
    }
    if (comp_has(T, C_menuPortal)) menus_open("leaderboard");
  }
}

/* ir: a door or path with the player in it takes them on when OK is let go */
static void go_portal(const char *spec) {
  char name[48];
  snprintf(name, sizeof name, "%s", spec);
  char *at = strchr(name, '@'), *colon = strchr(name, ':');
  bool plain = !colon || (at && colon > at);
  if (at) *at = 0;
  if (colon && (!at || colon < at)) *colon = 0;
  /* a sport's first visit starts with its film */
  if (game_is_sport(name) && plain) {
    char k[48];
    snprintf(k, sizeof k, "%sintro_VIDEO_SEEN", name);
    if (!store_bool(k, false)) {
      snprintf(k, sizeof k, "video:%sintro", name);
      save_place();
      game_go(k);
      return;
    }
  }
  save_place();
  game_go(spec);
}

static void sys_portals(void) {
  if (!in.released[A_ACTION]) return;
  for (int i = 0; i < ent_count(); i++) {
    NodeId n = ent_at(i);
    if (!n || !comp_has(nodes[n].T, C_scenePortal) || !triggered(n)) continue;
    uint16_t nm = comp_str(nodes[n].T, C_scenePortal, F_name);
    if (nm != NONE16) { go_portal(str(nm)); return; }
  }
}

/* or: talking to someone (OK, standing still, in their trigger area) */
static void sys_npcs(void) {
  if (!in.held[A_ACTION]) W->talked = 0;
  bool still = in.jx == 0 && in.jy == 0;
  for (int i = 0; i < ent_count(); i++) {
    NodeId n = ent_at(i);
    if (!n || !comp_has(nodes[n].T, C_npc)) continue;
    uint16_t T = nodes[n].T;
    if (!triggered(n)) continue;
    if (!still || W->talked == n || !in.pressed[A_ACTION]) continue;
    uint16_t name = comp_str(T, C_npc, F_name), node = NONE16;
    if (comp_has(T, C_storageNpc))
      for (int k = 0; k < 10 && node == NONE16; k++) {
        uint16_t nd = comp_str(T, C_storageNpc, NODE_F[k]);
        if (nd == NONE16 || !str(nd)[0]) continue;
        uint16_t cond = comp_str(T, C_storageNpc, COND_F[k]);
        if (cond == NONE16 || !str(cond)[0] || cond_eval(cond)) node = nd;
      }
    if (node == NONE16) node = comp_str(T, C_npc, F_node);
    extern bool dialog_first_node(uint16_t npc, uint16_t *name);
    if ((node == NONE16 || !str(node)[0]) && !(name != NONE16 && dialog_first_node(name, &node))) continue;
    W->talked = n;
    input_consume(A_ACTION);
    dialog_start(name, node);
    return;
  }
}

/* tr, ur, Fr: what trigger areas do when the player walks in */
static void on_trigger(NodeId t, NodeId other, bool entered) {
  if (!entered) return;
  uint16_t T = nodes[t].T;
  if (comp_has(T, C_dialogTrigger)) {
    uint16_t npc = comp_str(T, C_dialogTrigger, F_npc), node = comp_str(T, C_dialogTrigger, F_node);
    if (npc != NONE16 && node != NONE16) dialog_start(npc, node);
  }
  if (comp_has(T, C_sceneTrigger)) {
    uint16_t nm = comp_str(T, C_sceneTrigger, F_name);
    if (nm != NONE16) { save_place(); game_go(str(nm)); }
  }
  if (comp_has(T, C_storageTrigger)) {
    uint16_t k = comp_str(T, C_storageTrigger, F_key), v = comp_str(T, C_storageTrigger, F_value);
    if (k != NONE16) store_parsed(str(k), v != NONE16 ? str(v) : "");
  }
  (void)other;
}

/* rr: the rain over the mountain fades in near it (until the RAIN quest is done) */
static void sys_rain(void) {
  if (W->region_rain < 0 || !W->player) return;
  const uint8_t *r = wd.regions + 10 * (unsigned)W->region_rain;
  float rx = rds16(r + 2), rb = rds16(r + 8);
  float px, py;
  ent_pos(W->player, &px, &py);
  float a = fminf(clampf((px - rx - 250) / 300, 0, 1), clampf((rb - py) / 400, 0, 1));
  W->rain = W->rain * .8f + .2f * a;
}

/* bca: 48 px rain tiles over the view, moved with the camera (qr, repeatable
 * tiles), its two looks (Yi: u_a, v_a) taking turns every 3 ticks (Os), at
 * half alpha; drawn as one pattern from the data (pack.py's tiles4) */
static bool rain_slot(unsigned slot, const SlotInfo *si, void *ctx) {
  if (W->rain_n >= RAIN_MAX) return false;
  W->rain_pos[2 * W->rain_n] = (int16_t)floorf(si->x + .5f);
  W->rain_pos[2 * W->rain_n + 1] = (int16_t)floorf(si->y + .5f);
  W->rain_n++;
  return true;
}
static uint16_t glow_sprite(void) {
  SymInfo glow;
  sym_info(S_overworld_nAa, &glow);
  return glow.type == SYM_BITMAP ? (uint16_t)glow.v : NONE16;
}

/* a pattern set of the data (pack.py: rain, petals) */
static const uint8_t *pattern(unsigned i) {
  if (!HDR(H_TILES)) return NULL;
  const uint8_t *blk = ci_data + HDR(H_TILES);
  return i < rd16(blk) ? blk + rd32(blk + 4 + 4 * i) : NULL;
}

/* oAa, mAa: after the ending (outro seen), a glow over the top of the view and
 * the petals falling over the island (repeatable tiles of 144 x 62, the 20
 * looks of a petal taking turns every 3 ticks) */
static bool petal_slot(unsigned slot, const SlotInfo *si, void *ctx) {
  if (W->npetal >= PETAL_MAX) return false;
  W->petal_pos[2 * W->npetal] = (int16_t)floorf(si->x + .5f);
  W->petal_pos[2 * W->npetal + 1] = (int16_t)floorf(si->y + .5f);
  W->npetal++;
  return true;
}
static void draw_ending(float mx, float my) {
  uint16_t gs = glow_sprite();
  if (gs != NONE16) gfx_sprite(gs, (Mat){1, 0, 0, 1, 0, (float)gfx_view_top()}, 255);   /* over the top of the screen */
  const uint8_t *t = pattern(1);
  if (!t || !W->npetal) return;
  unsigned looks = rd16(t + 2), look = W->tick / 3 % (looks ? looks : 1);
  const uint8_t *offs = t + 56;
  int ox = (int)floorf((fmodf(fmodf(mx, 432) + 432, 432) - 432) / 3 + .5f);
  int oy = (int)floorf((fmodf(fmodf(my, 186) + 186, 186) - 186) / 3 + .5f);
  gfx_points(t + rd32(offs + 4 * look), rd16(t + 4), rd16(t + 6), (const uint16_t *)(t + 8), t + 40, W->petal_pos, W->npetal,
             ox, oy, 255);
}

static void draw_over_overworld(void) {
  if (!W || !W->map) return;
  float mx = nodes[W->map].x, my = nodes[W->map].y;
  if (W->ending) draw_ending(mx, my);
  if (W->rain < .01f || !W->rain_n) return;
  if (W->rain_cond != NONE16 && str(W->rain_cond)[0] && !cond_eval(W->rain_cond)) return;
  const uint8_t *t = pattern(0);
  if (!t) return;
  int looks = rd16(t + 2), w = rd16(t + 4), h = rd16(t + 6);
  unsigned look = (W->tick / 3) % 2 % (unsigned)(looks ? looks : 1);
  int ox = (int)floorf((fmodf(fmodf(mx, 144) + 144, 144) - 144) / 3 + .5f);
  int oy = (int)floorf((fmodf(fmodf(my, 144) + 144, 144) - 144) / 3 + .5f);
  gfx_tiles(t + 56 + look * (unsigned)(w * h / 2), w, h, (const uint16_t *)(t + 8), t + 40, W->rain_pos, W->rain_n, ox, oy,
            (uint8_t)(W->rain * 128 + .5f));
}

/* mr: the place is saved every two seconds */
static void sys_save_place(void) {
  if (W->tick % 60 == 59) save_place();
}

/* ---------------------------------------------------------------- tests (host) */
#ifdef HOST
typedef struct { int want, i; char name[32]; } Nth;
void nth_place(const char *name, float x, float y, void *ctx) {
  Nth *q = ctx;
  if (q->i++ == q->want) snprintf(q->name, sizeof q->name, "%s", name);
  (void)x; (void)y;
}
static int subtree(NodeId n) {
  int t = 1;
  for (NodeId c = nodes[n].first; c; c = nodes[c].next) t += subtree(c);
  return t;
}
void world_census(void) {
  int used = 0;
  for (int i = 1; i < NODE_MAX; i++) if (nodes[i].flags & NF_USED) used++;
  fprintf(stderr, "census: %d nodes\n", used);
  for (int i = 1; i < NODE_MAX; i++)
    if ((nodes[i].flags & NF_USED) && !nodes[i].parent) fprintf(stderr, "  root %d sym %d: %d\n", i, nodes[i].sym, subtree((NodeId)i));
  if (!W || !W->map) return;
  int k = 0;
  for (NodeId c = nodes[W->map].first; c; c = nodes[c].next, k++) {
    int n = subtree(c);
    if (n >= 4) fprintf(stderr, "  map child sym %d T %d: %d\n", nodes[c].sym, nodes[c].T, n);
  }
  fprintf(stderr, "  map children %d\n", k);
}
#endif

/* ---------------------------------------------------------------- the scene */
static bool player_slot(unsigned slot, const SlotInfo *si, void *ctx) {
  Clip c;
  if (!clip_get(si->sym, &c) || c.T == NONE16 || !comp_has(c.T, C_overworldPlayer)) return true;
  float *p = ctx;
  p[0] = si->x;
  p[1] = si->y;
  (void)slot;
  return false;
}

static NodeId child_of_sym(NodeId n, uint16_t sym) {
  for (NodeId c = nodes[n].first; c; c = nodes[c].next)
    if (nodes[c].sym == sym) return c;
  return 0;
}

static void start_overworld(void) {
  bool ending = store_bool("outro_VIDEO_SEEN", false);
  W = scene_state(sizeof(World));
  W->region_rain = -1;
  world_load(S_overworld_jqa);
  /* oU: the sea (w9a), the map (jqa, lazy), the ending's sky (oAa, mAa) and the rain (bca) */
  node_lazy_sym = S_overworld_jqa;
  NodeId root = node_new_sym(S_overworld_oU);
  node_lazy_sym = NONE16;
  if (!root) return;
  node_add(game.root, root);
  W->map = child_of_sym(root, S_overworld_jqa);
  /* the sea and the rain are drawn by code (the layer's water, draw_over) */
  node_free(child_of_sym(root, S_overworld_w9a));
  NodeId rain = child_of_sym(root, S_overworld_bca);
  W->rain_cond = rain && nodes[rain].T != NONE16 ? comp_str(nodes[rain].T, C_conditionallyVisible, F_condition) : NONE16;
  if (rain) { node_remove(rain); node_free(rain); }
  clip_each_slot(S_overworld_bca, 0, rain_slot, NULL);
  /* the ending's glow and petals are drawn by code too (draw_over) */
  node_free(child_of_sym(root, S_overworld_oAa));
  node_free(child_of_sym(root, S_overworld_mAa));
  W->ending = ending;
  if (W->ending) clip_each_slot(S_overworld_mAa, 0, petal_slot, NULL);
  if (!W->map) return;
  /* the island fills the screen: the stage and 30 rows above and below */
  gfx_view(-VIEW_Y, VIEW_H + VIEW_Y);
  mem_layout(LAYER_W, LAYER_H, SHEET_OVERWORLD, paint);
  /* after the ending, the glow is read from its runs in the data (pack.py) */
  const uint8_t *glow = pattern(2);
  if (ending && glow && rd16(glow + 2) == glow_sprite()) spr_pin(glow_sprite(), glow + 4);
  bg_blend_lut(wd.lut);
  ent_register_tree(root);
  node_stream_hook = streamed_in;
  node_draw_hook_id = W->map;
  node_draw_hook = draw_map;
  ent_on_trigger(on_trigger);
  /* wr: the island's physics */
  phys_steps(1, 20);
  for (int i = 0; i < wd.nregions; i++)
    if (rd16(wd.regions + 10 * i) == S_overworld_bT) W->region_rain = i;
  /* the player (the map's child "Player") is always there */
  float ppos[2] = {0, 0};
  clip_each_slot(S_overworld_jqa, 0, player_slot, ppos);
  node_stream(W->map, ppos[0] - 1, ppos[1] - 1, ppos[0] + 1, ppos[1] + 1);
  for (NodeId c = nodes[W->map].first; c; c = nodes[c].next)
    if (nodes[c].T != NONE16 && comp_has(nodes[c].T, C_overworldPlayer)) W->player = c;
  if (W->player) nodes[W->player].flags2 |= NF2_KEEP;
  /* sr: where to appear: the scene's place, else the saved one (else the dock) */
  const char *where = game.location[0] ? game.location : store_str("PLAYER_LOC");
  if (where) teleport(where);
  sys_camera_snap();
  stream_children(true);
  W->water_look = -1;
}

static void end_overworld(void) {
  spr_pin(glow_sprite(), NULL);
  node_stream_hook = NULL;
  node_draw_hook = NULL;
  node_draw_hook_id = 0;
  ent_on_trigger(NULL);
  W = NULL;
}

static void tick_overworld(void) {
  if (!W || !W->map) return;
  sys_back_map();
  sys_overworld_player();
  sys_camera_target();
  sys_camera_move();
  sys_tile_backgrounds();
  sys_storage_sprites();
  sys_conditional();
  sys_on_action();
  sys_sort_draw();
  sys_triggers();
  sys_jump_to_frame();
  sys_sprite_dirs();
  solids_update();
  sys_physics();
  sys_ground_height();
  sys_portals();
  sys_save_place();
  sys_npcs();
  sys_rain();
  stream_children(false);
#ifdef HOST
  /* CI_TOUR: visit every place, one per 20 frames, and report the busiest */
  if (getenv("CI_TOUR")) {
    static int place_i, max_nodes;
    static char max_at[32];
    typedef struct { int want, i; char name[32]; } Nth;
    void nth_place(const char *name, float x, float y, void *ctx);
    if (W->tick % 20 == 19) {
      unsigned used = node_count();
      if ((int)used > max_nodes) { max_nodes = (int)used; snprintf(max_at, sizeof max_at, "%s", game.location); }
      Nth q = {place_i++, 0, ""};
      each_place(nth_place, &q);
      if (q.name[0]) { teleport(q.name); snprintf(game.location, sizeof game.location, "%s", q.name); sys_camera_snap(); stream_children(true); }
      else if (place_i == q.i + 1) fprintf(stderr, "tour: busiest %s with %d nodes\n", max_at, max_nodes);
    }
  }
  if (getenv("CI_DEBUG") && W->player && W->tick % 10 == 0) {
    float px, py;
    ent_pos(W->player, &px, &py);
    Ent *e = ent_peek(W->player);
    Rect r;
    bool hb = ent_bounds(W->player, &r);
    for (int i = 0; i < ent_count(); i++) {
      NodeId n = ent_at(i);
      if (!n || !comp_has(nodes[n].T, C_scenePortal)) continue;
      float x, y;
      ent_pos(n, &x, &y);
      Ent *pe = ent_peek(n);
      if (fabsf(x - px) < 150 && fabsf(y - py) < 150)
        fprintf(stderr, "  portal %s at %.0f,%.0f trig %d vis %d\n", str(comp_str(nodes[n].T, C_scenePortal, F_name)), x, y, pe ? pe->ntrig : -1, node_visible(n));
    }
    fprintf(stderr, "t%u in %.1f,%.1f p %.1f,%.1f v %.2f,%.2f body %d bounds %d (%.0f %.0f %.0f %.0f) solids %d label %s\n", W->tick, in.jx, in.jy,
            px, py, e ? e->vx : 0, e ? e->vy : 0, e ? e->body : -9, hb, r.x, r.y, r.w, r.h, W->nsolid, node_label(W->player));
  }
#endif
  W->tick++;
}

static void draw_under_overworld(void) {
  if (!W || !W->map) return;
  /* the sea's look changes every 16 frames (of the island's: still under a menu) */
  int look = (int)(W->tick / 16) % 2;
  if (look != W->water_look) {
    water_make(look);
    bg_water4(W->water, WATER, WATER, W->water_col);
  }
  float vx, vy;
  view_origin(&vx, &vy);
  bg_camera((int)floorf(vx + .5f), (int)floorf(vy + .5f));
}

const SceneDef scene_overworld = {"overworld", start_overworld, tick_overworld, end_overworld, draw_under_overworld, draw_over_overworld, NULL};

/* ---------------------------------------------------------------- interiors */
/* Ir: a room is a frame of the interior library's root (its label is the
 * scene's variant), a small map drawn at the middle of the screen */
static uint16_t room_label = NONE16;

static void start_interior(void) {
  W = scene_state(sizeof(World));
  W->region_rain = -1;
  W->water_look = -1;
  Clip c;
  int frame = 0;
  if (clip_get(S_interior_mbb, &c) && game.variant[0]) {
    int f = clip_label(&c, game.variant);
    if (f >= 0) frame = f;
  }
  NodeId root = node_new_sym_frame(S_interior_mbb, frame);
  if (!root) return;
  node_add(game.root, root);
  ent_register_tree(root);
  translate_tree(root);
  W->map = ent_map;
  if (W->map) world_load(nodes[W->map].sym);
  for (int i = 0; i < ent_count(); i++) {
    NodeId n = ent_at(i);
    if (n && comp_has(nodes[n].T, C_overworldPlayer)) W->player = n;
    /* sr: places are markers */
    if (n && comp_has(nodes[n].T, C_location) && !comp_has(nodes[n].T, C_boundable)) nodes[n].alpha = 0;
  }
  ent_on_trigger(on_trigger);
  phys_steps(1, 20);
  (void)room_label;
}

static void tick_interior(void) {
  if (!W || !W->map) return;
  sys_back_map();
  sys_on_action();
  sys_sort_draw();
  sys_triggers();
  sys_player_movement();
  sys_player_dir();
  sys_jump_to_frame();
  sys_walk_idle();
  sys_storage_sprites();
  sys_sprite_dirs();
  solids_update();
  sys_physics();
  sys_ground_height();
  sys_portals();
  sys_npcs();
  W->tick++;
}

static void end_interior(void) {
  ent_on_trigger(NULL);
  W = NULL;
}

const SceneDef scene_interior = {"interior", start_interior, tick_interior, end_interior, NULL, NULL, NULL};
