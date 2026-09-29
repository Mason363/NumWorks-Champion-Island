/* Marathon: the doodle's scene es (library 6A005E6C90D74E9A971A793C2ECC526F).
 *
 * Lucky races the tanuki along a beach track that scrolls by: the arrows
 * steer, pressing right near the front speeds up, staying on the sand path
 * builds speed and leaving it loses it. Rocks rolling in, and the projectiles
 * the spawners throw across the track, make her stumble (a quarter of her
 * speed gone); OK dodges them (40 ticks, then 150 to recharge). The time to
 * the finish is the score; the place (1st, 2nd, 3rd) gives the stars.
 * Variants: 400m (default), 800m, 1500m, 5000m.
 *
 * Systems, in the doodle's order: Ep (pause), Uo (rules), Wo (countdown),
 * Hr (triggers: path, obstacles), Ur (scrolling), Yr (new path pieces),
 * Rr (Lucky), Vr (spawners and rocks), Wr (speed lines), Xr (spawners'
 * throws), Tr (the tanuki), aq (sprite directions), zr (moves), yr (physics),
 * $r (HUD), as (the finish), Up (off screen), Vp (effects), Pp (draw order).
 * Speed lines are cosmetic streaks: kept in a table and drawn through one
 * node, not one node each. */
#include <math.h>
#include <stdio.h>
#include "ent.h"
#include "phys.h"

#define OPP_MAX 12
#define PATH_MAX 24
#define LINE_MAX 128
#define WALL_MAX 6
#define END_DELAY 80
#define MR (180 * .55f)        /* Mr */

typedef struct {
  NodeId n, cursor, text;
  float speed, ncx, ncy, vx, vy, calpha;
  bool nc;
  int16_t body;
  const char *color;
} Opp;

typedef struct { float x, y; int8_t vx; uint8_t cls; } Line;

typedef struct {
  NodeId root, map, hud, player, ref, line_proto;
  NodeId t_time, t_time_s, t_speed, t_speed_s, t_dist, t_dist_s, placement, pcursor, ptext, needle, poof;
  /* the variant */
  int total, count;
  float opp_min, opp_max;
  uint16_t spawners[8], obstacles[2];
  int nspawners, nobstacles;
  /* the map (Ug): ticks, speed build-up, speed, distance scrolled */
  uint32_t tick;
  float LL, UB, off;
  /* Lucky (Sg) */
  bool on_path, trig_path, trig_obst;
  float pcalpha;
  int vK, KQ, U_;
  float eqa, vx, vy;
  int16_t body;
  /* the tanuki (Xg) */
  Opp opp[OPP_MAX];
  int nopp;
  /* the track's walls */
  NodeId walls[WALL_MAX];
  int16_t wall_body[WALL_MAX];
  int nwalls;
  /* spawner timers (Vr) and path pieces (Yr) */
  float t_spawn, t_obst;
  NodeId path[PATH_MAX];
  int npath;
  Line line[LINE_MAX];
  int nline;
  /* flow */
  Countdown cd;
  bool started, ended, finish_shown;
  int end_t, rating;
  uint32_t score;
  char time_buf[16], speed_buf[32], dist_buf[24], place_buf[OPP_MAX + 1][12];
  uint16_t place_color[OPP_MAX + 1];
  NodeId place_text[OPP_MAX + 1];
  int nplace;
} State;

static State *S;

/* ---------------------------------------------------------------- helpers */
static bool has(NodeId n, int c) { return n && nodes[n].T != NONE16 && comp_has(nodes[n].T, c); }
static float jround(float v) { return floorf(v + 0.5f); }
static float len2(float x, float y) { return sqrtf(x * x + y * y); }
static float lerp(float a, float t, float b) { return a + (b - a) * t; }     /* eh */
static int qh(int a, int b) { return (int)floorf(frand_range((float)a, (float)b)); }
static float pos_x(NodeId n) { float x, y; ent_pos(n, &x, &y); return x; }
static float track(NodeId n) { return pos_x(n) + S->off - 27; }   /* Qr: where on the track, in px */

/* lk: frames as m:ss.cc */
static void fmt_time(char *b, size_t n, uint32_t f) {
  snprintf(b, n, "%u:%02u.%02u", (unsigned)(f / 1800 % 60), (unsigned)(f / 30 % 60), (unsigned)(f % 30 * 100 / 30));
}

/* Pr: the speed's class, 0 (still) to 4 (supernaruto) */
static int speed_class(float ub) { return ub == 0 ? 0 : ub < 5 ? 1 : ub < 6 ? 2 : ub < 7.5f ? 3 : 4; }

static void set_label(NodeId n, const char *l) {   /* Kj without keeping the pose's frame */
  if (!n || !node_has_label(n, l)) return;
  if (!strcmp(node_label(n), l)) { node_stop(n); return; }
  node_goto(n, l, 0, false);
  node_update_one(n);
}

/* The scene's entities are not put in ent.c's registry: its records would run
 * short with twelve tanuki and a dozen path pieces. The systems below walk the
 * map's children instead. */
static NodeId spawn(uint16_t sym, NodeId parent, float x, float y) {   /* Pj + Qj + Fj */
  NodeId e = node_new_sym(sym);
  if (!e) return 0;
  node_add(parent, e);
  ent_set_pos(e, x, y);
  return e;
}

static void kill(NodeId e) {   /* co */
  if (e) node_free(e);
}

/* Hr: is Lucky's origin in one of t's trigger areas (t shown)? */
static bool inside(NodeId t) {
  NodeId p = S->player;
  if (!p || !node_visible(t)) return false;
  Mat og = node_global(p);
  for (NodeId c = nodes[t].first; c; c = nodes[c].next) {
    if (!has(c, C_triggerArea)) continue;
    Mat ag = node_global(c);
    float det = ag.a * ag.d - ag.b * ag.c;
    if (fabsf(det) < 1e-9f) continue;
    float dx = og.tx - ag.tx, dy = og.ty - ag.ty;
    if (node_hit(c, (ag.d * dx - ag.c * dy) / det, (-ag.b * dx + ag.a * dy) / det)) return true;
  }
  return false;
}

static void triggers(void) {
  S->trig_path = S->trig_obst = false;
  for (NodeId c = nodes[S->map].first; c; c = nodes[c].next) {
    if (!has(c, C_trigger)) continue;
    bool in = inside(c);
    if (in && has(c, C_marathonPath)) S->trig_path = true;
    if (in && has(c, C_marathonObstacle)) S->trig_obst = true;
  }
}

/* zr: what has a velocity and no body moves by it */
static void move_direct(void) {
  for (NodeId c = nodes[S->map].first; c; c = nodes[c].next) {
    float vx = 0, vy = 0;
    if (!has(c, C_velocity) || (has(c, C_collidable) && has(c, C_boundable))) continue;
    comp_vec(nodes[c].T, C_velocity, F_velocity, &vx, &vy);
    float x, y;
    ent_pos(c, &x, &y);
    ent_set_pos(c, x + vx, y + vy);
  }
}

/* Up: far off the stage, gone. The doodle's spawners have no deleteOffScreen
 * and pile up off the left edge forever (what they throw from there is gone at
 * once): here they go too, or the nodes would run out in the long races. */
static void delete_offscreen(void) {
  for (NodeId c = nodes[S->map].first, nx; c; c = nx) {
    nx = nodes[c].next;
    if (!has(c, C_deleteOffScreen) && !has(c, C_marathonSpawn)) continue;
    Mat g = node_global(c);
    if (g.tx < -1440 || g.ty < -1440 || g.tx >= 960 + 1440 || g.ty >= 540 + 1440) {
      for (int i = 0; i < S->npath; i++) if (S->path[i] == c) S->path[i] = S->path[--S->npath];
      if (c == S->ref) S->ref = 0;
      kill(c);
    }
  }
}

/* Vp: effects end on their death frame */
static void ephemeral_in(NodeId p) {
  for (NodeId c = nodes[p].first, nx; c; c = nx) {
    nx = nodes[c].next;
    if (!has(c, C_ephemeral)) continue;
    int df = comp_int(nodes[c].T, C_ephemeral, F_deathFrame, -1);
    if (nodes[c].frame == (df >= 0 ? df : node_frames(c) - 1)) kill(c);
  }
}

/* Pp: the map's children by draw order, else by y (hidden ones first) */
static void sort_draw(void) {
  NodeId m = S->map, list[160];
  float key[160];
  int n = 0;
  for (NodeId c = nodes[m].first; c && n < 160; c = nodes[c].next) {
    float k;
    if (has(c, C_drawOrderOverride)) k = comp_float(nodes[c].T, C_drawOrderOverride, F_drawOrder, 0);
    else if (!node_visible(c)) k = -1e15f;
    else { float x; ent_pos(c, &x, &k); }
    int j = n++;
    while (j > 0 && key[j - 1] > k) { list[j] = list[j - 1]; key[j] = key[j - 1]; j--; }
    list[j] = c;
    key[j] = k;
  }
  if (nodes[m].first && n < 160) {   /* (more than 160: left as they are) */
    for (int i = 0; i < n; i++) nodes[list[i]].next = i + 1 < n ? list[i + 1] : 0;
    nodes[m].first = n ? list[0] : 0;
  }
  nodes[m].flags |= NF_SORTED;
}

/* aq: Lucky's sprites face her direction (s) */
static void sprite_dirs(void) {
  if (!S->player) return;
  for (NodeId c = nodes[S->player].first; c; c = nodes[c].next)
    if (has(c, C_sprite) && node_visible(c) && node_has_label(c, "n") && node_has_label(c, "e") && node_has_label(c, "w"))
      set_label(c, "s");
}

/* ---------------------------------------------------------------- setting up */
static void start(void) {
  S = scene_state(sizeof(State));
  phys_reset();
  /* ds: the body types of this race */
  phys_body_type("character", "bodyMaterial", 40, 4, 19, 0);
  phys_body_type("prop", "bodyMaterial", 0, 2, 4, 0);
  phys_body_type("opponent", "bodyMaterial", 10, 16, 5, 0);
  S->body = -1;
  /* the variants (es.start) */
  const char *v = game.variant;
  S->spawners[0] = S_marathon_dL;
  S->spawners[1] = S_marathon_ZK;
  S->nspawners = 2;
  S->obstacles[0] = S_marathon_kY;
  S->obstacles[1] = S_marathon_lY;
  S->nobstacles = 2;
  if (!strcmp(v, "800m")) { S->total = 800; S->count = 6; S->opp_max = 7.6f; S->opp_min = 4.2f; }
  else if (!strcmp(v, "1500m") || !strcmp(v, "5000m")) {
    bool k5 = v[0] == '5';
    S->total = k5 ? 5000 : 1500;
    S->count = k5 ? 12 : 8;
    S->opp_max = k5 ? 8.4f : 7.8f;
    S->opp_min = k5 ? 4.7f : 4.5f;
    static const uint16_t sp[7] = {S_marathon_dL, S_marathon_ZK, S_marathon_dL, S_marathon_ZK, S_marathon_dL, S_marathon_ZK, S_marathon_mja};
    memcpy(S->spawners, sp, sizeof sp);
    S->nspawners = 7;
  } else { S->total = 400; S->count = 6; S->opp_max = 7; S->opp_min = 4; }
  S->LL = 1;
  S->off = 1;
  S->pcalpha = 1;
  S->t_spawn = S->t_obst = 200;

  S->root = node_new_sym(S_marathon_kU);
  if (!S->root) return;
  node_add(game.root, S->root);
  for (NodeId c = nodes[S->root].first; c; c = nodes[c].next) {
    if (has(c, C_map)) S->map = c;
    if (has(c, C_marathonHud)) S->hud = c;
  }
  if (!S->map || !S->hud) return;
  nodes[S->map].alpha = 0;     /* drawn by draw_under, with the speed lines over it */
  S->t_time = node_find(S->hud, "time");
  S->t_time_s = node_find(S->hud, "timeShadow");
  S->t_speed = node_find(S->hud, "speed");
  S->t_speed_s = node_find(S->hud, "speedShadow");
  S->t_dist = node_find(S->hud, "dist");
  S->t_dist_s = node_find(S->hud, "distShadow");
  S->placement = node_find(S->hud, "placement");
  S->pcursor = S->placement ? node_find(S->placement, "player") : 0;
  S->ptext = S->pcursor ? node_find(S->pcursor, "place") : 0;
  NodeId sm = node_find(S->hud, "speedometer");
  S->needle = sm ? node_find(sm, "needle") : 0;
  S->poof = node_find(S->hud, "poof");
  S->ref = node_find(S->map, "ref");

  /* the map's children */
  for (NodeId c = nodes[S->map].first; c; c = nodes[c].next) {
    if (has(c, C_marathonPlayer)) S->player = c;
    else if (has(c, C_marathonOpponent) && S->nopp < OPP_MAX) {
      Opp *o = &S->opp[S->nopp++];
      o->n = c;
      o->body = -1;
      uint16_t col = comp_str(nodes[c].T, C_marathonOpponent, F_cursorColor);
      o->color = col != NONE16 ? str(col) : "op1";
    } else if (has(c, C_marathonPath) && S->npath < PATH_MAX) S->path[S->npath++] = c;
    else if (has(c, C_boundable) && has(c, C_collidable) && S->nwalls < WALL_MAX) {
      S->wall_body[S->nwalls] = -1;
      S->walls[S->nwalls++] = c;
    }
  }
  /* bs: the finish line */
  spawn(S_marathon_dfa, S->map, 24.0f * S->total + 20, 34);
  /* cs: the leftmost `count` tanuki run, in a shuffled order of speeds */
  for (int i = 1; i < S->nopp; i++)
    for (int j = i; j > 0 && nodes[S->opp[j - 1].n].x > nodes[S->opp[j].n].x; j--) {
      Opp t = S->opp[j]; S->opp[j] = S->opp[j - 1]; S->opp[j - 1] = t;
    }
  while (S->nopp > S->count) kill(S->opp[--S->nopp].n);
  for (int m = S->nopp - 1; m > 0; m--) {   /* bb: Fisher-Yates */
    int k = (int)floorf(frand() * (m + 1));
    Opp t = S->opp[m]; S->opp[m] = S->opp[k]; S->opp[k] = t;
  }
  for (int k = 0; k < S->nopp; k++) S->opp[k].speed = lerp(S->opp_min, powf((float)k / S->nopp, .85f), S->opp_max);
  /* the speed lines' shape */
  S->line_proto = node_new_sym(S_marathon_Cna);
  sys_countdown_start(&S->cd);
}

/* ---------------------------------------------------------------- Ur: the world scrolls by */
static void scroll(void) {
  for (NodeId n = nodes[S->map].first; n; n = nodes[n].next) {
    if (!has(n, C_marathonTile)) continue;
    float x, y;
    ent_pos(n, &x, &y);
    ent_set_pos(n, x - S->UB, y);
  }
}

/* ---------------------------------------------------------------- Yr: the path goes on */
static float path_width(NodeId n) { return (float)comp_int(nodes[n].T, C_marathonPath, F_width, 64); }

static void paths(void) {
  /* sorted by x; gone once past the left edge */
  for (int i = 1; i < S->npath; i++)
    for (int j = i; j > 0 && pos_x(S->path[j - 1]) > pos_x(S->path[j]); j--) {
      NodeId t = S->path[j]; S->path[j] = S->path[j - 1]; S->path[j - 1] = t;
    }
  int k = 0;
  for (int i = 0; i < S->npath; i++) {
    NodeId n = S->path[i];
    if (pos_x(n) < -path_width(n)) kill(n);
    else S->path[k++] = n;
  }
  S->npath = k;
  if (!k) return;
  NodeId g = S->path[k - 1];
  float m = path_width(g), b = pos_x(g) + m;
  if (b >= 800) return;
  uint16_t sym;
  if ((track(g) + m) / 24 > S->total) sym = S_marathon_hka;
  else {
    uint16_t e = comp_str(nodes[g].T, C_marathonPath, F_end);
    const char *end = e != NONE16 ? str(e) : "";
    static const uint16_t next[3][3] = {{S_marathon_aL, S_marathon_Zja, S_marathon__S_ja},
                                        {S_marathon_aka, S_marathon_bka, S_marathon_cka},
                                        {S_marathon_eka, S_marathon_fka, S_marathon_gka}};
    if (end[0] < 'a' || end[0] > 'c') return;   /* after the finish: nothing (the doodle throws) */
    sym = next[end[0] - 'a'][qh(0, 3)];
  }
  if (S->npath >= PATH_MAX) return;
  NodeId n = spawn(sym, S->map, b, 0);
  if (n) S->path[S->npath++] = n;
}

/* ---------------------------------------------------------------- Rr: Lucky */
static void lucky(void) {
  NodeId b = S->player;
  if (!b) return;
  float mx, my;
  ent_pos(b, &mx, &my);
  S->on_path = S->trig_path;
  if (S->trig_obst && S->vK == 0 && S->KQ == 0) {
    SOUND(COa);
    S->vK = 30;
    S->LL *= .25f;
  }
  int mask;
  if (S->vK > 0) { S->vK--; mask = 3; }
  else if (S->KQ > 0) {
    S->KQ--;
    mask = 3;
    if (S->KQ == 4) { NodeId fx = node_new_sym(S_marathon_kXa); if (fx) node_add(b, fx); }
  } else {
    mask = 19;
    if (S->on_path) S->LL++;
    else S->LL *= .9f;
  }
  if (S->body >= 0) bodies[S->body].mask = (uint16_t)mask;
  if (S->U_ > 0) S->U_--;
  else if (S->vK == 0 && S->KQ == 0 && in.pressed[A_ACTION]) {
    S->KQ = 40;
    S->U_ = 150;
    NodeId fx = node_new_sym(S_marathon_iXa);
    if (fx) node_add(b, fx);
  }
  S->UB = sqrtf(111 + S->LL) / 3;
  S->eqa = fminf(2.5f, .45f * S->UB);
  for (NodeId c = nodes[b].first; c; c = nodes[c].next)
    if (has(c, C_sprite)) nodes[c].alpha = (S->vK % 8 < 4 && S->KQ == 0) ? 255 : 77;
  S->off += S->UB;
  float cl = len2(in.jx, in.jy);
  if (cl > 0) {
    S->vx = in.jx / cl * S->eqa * cl;
    S->vy = in.jy / cl * S->eqa * cl;
    float a = clampf((mx / 320 - .2f) / .2f, 0, 1), n = S->vK == 0 ? (1 - a) * (1 - a) : 0;
    if (S->vx > 0) { S->vx *= n; S->LL += a * a * .4f; }
  } else S->vx = S->vy = 0;
  if (cl == 0 || S->vK > 0) S->vx -= 5 * clampf(mx / 320 - .1f, 0, .4f);
  static const char *const poses[5] = {"idle", "walk", "run", "naruto", "supernaruto"};
  int m = speed_class(S->UB);
  set_label(b, S->vK > 0 ? "stumble" : poses[m]);
  S->tick++;
}

/* ---------------------------------------------------------------- Vr: spawners and rocks come in */
static void spawners(void) {
  float m = 5 / powf(S->UB, 1.5f);
  if (S->off / 24 + 30 > S->total) return;
  S->t_spawn--;
  if (S->UB > 4.5f && S->t_spawn <= 0) {
    S->t_spawn = qh(80, 140) * m;
    NodeId k = spawn(S->spawners[qh(0, S->nspawners)], S->map, 0, 0);
    if (k) {
      uint16_t d = comp_str(nodes[k].T, C_marathonSpawn, F_direction);
      const char *dir = d != NONE16 ? str(d) : "";
      if (!strcmp(dir, "s")) ent_set_pos(k, 340, 20);
      else if (!strcmp(dir, "n")) ent_set_pos(k, 340, 160);
    }
  }
  S->t_obst--;
  if (S->UB > 4.5f && S->t_obst <= 0) {
    S->t_obst = qh(170, 200) * m;
    spawn(S->obstacles[qh(0, S->nobstacles)], S->map, 340, (float)qh(40, 140));
  }
}

/* ---------------------------------------------------------------- Wr: speed lines */
static void speed_lines(void) {
  static const float counts[5] = {0, 0, 1, 2, 4};
  int g = speed_class(S->UB);
  float a = counts[g];
  int n = frand() > fmodf(a, 1) ? (int)ceilf(a) : (int)floorf(a);
  static const int8_t vel[5] = {0, 0, -20, -25, -30};
  for (int i = 0; i < n && S->nline < LINE_MAX; i++)
    S->line[S->nline++] = (Line){(float)(340 - qh(1, 10)), (float)qh(5, 175), vel[g], (uint8_t)g};
}

/* zr for the lines; they are gone once off the screen (Up takes them much later) */
static void move_lines(void) {
  static const float sx[5] = {0, 0, 1, 1.5f, 2};
  int k = 0;
  for (int i = 0; i < S->nline; i++) {
    Line *l = &S->line[i];
    l->x += l->vx;
    if (l->x + 60 * sx[l->cls] >= 0) S->line[k++] = *l;
  }
  S->nline = k;
}

/* ---------------------------------------------------------------- Xr: what spawners throw */
static void throws(void) {
  for (NodeId n = nodes[S->map].first; n; n = nodes[n].next) {
    if (!has(n, C_marathonSpawn)) continue;
    int f = comp_int(nodes[n].T, C_marathonSpawn, F_spawnFrame, -1);
    uint16_t s = comp_sym(nodes[n].T, C_marathonSpawn, F_toSpawn);
    if (nodes[n].frame != f || s == NONE16) continue;
    float x, y;
    ent_pos(n, &x, &y);
    spawn(s, S->map, x, y);
  }
}

/* ---------------------------------------------------------------- Tr: the tanuki */
static void tanuki(void) {
  NodeId p = S->player;
  if (!p) return;
  float kx, ky;
  ent_pos(p, &kx, &ky);
  for (int i = 0; i < S->nopp; i++) {
    Opp *o = &S->opp[i];
    float hx, hy;
    ent_pos(o->n, &hx, &hy);
    set_label(o->n, "idle");
    if (!o->nc) { o->nc = true; o->ncx = hx; o->ncy = hy; }
    float e = .85f * (o->ncy - MR);
    float vx = (hx - kx) / 24, vy = (hy - ky) / 24, u = 1;
    if (vx < 4) u = powf(1 + clampf(fabsf(vx - 4), 0, 12) / 100, 1.5f);
    /* (the doodle's bonus for runners "3" and "4" compares a string with numbers: never) */
    float E = track(o->n) / 24, left = S->total - track(o->n) / 24;
    if (left < 100 && left > 0) u += .1f * sqrtf(1 - left / 100);
    if (E < 100) u += E / 100 * .05f;
    if (E < 3) u *= .4f + .6f * E / 3;
    float d = o->speed;
    if (E > 20 && vx < 5) {
      float n = powf((6 - fabsf(clampf(vx + 1, -5, 6))) / 9, 1.5f);
      d = d * (1 - n) + S->UB * n;
    }
    float nx = d * u, ny = 0;
    /* towards the next waypoint ahead, in its own lane */
    NodeId wp = 0;
    float wx = 1e9f, wy = 0;
    for (int j = 0; j < S->npath; j++)
      for (NodeId w = nodes[S->path[j]].first; w; w = nodes[w].next) {
        if (!has(w, C_marathonWaypoint)) continue;
        float x, y;
        ent_pos(w, &x, &y);
        if (x > hx && x < wx) { wp = w; wx = x; wy = y; }
      }
    if (wp) {
      float ex = wx - hx, ey = wy + e - hy, el = len2(ex, ey);
      if (el > 0) {
        u += .15f * ey / el;
        float s = fminf(d * u, el);
        nx = ex / el * s;
        ny = ey / el * s;
      } else nx = ny = 0;
    } else { nx = .7f * d * u; ny = 0; }
    if (S->on_path && E > 20 && fabsf(vy) < 1.5f && vx < 5 && vx > 0)
      ny = lerp(ny, powf(.25f - vx / 5 * .22f, 1.4f), ky - hy);
    o->vx += (nx - o->vx) * .2f;
    o->vy += (ny - o->vy) * .2f;
  }
}

/* ---------------------------------------------------------------- yr: physics */
static int16_t body_for(NodeId n, int type) {
  Rect r;
  if (!ent_bounds(n, &r)) return -1;
  int b = phys_add(type, SH_BOX, r.x + r.w / 2, r.y + r.h / 2, 50, r.w / 2, r.h / 2, 50);
  if (b >= 0) bodies[b].user = n;
  return (int16_t)b;
}

static void sync_in(NodeId n, int16_t b, float vx, float vy) {
  Rect r;
  if (b < 0 || !ent_bounds(n, &r)) return;
  Body *bd = &bodies[b];
  bd->x = r.x + r.w / 2;
  bd->y = r.y + r.h / 2;
  bd->z = 50;
  bd->vx = 30 * vx;
  bd->vy = 30 * vy;
  bd->vz = 0;
}

static void sync_out(NodeId n, int16_t b, float *vx, float *vy) {
  Rect r;
  if (b < 0 || !ent_bounds(n, &r)) return;
  Body *bd = &bodies[b];
  *vx = bd->vx / 30;
  *vy = bd->vy / 30;
  float px, py;
  ent_pos(n, &px, &py);
  ent_set_pos(n, bd->x + (px - (r.x + r.w / 2)), bd->y + (py - (r.y + r.h / 2)));
}

static void physics(void) {
  int tc = phys_type("character"), to = phys_type("opponent"), tp = phys_type("prop");
  if (S->player && S->body < 0) S->body = body_for(S->player, tc);
  for (int i = 0; i < S->nopp; i++)
    if (S->opp[i].body < 0) S->opp[i].body = body_for(S->opp[i].n, to);
  for (int i = 0; i < S->nwalls; i++)
    if (S->wall_body[i] < 0) S->wall_body[i] = body_for(S->walls[i], tp);
  sync_in(S->player, S->body, S->vx, S->vy);
  for (int i = 0; i < S->nopp; i++) sync_in(S->opp[i].n, S->opp[i].body, S->opp[i].vx, S->opp[i].vy);
  phys_step(1.0f / FPS);
  sync_out(S->player, S->body, &S->vx, &S->vy);
  for (int i = 0; i < S->nopp; i++) sync_out(S->opp[i].n, S->opp[i].body, &S->opp[i].vx, &S->opp[i].vy);
}

/* ---------------------------------------------------------------- $r: HUD */
static void set_text2(NodeId a, NodeId b, const char *s) {
  if (a) node_set_text(a, s);
  if (b) node_set_text(b, s);
}

static void hud(void) {
  NodeId b = S->player;
  if (!b) return;
  float kmh = S->UB / 24 * 108;
  int tenths = (int)jround(kmh * 10);   /* toFixed(1) (the calculator's printf has no %f) */
  snprintf(S->speed_buf, sizeof S->speed_buf, "%d.%dkm/h", tenths / 10, tenths % 10);
  set_text2(S->t_speed, S->t_speed_s, S->speed_buf);
  float h = track(b) / 24;
  snprintf(S->dist_buf, sizeof S->dist_buf, "%dm/%dm", (int)jround(h), S->total);
  set_text2(S->t_dist, S->t_dist_s, S->dist_buf);
  if (S->needle) {
    int n = (int)floorf(22 * clampf((kmh - 18) / 20, 0, 1));
    n = (int)jround(lerp(nodes[S->needle].frame, .3f, (float)n));
    if (n == 22 && frand() < .5f) n--;
    node_goto(S->needle, NULL, n, false);
  }
  if (S->poof) node_goto(S->poof, NULL, (int)jround(10 * (1 - S->U_ / 150.0f)), false);
  fmt_time(S->time_buf, sizeof S->time_buf, S->tick);
  set_text2(S->t_time, S->t_time_s, S->time_buf);
  if (S->ref && S->tick == 1) { node_play(S->ref); S->ref = 0; }   /* the referee's flag */
  if (!S->placement) return;
  /* the runners around Lucky, by x */
  NodeId a[OPP_MAX + 1];
  int na = 0, idx[OPP_MAX + 1];
  a[na] = b; idx[na++] = -1;
  for (int i = 0; i < S->nopp; i++) { a[na] = S->opp[i].n; idx[na++] = i; }
  for (int i = 1; i < na; i++)
    for (int j = i; j > 0 && nodes[a[j - 1]].x > nodes[a[j]].x; j--) {
      NodeId t = a[j]; a[j] = a[j - 1]; a[j - 1] = t;
      int ti = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = ti;
    }
  int g = 0;
  while (g < na && a[g] != b) g++;
  S->nplace = na;
  int n0 = g - (na - 5) > 0 ? g - (na - 5) : 0;
  for (int f = 0; f < na; f++) {
    NodeId d, t;
    char *buf = S->place_buf[f];
    uint16_t *col = &S->place_color[f];
    float *ca = &S->pcalpha;
    if (idx[f] < 0) { d = S->pcursor; t = S->ptext; }
    else {
      Opp *o = &S->opp[idx[f]];
      if (!o->cursor) {
        o->cursor = node_new_sym(S_marathon_JWa);
        if (o->cursor) {
          node_add(S->placement, o->cursor);
          node_goto(o->cursor, o->color, 0, false);
          node_update_one(o->cursor);
          o->text = node_find(o->cursor, "place");
          nodes[o->cursor].alpha = 255;
          o->calpha = 1;
        }
      }
      d = o->cursor;
      t = o->text;
      ca = &o->calpha;
    }
    if (!d) continue;
    int v = n0 + (f - g);
    float u = 6 + 105 * clampf((float)v, 0, 4);
    *col = rgb565(0x11, 0xae, 0x00);
    if (v == n0) snprintf(buf, 12, "%d", na - f);
    else {
      float fm = fabsf(jround(track(a[f]) / 24 - h));
      unsigned m = fm > 99 || fm != fm ? 99u : (unsigned)fm;
      if (v > n0) { u -= 30; snprintf(buf, 12, "+%um", m); *col = rgb565(0x11, 0xcc, 0x11); }
      else { snprintf(buf, 12, "-%um", m); *col = rgb565(0xcc, 0x11, 0x11); }
    }
    if (t) node_set_text(t, "");     /* drawn by draw_over, in its colour */
    S->place_text[f] = t;
    float e = (v >= 0 && v < 5) ? 1 : 0;
    float cx, cy;
    ent_local_pos(d, &cx, &cy);
    ent_set_local_pos(d, lerp(cx, .5f, u), lerp(cy, .5f, 0));
    *ca = lerp(*ca, .5f, e);
    nodes[d].alpha = (uint8_t)(*ca * 255);
  }
  nodes[S->placement].alpha = (uint8_t)(clampf(((float)S->tick - 45) / 30, 0, 1) * 255);
}

/* ---------------------------------------------------------------- the finish (as) */
static void finish(void) {
  if (S->ended) {
    if (S->end_t > 0 && --S->end_t == 0) menus_game_over_rated((float)S->score, S->rating);
    return;
  }
  NodeId b = S->player;
  if (!b || track(b) / 24 <= S->total) return;
  if (S->body >= 0) bodies[S->body].mask = 1;
  S->vx = len2(S->vx, S->vy) + S->UB;
  S->vy = 0;
  S->UB = 0;
  int m = 0;
  float bx = pos_x(b);
  for (int i = 0; i < S->nopp; i++) {
    Opp *o = &S->opp[i];
    o->vx = len2(o->vx, o->vy);
    o->vy = 0;
    if (pos_x(o->n) > bx) m++;
  }
  if (m == 0) toast_full(msg("FIRST_PLACE"), 80, 0x111111, 0xa18722, 0xffde38);
  else if (m == 1) toast_full(msg("SECOND_PLACE"), 80, 0x111111, 0x5f6263, 0xa1a4a6);
  else if (m == 2) toast_full(msg("THIRD_PLACE"), 80, 0x111111, 0x6b4a29, 0xb58657);
  else toast(msg("FINISH"));
  S->ended = true;
  S->end_t = END_DELAY;
  S->score = S->tick;
  S->rating = 3 - m > 0 ? 3 - m : 0;
  SOUND($Za);
}

/* ---------------------------------------------------------------- the scene */
static void tick(void) {
  sys_back_pauses();                     /* Ep */
  if (!S || !S->map || menus_active()) return;
  if (!S->started) {                     /* Uo */
    S->started = true;
    sys_tutorial_once();
    if (menus_active()) return;
  }
  bool go = sys_countdown(&S->cd);       /* Wo */
  bool on = go && !S->ended;
  if (go) triggers();                    /* Hr */
  if (go) scroll();                      /* Ur */
  if (on) paths();                       /* Yr */
  if (on) lucky();                       /* Rr */
  if (on) spawners();                    /* Vr */
  if (go) speed_lines();                 /* Wr */
  if (go) throws();                      /* Xr */
  if (on) tanuki();                      /* Tr */
  if (go) sprite_dirs();                 /* aq */
  if (go) { move_direct(); move_lines(); }       /* zr */
  if (go) physics();                     /* yr */
  if (!S->ended) hud();                  /* $r */
  if (go) finish();                      /* as */
  if (go) delete_offscreen();            /* Up */
  ephemeral_in(S->map);                  /* Vp */
  if (S->player) ephemeral_in(S->player);
  sort_draw();                           /* Pp */
}

/* ---------------------------------------------------------------- drawing */
static void draw_under(void) {
  if (!S || !S->map) return;
  Mat third = {1.0f / 3, 0, 0, 1.0f / 3, 0, 0};
  Mat pm = mat_mul(third, node_global(S->root));
  /* path pieces wait off the right edge (up to x 800): not drawn there */
  for (int i = 0; i < S->npath; i++) {
    float x = pos_x(S->path[i]);
    node_set_visible(S->path[i], x < VIEW_W + 16 && x + path_width(S->path[i]) > -16);
  }
  nodes[S->map].alpha = 255;
  node_draw_in(S->map, pm, 255);
  nodes[S->map].alpha = 0;
  for (int i = 0; i < S->npath; i++) node_set_visible(S->path[i], true);
  /* the speed lines, over the map (drawOrder 1000) */
  if (!S->line_proto) return;
  static const float sx[5] = {0, 0, 1, 1.5f, 2};
  static const uint8_t al[5] = {0, 0, 77, 102, 128};
  Mat mm = mat_mul(third, node_global(S->map));
  for (int i = 0; i < S->nline; i++) {
    Line *l = &S->line[i];
    nodes[S->line_proto].x = l->x;
    nodes[S->line_proto].y = l->y;
    nodes[S->line_proto].alpha = al[l->cls];
    node_xform(S->line_proto, sx[l->cls], 1, 0);
    node_draw(S->line_proto, mm);
  }
}

/* a text of the HUD, in a colour of its own (the doodle sets Text.color) */
static void draw_text_as(NodeId t, const char *s, uint16_t color) {
  if (!t || !s[0] || nodes[t].ref == NONE16 || !node_visible_chain(t)) return;
  uint8_t alpha = 255;
  for (NodeId p = t; p; p = nodes[p].parent) alpha = (uint8_t)((alpha * nodes[p].alpha + 127) / 255);
  if (!alpha) return;
  node_text_draw(t, mat_mul((Mat){1.0f / 3, 0, 0, 1.0f / 3, 0, 0}, node_global(t)), s, color, alpha);
}

static void draw_over(void) {
  if (!S || !S->map) return;
  /* the placement bar's texts */
  if (S->placement && nodes[S->placement].alpha) {
    for (int f = 0; f < S->nplace; f++) draw_text_as(S->place_text[f], S->place_buf[f], S->place_color[f]);
  }
}

static void end(void) {
  if (S && S->line_proto) node_free(S->line_proto);
  phys_reset();
  S = NULL;
}

static void goto_frame(const char *label) { (void)label; }   /* the variant is read in start() */

const SceneDef scene_marathon = {"marathon", start, tick, end, draw_under, draw_over, goto_frame};
