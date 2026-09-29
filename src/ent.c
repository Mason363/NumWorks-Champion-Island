/* Entities and the doodle's shared systems (see ent.h). */
#include "ent.h"
#include "spr.h"
#include <math.h>
#include <stdio.h>
#include "phys.h"

const char *const dir_names[8] = {"e", "se", "s", "sw", "w", "nw", "n", "ne"};

NodeId ent_map, ent_camera;

#define REG_MAX 192   /* registered entities (153 at most seen) */
typedef struct { NodeId n; uint16_t T; uint16_t state; } Reg;   /* state: Ent + 1 */
static Reg reg[REG_MAX];
static int nreg;
static Ent ents[ENT_MAX];
static TriggerFn trigger_fn;

static void on_free(NodeId n) { ent_unregister(n); }
static void on_made(NodeId n);

void ent_reset(void) {
  memset(reg, 0, sizeof reg);
  memset(ents, 0, sizeof ents);
  nreg = 0;
  ent_map = ent_camera = 0;
  trigger_fn = NULL;
  node_free_hook = on_free;
  node_partial_hook = on_made;
  phys_reset();
}

void ent_on_trigger(TriggerFn fn) { trigger_fn = fn; }

int ent_count(void) { return nreg; }
NodeId ent_at(int i) { return i >= 0 && i < nreg ? reg[i].n : 0; }

static int reg_of(NodeId n) { return nodes[n].ent ? nodes[n].ent - 1 : -1; }

uint16_t ent_T(NodeId n) { return nodes[n].T; }

bool ent_has(NodeId n, int comp) { return n && nodes[n].ent && comp_has(nodes[n].T, comp); }

static void register_one(NodeId n) {
  if (nodes[n].ent || nodes[n].T == NONE16) return;
  int i = -1;
  for (int k = 0; k < nreg; k++) if (!reg[k].n) { i = k; break; }
  if (i < 0) {
    if (nreg >= REG_MAX) return;
    i = nreg++;
  }
  reg[i] = (Reg){n, nodes[n].T, 0};
  nodes[n].ent = (uint16_t)(i + 1);
  uint16_t T = nodes[n].T;
  if (!ent_map && comp_has(T, C_map)) ent_map = n;
  if (!ent_camera && comp_has(T, C_camera)) ent_camera = n;
  if (comp_has(T, C_map) && !comp_bool(T, C_map, F_positionalDrawOrder, true)) {}
}

static void register_rec(NodeId n) {
  register_one(n);
  /* the children of untraversable entities are not entities */
  if (nodes[n].T != NONE16 && comp_has(nodes[n].T, C_untraversable)) return;
  for (NodeId c = nodes[n].first; c; c = nodes[c].next) register_rec(c);
}

void ent_register_tree(NodeId root) { register_rec(root); }

/* a child a room or a menu made when its frame first showed it (node.c's
 * partial clips): an entity as if it had been there from the start */
static void made_rec(NodeId n) {
  ent_translate(n);
  uint16_t T = nodes[n].T;
  /* sr: places are markers for the designers */
  if (T != NONE16 && comp_has(T, C_location) && !comp_has(T, C_boundable)) nodes[n].alpha = 0;
  for (NodeId c = nodes[n].first; c; c = nodes[c].next) made_rec(c);
}
static void on_made(NodeId n) {
  for (NodeId a = nodes[n].parent; a; a = nodes[a].parent)
    if (nodes[a].T != NONE16 && comp_has(nodes[a].T, C_untraversable)) return;
  register_rec(n);
  made_rec(n);
}

void ent_unregister(NodeId n) {
  int i = reg_of(n);
  if (i < 0) return;
  if (reg[i].state) {
    Ent *e = &ents[reg[i].state - 1];
    if (e->body >= 0) phys_remove(e->body);
    memset(e, 0, sizeof *e);
  }
  if (n == ent_map) ent_map = 0;
  if (n == ent_camera) ent_camera = 0;
  reg[i].n = 0;
  nodes[n].ent = 0;
  /* forget it in triggers */
  for (int k = 0; k < ENT_MAX; k++)
    for (int t = 0; t < ents[k].ntrig; t++)
      if (ents[k].trig[t] == n) { ents[k].trig[t] = ents[k].trig[--ents[k].ntrig]; t--; }
}

/* the state record if it has one (never makes one): caches only use this,
 * so the records go to the entities that move, turn or hold triggers */
Ent *ent_peek(NodeId n) {
  int i = n ? reg_of(n) : -1;
  return i >= 0 && reg[i].state ? &ents[reg[i].state - 1] : NULL;
}

#ifdef HOST
/* tests: registered entities and state records in use */
void ent_stats(int *live, int *states) {
  *live = *states = 0;
  for (int k = 0; k < nreg; k++) if (reg[k].n) (*live)++;
  for (int k = 0; k < ENT_MAX; k++) if (ents[k].n) (*states)++;
}
#endif

Ent *ent_get(NodeId n) {
  int i = n ? reg_of(n) : -1;
  if (i < 0) return NULL;
  if (reg[i].state) return &ents[reg[i].state - 1];
  for (int k = 0; k < ENT_MAX; k++)
    if (!ents[k].n) {
      Ent *e = &ents[k];
      memset(e, 0, sizeof *e);
      e->n = n;
      e->body = -1;
      e->dir = DIR_S;
      uint16_t T = nodes[n].T;
      comp_vec(T, C_velocity, F_velocity, &e->vx, &e->vy);
      if (comp_has(T, C_direction)) {
        uint16_t d = comp_str(T, C_direction, F_direction);
        e->dir = (int8_t)(d != NONE16 ? dir_parse(str(d)) : DIR_S);
        if (e->dir < 0) e->dir = DIR_S;
      }
      e->z = comp_float(T, C_zObject, F_z, 0);
      reg[i].state = (uint16_t)(k + 1);
      return e;
    }
  return NULL;
}

NodeId ent_first(int comp) {
  for (int i = 0; i < nreg; i++)
    if (reg[i].n && comp_has(reg[i].T, comp)) return reg[i].n;
  return 0;
}

NodeId ent_first2(int comp, int comp2) {
  for (int i = 0; i < nreg; i++)
    if (reg[i].n && comp_has(reg[i].T, comp) && comp_has(reg[i].T, comp2)) return reg[i].n;
  return 0;
}

/* ---------------------------------------------------------------- directions */
int dir_parse(const char *s) {
  for (int i = 0; i < 8; i++)
    if (!strcmp(dir_names[i], s)) return i;
  return -1;
}

float dir_angle(int d) { return d < 0 ? 0 : d * 45.0f; }

int dir_of(float x, float y, bool four) {
  if (x == 0 && y == 0) return -1;
  float a = fmodf(atan2f(y, x) * (180.0f / 3.14159265f) + 360.0f, 360.0f);
  if (four) {
    if (a < 45 || a >= 315) return DIR_E;
    if (a < 135) return DIR_S;
    if (a < 225) return DIR_W;
    return DIR_N;
  }
  return (int)floorf((a + 22.5f) / 45.0f) & 7;
}

/* ---------------------------------------------------------------- positions */
static NodeId map_of(NodeId e) {
  for (NodeId p = e; p; p = nodes[p].parent)
    if (nodes[p].T != NONE16 && comp_has(nodes[p].T, C_map)) return p;
  return 0;
}

void ent_moved(NodeId e) {
  /* xj: this entity and its descendants forget their cached position and bounds */
  Ent *s = ent_peek(e);
  if (s) s->bounds_valid = s->pos_valid = false;
  for (NodeId c = nodes[e].first; c; c = nodes[c].next)
    if (nodes[c].ent) ent_moved(c);
}

void ent_pos(NodeId e, float *x, float *y) {
  Ent *s = ent_peek(e);
  if (s && s->pos_valid) { *x = s->px; *y = s->py; return; }
  NodeId m = map_of(e);
  Mat t = node_to(e, m);
  *x = t.tx;
  *y = t.ty;
  if (s) { s->px = *x; s->py = *y; s->pos_valid = true; }
}

void ent_local_pos(NodeId e, float *x, float *y) {
  Mat t = node_local(e);
  *x = t.tx;
  *y = t.ty;
}

void ent_set_local_pos(NodeId e, float x, float y) {
  Node *n = &nodes[e];
  Mat t = node_local(e);
  /* Dj: x = p.x + regX * a */
  n->x = x + n->rx4 * 0.25f * t.a;
  n->y = y + n->ry4 * 0.25f * t.d;
  ent_moved(e);
}

void ent_set_pos(NodeId e, float x, float y) {
  NodeId m = map_of(e);
  if (!m) return;
  NodeId p = nodes[e].parent;
  Mat t = node_to(p, m);
  float det = t.a * t.d - t.b * t.c;
  if (fabsf(det) < 1e-9f) return;
  float dx = x - t.tx, dy = y - t.ty;
  ent_set_local_pos(e, (t.d * dx - t.c * dy) / det, (-t.b * dx + t.a * dy) / det);
}

static NodeId find_bounds_child(NodeId n) {
  if (nodes[n].T != NONE16 && comp_has(nodes[n].T, C_bounds)) return n;
  for (NodeId c = nodes[n].first; c; c = nodes[c].next) {
    NodeId r = find_bounds_child(c);
    if (r) return r;
  }
  return 0;
}

bool ent_bounds(NodeId e, Rect *r) {
  Ent *s = ent_peek(e);
  if (s && s->bounds_valid) { *r = s->bounds; return true; }
  if (!comp_has(nodes[e].T, C_boundable)) return false;
  NodeId b = find_bounds_child(e);
  if (!b) return false;
  /* the bounds child's transformed bounds, taken as a rectangle of e's space */
  float x, y, w, h;
  if (!node_bounds_in(b, nodes[b].parent, &x, &y, &w, &h)) return false;
  NodeId m = map_of(e);
  Mat t = node_to(e, m);
  float x0 = t.a * x + t.c * y + t.tx, y0 = t.b * x + t.d * y + t.ty;
  float x1 = t.a * (x + w) + t.c * (y + h) + t.tx, y1 = t.b * (x + w) + t.d * (y + h) + t.ty;
  r->x = x0 < x1 ? x0 : x1;
  r->y = y0 < y1 ? y0 : y1;
  r->w = fabsf(x1 - x0);
  r->h = fabsf(y1 - y0);
  if (s) { s->bounds = *r; s->bounds_valid = true; }
  return true;
}

bool ent_overlap(NodeId a, NodeId b) {
  Rect ra, rb;
  float x, y;
  if (!ent_bounds(a, &ra)) { ent_pos(a, &x, &y); ra = (Rect){x, y, 0, 0}; }
  if (!ent_bounds(b, &rb)) { ent_pos(b, &x, &y); rb = (Rect){x, y, 0, 0}; }
  return rect_intersects(ra, rb);
}

Rect ent_viewport(void) {
  NodeId c = ent_camera ? ent_camera : ent_map;
  if (!c) return (Rect){0, 0, 960, 540};
  Mat t = node_global(c);
  float det = t.a * t.d - t.b * t.c;
  if (fabsf(det) < 1e-9f) return (Rect){0, 0, 960, 540};
  /* stage (0, 0)-(960, 540) in the camera's space */
  float x0 = (t.d * (0 - t.tx) - t.c * (0 - t.ty)) / det, y0 = (-t.b * (0 - t.tx) + t.a * (0 - t.ty)) / det;
  float x1 = (t.d * (960 - t.tx) - t.c * (540 - t.ty)) / det, y1 = (-t.b * (960 - t.tx) + t.a * (540 - t.ty)) / det;
  return (Rect){x0, y0, x1 - x0, y1 - y0};
}

bool ent_in_view4(NodeId e, float l, float t, float r, float b, bool use_bounds) {
  Rect v = rect_pad(ent_viewport(), l, t, r, b);
  if (use_bounds) {
    Rect br;
    float x, y;
    if (!ent_bounds(e, &br) && !node_bounds_in(e, map_of(e), &br.x, &br.y, &br.w, &br.h)) { ent_pos(e, &x, &y); br = (Rect){x, y, 0, 0}; }
    return rect_intersects(v, br) || (br.w == 0 && rect_contains(v, br.x, br.y));
  }
  float x, y;
  ent_pos(e, &x, &y);
  return rect_contains(v, x, y);
}

bool ent_in_view(NodeId e, float margin, bool use_bounds) { return ent_in_view4(e, margin, margin, margin, margin, use_bounds); }

/* ---------------------------------------------------------------- labels */
bool ent_label(NodeId e, const char *label, bool keep) {
  if (nodes[e].kind != NK_CLIP) return false;
  if (!strcmp(node_label(e), label) && node_has_label(e, label)) {
    node_stop(e);
    return false;
  }
  if (!node_has_label(e, label)) return false;
  NodeId old = node_onstage_child(e);
  int of = old ? nodes[old].frame : 0, ot = old ? node_frames(old) : 0;
  bool paused = old ? !(nodes[old].flags & NF_PLAYING) : false;
  node_goto(e, label, 0, false);
  node_update_one(e);
  if (keep && old) {
    NodeId now = node_onstage_child(e);
    if (now && now != old && node_frames(now) == ot) {
      nodes[now].frame = (uint16_t)of;
      if (paused) node_stop(now); else node_play(now);
    }
  }
  ent_register_tree(e);
  return true;
}

void ent_dir_label(NodeId e, int d) {
  if (d < 0) return;
  const char *l = dir_names[d];
  if (!node_has_label(e, l)) {
    if (d == DIR_NE || d == DIR_SE) { if (node_has_label(e, "e")) l = "e"; else return; }
    else if (d == DIR_NW || d == DIR_SW) { if (node_has_label(e, "w")) l = "w"; else return; }
    else return;
  }
  ent_label(e, l, true);
}

bool ent_play_label(NodeId e, const char *label) {
  bool r = node_goto(e, label, 0, true);
  node_update_one(e);
  ent_register_tree(e);
  return r;
}

/* ---------------------------------------------------------------- spawning */
NodeId ent_spawn(uint16_t sym) {
  NodeId n = node_new_sym(sym);
  return n;
}

void ent_add(NodeId e, NodeId parent) {
  if (!e || !parent) return;
  node_add(parent, e);
  ent_register_tree(e);
  if (nodes[e].ent) sys_add_fx(e);
}

NodeId ent_spawn_at(uint16_t sym, float x, float y) {
  NodeId e = ent_spawn(sym);
  if (!e) return 0;
  if (ent_map) {
    ent_add(e, ent_map);
    ent_set_pos(e, x, y);
  }
  return e;
}

void ent_remove(NodeId e) {
  if (!e) return;
  if (nodes[e].ent) sys_remove_fx(e);
  node_free(e);
}

/* ---------------------------------------------------------------- systems */
void sys_back_pauses(void) {
  if (in.pressed[A_BACK]) {
    input_consume(A_BACK);
    menus_open("pause");
  }
}

void sys_tutorial_once(void) {
  static const char *const tuts[][2] = {{"archery", "tutArcheryDesktop"}, {"climbing", "tutClimbingDesktop"},
                                        {"marathon", "tutMarathonDesktop"}, {"pingpong", "tutPingpongDesktop"},
                                        {"rugby", "tutRugbyDesktop"}, {"skate", "tutSkateDesktop"}, {"swim", "tutSwimDesktop"}};
  char k[48];
  for (unsigned i = 0; i < 7; i++)
    if (!strcmp(game.name, tuts[i][0])) {
      snprintf(k, sizeof k, "%s_TUTORIAL_SEEN", game.name);
      if (!store_bool(k, false)) {
        store_set_bool(k, true);
        menus_open(tuts[i][1]);
      }
    }
}

void sys_countdown_start(Countdown *c) { c->t = 105; c->active = true; }

bool sys_countdown(Countdown *c) {
  if (!c->active) return true;
  if (c->t == 90) toast_countdown("3");
  else if (c->t == 60) toast_countdown("2");
  else if (c->t == 30) toast_countdown("1");
  else if (c->t == 0) {
    toast_countdown(msg("GO"));
    c->active = false;
    return true;
  }
  if (c->t > 0) c->t--;
  return false;
}

void sys_ephemeral(void) {
  for (int i = 0; i < nreg; i++) {
    NodeId n = reg[i].n;
    if (!n || !comp_has(reg[i].T, C_ephemeral)) continue;
    int df = comp_int(reg[i].T, C_ephemeral, F_deathFrame, -1);
    if (nodes[n].frame == (df >= 0 ? df : node_frames(n) - 1)) ent_remove(n);
  }
}

/* the entity (itself or an ancestor within 3) that has all of comps */
static NodeId with_comps(NodeId n, int c1, int c2) {
  for (int d = 0; n && d <= 3; d++, n = nodes[n].parent)
    if (nodes[n].T != NONE16 && comp_has(nodes[n].T, c1) && (c2 < 0 || comp_has(nodes[n].T, c2))) return n;
  return 0;
}

static bool has_4dirs(NodeId n) {
  return node_has_label(n, "n") && node_has_label(n, "s") && node_has_label(n, "e") && node_has_label(n, "w");
}

void sys_sprite_dirs(void) {
  for (int i = 0; i < nreg; i++) {
    NodeId n = reg[i].n;
    if (!n || !node_visible(n) || !(comp_has(reg[i].T, C_sprite) || comp_has(reg[i].T, C_spritable))) continue;
    NodeId m = with_comps(n, C_spritable, C_direction);
    if (m && has_4dirs(n)) {
      Ent *e = ent_get(m);
      if (e) ent_dir_label(n, e->dir);
    }
  }
}

void sys_walk_idle(void) {
  for (int i = 0; i < nreg; i++) {
    NodeId n = reg[i].n;
    if (!n || !node_visible(n) || !(comp_has(reg[i].T, C_sprite) || comp_has(reg[i].T, C_spritable))) continue;
    if (with_comps(n, C_spritable, C_direction) && node_has_label(n, "walk") && node_has_label(n, "idle"))
      ent_label(n, (in.jx != 0 || in.jy != 0) ? "walk" : "idle", false);
  }
}

void sys_jump_to_frame(void) {
  for (int i = 0; i < nreg; i++) {
    NodeId n = reg[i].n;
    if (!n || !node_visible(n) || !comp_has(reg[i].T, C_jumpToFrameOnTrigger)) continue;
    NodeId t = with_comps(n, C_trigger, -1);
    Ent *te = t ? ent_get(t) : NULL;
    uint16_t f = comp_str(reg[i].T, C_jumpToFrameOnTrigger, te && te->ntrig ? F_triggerFrame : F_untriggerFrame);
    if (f != NONE16) ent_label(n, str(f), false);
  }
}

void sys_velocity(void) {
  for (int i = 0; i < nreg; i++) {
    NodeId n = reg[i].n;
    if (!n || !comp_has(reg[i].T, C_velocity)) continue;
    Ent *e = ent_get(n);
    if (!e) continue;
    float x, y;
    ent_local_pos(n, &x, &y);
    ent_set_local_pos(n, x + e->vx, y + e->vy);
  }
}

void sys_move_direct(void) {
  for (int i = 0; i < nreg; i++) {
    NodeId n = reg[i].n;
    if (!n || !comp_has(reg[i].T, C_velocity)) continue;
    if (comp_has(reg[i].T, C_collidable) && comp_has(reg[i].T, C_boundable)) continue;
    Ent *e = ent_get(n);
    if (!e) continue;
    float x, y;
    ent_pos(n, &x, &y);
    ent_set_pos(n, x + e->vx, y + e->vy);
  }
}

/* insertion sort of the map's children by a key (stable, like Array.sort in V8 for small arrays) */
static void sort_children(NodeId m, float (*key)(NodeId)) {
  NodeId sorted = 0;
  /* the keys live in the decoder's ring, idle between frames */
  uint32_t cap;
  float *keys = (float *)(void *)z_scratch(&cap);
  NodeId c = nodes[m].first;
  while (c) {
    NodeId nx = nodes[c].next;
    keys[c] = key(c);
    if (!sorted || keys[c] < keys[sorted]) { nodes[c].next = sorted; sorted = c; }
    else {
      NodeId s = sorted;
      while (nodes[s].next && keys[nodes[s].next] <= keys[c]) s = nodes[s].next;
      nodes[c].next = nodes[s].next;
      nodes[s].next = c;
    }
    c = nx;
  }
  nodes[m].first = sorted;
  nodes[m].flags |= NF_SORTED;
}

static float key_draw(NodeId c) {
  if (nodes[c].ent && comp_has(nodes[c].T, C_drawOrderOverride)) return comp_float(nodes[c].T, C_drawOrderOverride, F_drawOrder, 0);
  if (!node_visible(c)) return -1e15f;
  float x, y;
  ent_pos(c, &x, &y);
  return y;
}

static float key_positional(NodeId c) {
  if (!nodes[c].ent) { float x, y; ent_pos(c, &x, &y); return y; }
  uint16_t T = nodes[c].T;
  if (comp_has(T, C_drawOrderOverride)) return comp_float(T, C_drawOrderOverride, F_drawOrder, 0);
  Rect r;
  Ent *e = ent_peek(c);
  if (comp_has(T, C_zObject) && comp_has(T, C_boundable) && comp_has(T, C_zBoundable) && ent_bounds(c, &r)) {
    float h = comp_float(T, C_zBoundable, F_height, 100);
    return r.y + h + (comp_has(T, C_velocity) ? (e && e->has_ground ? e->ground : 0) : e ? e->z : 0);
  }
  float x, y;
  ent_pos(c, &x, &y);
  return y;
}

void sys_sort_draw(void) { if (ent_map) sort_children(ent_map, key_draw); }
void sys_sort_positional(void) { if (ent_map) sort_children(ent_map, key_positional); }

void sys_zsprite(void) {
  for (int i = 0; i < nreg; i++) {
    NodeId n = reg[i].n;
    if (!n || !node_visible(n) || !comp_has(reg[i].T, C_zSprite)) continue;
    NodeId z = with_comps(n, C_zObject, -1);
    if (!z) continue;
    Ent *e = ent_get(z);
    float x, y;
    ent_local_pos(n, &x, &y);
    ent_set_local_pos(n, x, -(e ? e->z : 0));
  }
}

void sys_delete_offscreen(void) {
  for (int i = 0; i < nreg; i++) {
    NodeId n = reg[i].n;
    if (!n || !comp_has(reg[i].T, C_deleteOffScreen)) continue;
    Mat g = node_global(n);
    if (g.tx < -1440 || g.ty < -1440 || g.tx >= 960 + 1440 || g.ty >= 540 + 1440) ent_remove(n);
  }
}

/* is `other` inside one of trigger t's areas? */
static bool trigger_hits(NodeId t, NodeId other) {
  if (!node_visible(t)) return false;
  Mat og = node_global(other);
  bool hit = false;
  for (NodeId a = nodes[t].first; a && !hit; a = nodes[a].next) {
    if (!(nodes[a].T != NONE16 && comp_has(nodes[a].T, C_triggerArea))) continue;
    Mat ag = node_global(a);
    float det = ag.a * ag.d - ag.b * ag.c;
    if (fabsf(det) < 1e-9f) continue;
    float dx = og.tx - ag.tx, dy = og.ty - ag.ty;
    float u = (ag.d * dx - ag.c * dy) / det, v = (-ag.b * dx + ag.a * dy) / det;
    hit = node_hit(a, u, v);
  }
  if (!hit) return false;
  if (nodes[other].ent && comp_has(nodes[other].T, C_zObject)) {
    Ent *e = ent_get(other);
    float lo = comp_float(nodes[t].T, C_trigger, F_zMin, -1e9f), hi = comp_float(nodes[t].T, C_trigger, F_zMax, 1e9f);
    float z = e ? e->z : 0;
    return (z >= lo && z <= hi) || (z <= lo && z >= hi);
  }
  return true;
}

void sys_triggers(void) {
  for (int i = 0; i < nreg; i++) {
    NodeId t = reg[i].n;
    if (!t || !comp_has(reg[i].T, C_trigger)) continue;
    uint16_t want = comp_str(reg[i].T, C_trigger, F_triggerComponent);
    if (want == NONE16) continue;
    int wc = -1;
    extern const char *const comp_names[];
    for (int c = 0; c < C_COUNT; c++) if (!strcmp(comp_names[c], str(want))) { wc = c; break; }
    Ent *e = ent_get(t);
    if (!e) continue;
    /* who left */
    for (int k = 0; k < e->ntrig; k++) {
      NodeId o = e->trig[k];
      if (!nodes[o].parent || !trigger_hits(t, o)) {
        e->trig[k] = e->trig[--e->ntrig];
        k--;
        if (trigger_fn) trigger_fn(t, o, false);
      }
    }
    /* someone new (one per tick) */
    if (wc < 0) continue;
    for (int k = 0; k < nreg; k++) {
      NodeId o = reg[k].n;
      if (!o || !comp_has(reg[k].T, wc)) continue;
      bool inside = false;
      for (int q = 0; q < e->ntrig; q++) if (e->trig[q] == o) inside = true;
      if (inside || !trigger_hits(t, o)) continue;
      if (e->ntrig < TRIG_MAX) e->trig[e->ntrig++] = o;
      if (trigger_fn) trigger_fn(t, o, true);
      break;
    }
  }
}

void sys_player_movement(void) {
  for (int i = 0; i < nreg; i++) {
    NodeId n = reg[i].n;
    if (!n || !comp_has(reg[i].T, C_playerMovement) || !comp_has(reg[i].T, C_velocity)) continue;
    Ent *e = ent_get(n);
    if (!e) continue;
    float sp = comp_float(reg[i].T, C_playerMovement, F_speed, 1);
    float len = sqrtf(in.jx * in.jx + in.jy * in.jy);
    e->vx = len ? in.jx / len * sp * len : 0;
    e->vy = len ? in.jy / len * sp * len : 0;
  }
}

void sys_waypoints(void) { /* Sp: moves waypoint entities; sports set their targets themselves */ }

void sys_player_dir(void) {
  for (int i = 0; i < nreg; i++) {
    NodeId n = reg[i].n;
    if (!n || !comp_has(reg[i].T, C_playerMovement) || !comp_has(reg[i].T, C_velocity)) continue;
    int d = dir_of(in.jx, in.jy, false);
    Ent *e = ent_get(n);
    if (d >= 0 && e) e->dir = (int8_t)d;
  }
}

static uint32_t vis_tick;
void sys_visibility(void) {
  if (!ent_map) return;
  for (NodeId c = nodes[ent_map].first; c; c = nodes[c].next) {
    if (!nodes[c].ent) continue;
    uint16_t T = nodes[c].T;
    if (comp_has(T, C_tileBackground)) { node_set_visible(c, ent_in_view(c, 0, true)); continue; }
    if (comp_has(T, C_region) || comp_has(T, C_overworldPlayer) || c % 4 != vis_tick % 4) continue;
    if (comp_has(T, C_conditionallyVisible) && !node_visible(c)) continue;
    node_set_visible(c, ent_in_view4(c, 240, 216, 240, 432, false));
  }
  vis_tick++;
}

void sys_tile_backgrounds(void) {
  /* Os: tileBackground with frameCount > 0 shows frame floor(tick / frameDuration) % frameCount */
  for (int i = 0; i < nreg; i++) {
    NodeId n = reg[i].n;
    if (!n || !comp_has(reg[i].T, C_tileBackground)) continue;
    int fc = comp_int(reg[i].T, C_tileBackground, F_frameCount, 0), fd = comp_int(reg[i].T, C_tileBackground, F_frameDuration, 1);
    if (fc <= 0) continue;
    Ent *e = ent_get(n);
    if (!e) continue;
    e->anim_t++;
    int f = (int)(e->anim_t / (fd > 0 ? fd : 1)) % fc;
    /* its children are clips: all show frame f * frameDuration */
    for (NodeId c = nodes[n].first; c; c = nodes[c].next)
      if (nodes[c].kind == NK_CLIP) { nodes[c].frame = (uint16_t)((f * fd) % node_frames(c)); node_stop(c); }
  }
}

/* ---------------------------------------------------------------- camera */
static float cam_tx, cam_ty;
static bool cam_follow = true;

/* Bq: where the camera (the map) should be for the target to sit mid-screen */
static void camera_goal(NodeId cam, NodeId target, float *gx, float *gy) {
  float px, py;
  ent_pos(target, &px, &py);
  Mat cg = node_global(cam);
  /* stage centre (480, 270) in the camera's space */
  float det = cg.a * cg.d - cg.b * cg.c;
  float cx = (cg.d * (480 - cg.tx) - cg.c * (270 - cg.ty)) / det, cy = (-cg.b * (480 - cg.tx) + cg.a * (270 - cg.ty)) / det;
  float dx = px - cx, dy = py - cy;
  /* that offset, in the camera's parent space */
  Mat cl = node_local(cam);
  float ox = cl.a * dx + cl.c * dy, oy = cl.b * dx + cl.d * dy;
  Ent *te = ent_get(target);
  if (te && comp_has(nodes[target].T, C_zObject)) oy -= .6f * te->z * 3;
  if (fabsf(ox) < .1f) ox = 0;
  if (fabsf(oy) < .1f) oy = 0;
  *gx = nodes[cam].x - ox;
  *gy = nodes[cam].y - oy;
}

void sys_camera_target(void) {
  NodeId cam = ent_camera, t = ent_first(C_cameraTarget);
  if (!cam || !t) return;
  float gx, gy, ox = 0, oy = 0;
  camera_goal(cam, t, &gx, &gy);
  comp_vec(nodes[t].T, C_cameraTarget, F_offset, &ox, &oy);
  cam_tx = gx - ox;
  cam_ty = gy - oy;
}

void camera_set_follow(bool f) { cam_follow = f; }

void sys_camera_move(void) {
  NodeId cam = ent_camera;
  if (!cam) return;
  uint16_t T = nodes[cam].T;
  float ease = comp_float(T, C_camera, F_ease, .6f), speed = comp_float(T, C_camera, F_speed, 60);
  float dx = cam_tx - nodes[cam].x, dy = cam_ty - nodes[cam].y, d = sqrtf(dx * dx + dy * dy);
  if (d > 0 && cam_follow) {
    float step = (ease * d < speed ? ease * d : speed) / d;
    nodes[cam].x += dx * step;
    nodes[cam].y += dy * step;
    ent_moved(cam);
  }
}

void sys_camera_snap(void) {
  NodeId cam = ent_camera, t = ent_first(C_cameraTarget);
  if (!cam || !t) return;
  float gx, gy;
  camera_goal(cam, t, &gx, &gy);
  nodes[cam].x = gx;
  nodes[cam].y = gy;
  cam_tx = gx;
  cam_ty = gy;
  ent_moved(cam);
}

void sys_ground_height(void) {
  for (int i = 0; i < nreg; i++) {
    NodeId n = reg[i].n;
    if (!n || !comp_has(reg[i].T, C_zObject)) continue;
    Ent *e = ent_get(n);
    if (!e || (!comp_has(reg[i].T, C_velocity) && e->has_ground)) continue;
    Rect r;
    float px[4], py[4];
    int np = 1;
    if (ent_bounds(n, &r)) {
      px[0] = r.x; py[0] = r.y; px[1] = r.x + r.w; py[1] = r.y; px[2] = r.x; py[2] = r.y + r.h; px[3] = r.x + r.w; py[3] = r.y + r.h;
      np = 4;
    } else ent_pos(n, &px[0], &py[0]);
    float g = 0;
    for (int k = 0; k < np; k++) {
      float h = phys_ground(px[k], py[k], e->z + 9);
      if (h > g) g = h;
    }
    e->ground = g < e->z ? g : e->z;
    e->has_ground = true;
  }
}

/* Oq: a translatable text shows the message with its id */
void ent_translate(NodeId e) {
  if (!e || nodes[e].T == NONE16 || !comp_has(nodes[e].T, C_translatable)) return;
  uint16_t id = comp_str(nodes[e].T, C_translatable, F_id);
  NodeId t = node_child(e, "text");
  if (id != NONE16 && t) node_set_text(t, msg(str(id)));
}

/* addFx / removeFx: spawn an effect where an entity appears or goes */
static void spawn_fx(NodeId e, int comp) {
  uint16_t s = comp_sym(nodes[e].T, comp, F_mc);
  if (s == NONE16 || !nodes[e].parent) return;
  NodeId fx = node_new_sym(s);
  if (!fx) return;
  float x, y;
  ent_local_pos(e, &x, &y);
  node_add(nodes[e].parent, fx);
  ent_register_tree(fx);
  ent_set_local_pos(fx, x, y);
  Ent *se = ent_get(e), *fe = ent_get(fx);
  if (se && fe && comp_has(nodes[fx].T, C_zObject)) fe->z = se->z;
}

void sys_add_fx(NodeId e) { if (comp_has(nodes[e].T, C_addFx)) spawn_fx(e, C_addFx); }
void sys_remove_fx(NodeId e) { if (comp_has(nodes[e].T, C_removeFx)) spawn_fx(e, C_removeFx); }

/* ---------------------------------------------------------------- physics (yr) */
static void body_center(NodeId n, const Rect *r, float *x, float *y, float *z) {
  Ent *e = ent_get(n);
  uint16_t T = nodes[n].T;
  bool zo = comp_has(T, C_zObject) && comp_has(T, C_zBoundable);
  float h = zo ? comp_float(T, C_zBoundable, F_height, 100) : 100;
  *x = r->x + r->w / 2;
  *y = r->y + r->h / 2;
  *z = (zo && e ? e->z : 0) + h / 2;
}

void sys_physics(void) {
  for (int i = 0; i < nreg; i++) {
    NodeId n = reg[i].n;
    uint16_t T = reg[i].T;
    if (!n || !comp_has(T, C_boundable) || !comp_has(T, C_collidable)) continue;
    Ent *e = ent_get(n);
    if (!e) continue;
    Rect r;
    bool has = ent_bounds(n, &r);
    bool moving = comp_has(T, C_velocity);
    if (has && e->body < 0) {
      bool zo = comp_has(T, C_zObject) && comp_has(T, C_zBoundable);
      float h = zo ? comp_float(T, C_zBoundable, F_height, 100) : 100;
      uint8_t shape = SH_BOX;
      if (zo) {
        uint16_t s = comp_str(T, C_zBoundable, F_shape);
        if (s != NONE16 && !strcmp(str(s), "leftRamp")) shape = SH_LEFT_RAMP;
        if (s != NONE16 && !strcmp(str(s), "rightRamp")) shape = SH_RIGHT_RAMP;
      }
      uint16_t bid = comp_str(T, C_collidable, F_bodyId);
      int type = bid != NONE16 && str(bid)[0] ? phys_type(str(bid)) : phys_type(moving ? "character" : "prop");
      float x, y, z;
      body_center(n, &r, &x, &y, &z);
      e->body = (int16_t)phys_add(type, shape, x, y, z, r.w / 2, r.h / 2, h / 2);
      if (e->body >= 0) bodies[e->body].user = n;
    } else if (!has && e->body >= 0) {
      phys_remove(e->body);
      e->body = -1;
    }
    if (e->body < 0) continue;
    Body *b = &bodies[e->body];
    b->active = node_visible_chain(n);
    if (moving && b->active) {
      float x, y, z;
      body_center(n, &r, &x, &y, &z);
      b->x = x; b->y = y; b->z = z;
      b->vx = 30 * e->vx; b->vy = 30 * e->vy; b->vz = 30 * e->vz;
    }
  }
  phys_step(1.0f / FPS);
  for (int i = 0; i < nreg; i++) {
    NodeId n = reg[i].n;
    uint16_t T = reg[i].T;
    if (!n || !comp_has(T, C_boundable) || !comp_has(T, C_collidable) || !comp_has(T, C_velocity)) continue;
    Ent *e = ent_get(n);
    if (!e || e->body < 0) continue;
    Body *b = &bodies[e->body];
    e->vx = b->vx / 30; e->vy = b->vy / 30; e->vz = b->vz / 30;
    Rect r;
    if (!ent_bounds(n, &r)) continue;
    float px, py;
    ent_pos(n, &px, &py);
    ent_set_pos(n, b->x + (px - (r.x + r.w / 2)), b->y + (py - (r.y + r.h / 2)));
    if (comp_has(T, C_zObject)) e->z = b->z - comp_float(T, C_zBoundable, F_height, 100) / 2;
  }
}

/* entities touching e in the last physics step (0: the ground) */
int ent_contacts(NodeId e, NodeId *out, int max) {
  Ent *s = ent_get(e);
  if (!s || s->body < 0) return 0;
  int n = 0;
  const Body *b = &bodies[s->body];
  for (int i = 0; i < b->ncontacts && n < max; i++)
    out[n++] = b->contacts[i] < 0 ? 0 : bodies[b->contacts[i]].user;
  return n;
}
