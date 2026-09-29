/* Archery: the doodle's scene "gq" (kitsune20.js, the systems from "ap" to
 * "lp" before it), with its systems in the doodle's order:
 *
 *   Ep  Back pauses          Uo  the rules, the first time
 *   Wo  3, 2, 1, Go (Sp lp Vp kp fp jp np wait for it; targets already come)
 *   Sp  the champion walks to its waypoint      Vp  effects end
 *   kp  the player moves and shoots while OK is held
 *   cp  targets: normal ones from the sides, fans anywhere, barrels that explode
 *   fp  arrows hit targets (points grow with each target an arrow goes through)
 *   ep  targets drift across, faster lower down and as time runs out
 *   ip  the HUD      jp  Yoichi shoots when a target will pass over him, and moves
 *   np  the walls stop the player      fq  velocities move things
 *   lp  60 seconds, then the banner and the results      Pp  draw order */
#include <math.h>
#include <stdio.h>
#include "../src/ent.h"
#include "../src/font.h"

#define TARGET_MAX 72
#define ARROW_MAX 24

enum { T_NORMAL, T_EXPLOSIVE, T_FAN, T_OBSTACLE, T_ADDBOMB, T_ADDPIERCE, T_OTHER };
enum { A_NORMAL, A_PIERCE, A_BOMB, A_EXPLOSIVE };
enum { TEAM_NONE, TEAM_PLAYER, TEAM_CHAMP };

typedef struct {
  NodeId n;
  uint8_t type;
  bool left;          /* came in on the left: goes right */
  bool dead;
  float vx;           /* its velocity component */
} Target;

typedef struct {
  NodeId n;
  uint8_t type, team;
  uint8_t hits;       /* RL.length */
  bool dead;
  float vy;
} Arrow;

typedef struct { uint8_t side; uint8_t type; int16_t ymin, ymax; int wait, every; bool on; } Spawner;   /* bp */

typedef struct {
  NodeId root, map, hud, player, champ, overlay;
  NodeId back, sky;   /* the background clip, its gradient (drawn as bands) */
  NodeId walls[2];
  int nwalls;
  NodeId t_score, t_champ, t_time, t_time2, avatar, cavatar, arrow_ui, carrow_ui, arrow_inf;
  /* archeryGame */
  int tick;
  float zx;
  int score, cscore;
  /* archeryPlayer */
  int pw, zn, ut;
  uint8_t next;       /* u_: the next arrow */
  float pvx, pvy;
  /* archeryChampion */
  int cut;
  float caa, cpw, czn;   /* Caa, Pw, zN (fractions of ticks, as in the doodle) */
  float cvx, cvy;
  bool has_wp;
  float wpx, wpy, wps;
  Spawner sp[3];
  Target targets[TARGET_MAX];
  int ntargets;
  Arrow arrows[ARROW_MAX];
  int narrows;
  int cd;
  bool counting, uo;
  bool over, reported;
  int end_t, rating;
  int last[6];
  int arrow_f, carrow_f;   /* the arrow meters' frames */
  char s_score[12], s_cscore[12], s_time[12];
  char nums[2][41][6];   /* the points that pop up, green (player) and red (champion) */
} State;

static State *S;
static const char TXT_BLOCKED[] = "BLOCKED", TXT_BOMB[] = "+BOMB", TXT_PIERCE[] = "+PIERCE", TXT_EMPTY[] = "";

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

static void mat_box(Mat m, float *x, float *y, float *w, float *h) {
  float xs[4] = {*x, *x + *w, *x, *x + *w}, ys[4] = {*y, *y, *y + *h, *y + *h};
  float lx = 1e9f, ly = 1e9f, hx = -1e9f, hy = -1e9f;
  for (int i = 0; i < 4; i++) {
    float X = m.a * xs[i] + m.c * ys[i] + m.tx, Y = m.b * xs[i] + m.d * ys[i] + m.ty;
    lx = fminf(lx, X); hx = fmaxf(hx, X); ly = fminf(ly, Y); hy = fmaxf(hy, Y);
  }
  *x = lx; *y = ly; *w = hx - lx; *h = hy - ly;
}

/* createjs getBounds(): the library's "bounds" clip sets its own
 * (this.setBounds(-36, -36, 72, 72)); anything else: the union of its timeline */
static bool sym_bounds(uint16_t sym, unsigned frame, int depth, float *bx, float *by, float *bw, float *bh) {
  if (sym == S_archery_Ta) { *bx = -36; *by = -36; *bw = 72; *bh = 72; return true; }
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

/* the node for one of a clip's timeline children drawn from the data */
static NodeId materialize_slot(NodeId m, unsigned slot) {
  Clip c;
  if (!clip_get(nodes[m].sym, &c) || slot >= c.nslots) return 0;
  const uint8_t *p = c.slots;
  for (unsigned s = 0; s < c.nslots; s++) {
    unsigned nk;
    p = varint(p, &nk);
    TKey cur = {0}, k;
    int idx = -1;
    for (unsigned i = 0; i < nk; i++) {
      p = tkey(p, &k);
      if (k.start <= nodes[m].frame) { cur = k; idx = (int)i; }
    }
    if (s != slot) continue;
    if (idx < 0 || (cur.flags & K_ABSENT) || cur.kind != CK_SYM) return 0;
    for (NodeId ch = nodes[m].first; ch; ch = nodes[ch].next)
      if (nodes[ch].slot == s) return ch;
    NodeId n = node_new_sym(cur.ref);
    if (!n) return 0;
    Node *q = &nodes[n];
    q->x = cur.x; q->y = cur.y; q->rx4 = cur.rx4; q->ry4 = cur.ry4; q->mat = cur.mat; q->alpha = cur.alpha;
    q->parent = m;
    q->slot = (uint16_t)s;
    q->key = (uint16_t)idx;
    q->next = nodes[m].first;   /* slots in order: this is the first */
    nodes[m].first = n;
    return n;
  }
  return 0;
}

/* Mj(..., advance): a new clip shows its first frame and runs its script */
static void init_frame0(NodeId n, int depth) {
  Node *p = &nodes[n];
  if (p->kind == NK_CLIP && node_mode(p) == MODE_INDEPENDENT && p->frame == 0 && (p->flags & NF_ONSTAGE))
    node_goto(n, NULL, 0, (p->flags & NF_PLAYING) != 0);
  if (depth < 8)
    for (NodeId c = p->first; c; c = nodes[c].next) init_frame0(c, depth + 1);
}

/* ---------------------------------------------------------------- positions (in the map's space) */
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

static void move_by(NodeId n, float dx, float dy) {  /* fq: Dj(b, Cj(b) + velocity) */
  Mat t = node_local(n);
  set_local(n, t.tx + dx, t.ty + dy);
}

static NodeId bounds_child(NodeId e) {
  for (NodeId c = nodes[e].first; c; c = nodes[c].next)
    if (has(c, C_bounds)) return c;
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

/* Sj(): bounds (or positions) overlap, as createjs Rectangle.intersects */
static bool overlap(NodeId a, NodeId b) {
  Rect ra, rb;
  if (!box_of(a, &ra)) { pos_of(a, &ra.x, &ra.y); ra.w = ra.h = 0; }
  if (!box_of(b, &rb)) { pos_of(b, &rb.x, &rb.y); rb.w = rb.h = 0; }
  return !(rb.x >= ra.x + ra.w || ra.x >= rb.x + rb.w || rb.y >= ra.y + ra.h || ra.y >= rb.y + rb.h);
}

static bool contains(Rect r, float x, float y) { return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h; }

/* Rj(): a new entity in the map at (x, y) */
static NodeId spawn(uint16_t sym, float x, float y) {
  NodeId e = ent_spawn(sym);
  if (!e) return 0;
  ent_add(e, S->map);
  init_frame0(e, 0);
  set_pos(e, x, y);
  return e;
}

static void goto_frame(NodeId n, int f, int *last) {   /* gotoAndStop(frame), once */
  if (!n || f == *last) return;
  node_goto(n, NULL, f, false);
  *last = f;
}

/* ---------------------------------------------------------------- records */
static void kill_target(Target *t) {   /* co(): removeFx shows the broken target */
  if (t->dead) return;
  t->dead = true;
  ent_remove(t->n);
  t->n = 0;
}

static void kill_arrow(Arrow *a) {
  if (a->dead) return;
  a->dead = true;
  ent_remove(a->n);
  a->n = 0;
}

static void compact(void) {
  int j = 0;
  for (int i = 0; i < S->ntargets; i++)
    if (!S->targets[i].dead) S->targets[j++] = S->targets[i];
  S->ntargets = j;
  j = 0;
  for (int i = 0; i < S->narrows; i++)
    if (!S->arrows[i].dead) S->arrows[j++] = S->arrows[i];
  S->narrows = j;
}

/* $o: points and words that pop up (green for the player, red for Yoichi) */
static void popup(float x, float y, const char *s) {
  NodeId p = spawn(S_archery_f5, x, y);
  NodeId t = p ? node_find(p, "text") : 0;
  if (t) node_set_text(t, s);
}

static const char *points(int d, int team) {
  int k = d / 50;
  k = k < 0 ? 0 : k > 40 ? 40 : k;
  return S->nums[team == TEAM_CHAMP][k];
}

/* ap: an arrow leaves the bow */
static void shoot(float x, float y, uint8_t type, uint8_t team) {
  if (S->narrows >= ARROW_MAX) return;
  NodeId n = spawn(type == A_BOMB ? S_archery_sya : type == A_PIERCE ? S_archery_Y4 : S_archery_VL, x, y);
  if (!n) return;
  S->arrows[S->narrows++] = (Arrow){n, type, team, 0, false, team == TEAM_CHAMP ? -13.0f : -10.0f};
}

/* ---------------------------------------------------------------- fp: hits */
static void hit(Arrow *a, Target *t);

/* hp: an explosion hits every target within 35 px */
static void explode(float x, float y, uint8_t team) {
  SOUND(explosion);
  Arrow boom = {0, A_EXPLOSIVE, team, 0, false, 0};
  for (int i = 0; i < S->ntargets; i++) {
    Target *t = &S->targets[i];
    if (t->dead) continue;
    float tx, ty;
    pos_of(t->n, &tx, &ty);
    if (hypotf(tx - x, ty - y) < 35) hit(&boom, t);
  }
  spawn(S_archery_yDa, x, y);
}

/* gp */
static void hit(Arrow *a, Target *t) {
  float x, y;
  pos_of(t->n, &x, &y);
  if (a->type == A_BOMB) {
    kill_arrow(a);
    explode(x, y, a->team);
    return;
  }
  const char *e = TXT_EMPTY;
  if (a->team == TEAM_PLAYER) S->ut = S->ut + 1 < 45 ? S->ut + 1 : 45;
  else if (a->team == TEAM_CHAMP) S->cut = S->cut + 1 < 30 ? S->cut + 1 : 30;
  a->hits++;
  bool through = a->type == A_PIERCE || a->type == A_EXPLOSIVE;
  int d = 0;
  switch (t->type) {
    case T_OBSTACLE:
      if (through) { d = 50 * a->hits; e = points(d, a->team); kill_target(t); }
      else {
        SOUND(blocked);
        if (a->team == TEAM_PLAYER) popup(x, y, TXT_BLOCKED);
        a->hits = 0;
        kill_arrow(a);
        return;
      }
      break;
    case T_NORMAL:
      d = 100 * a->hits;
      e = points(d, a->team);
      if (!through) kill_arrow(a);
      kill_target(t);
      break;
    case T_FAN:
      d = 50 * a->hits;
      e = points(d, a->team);
      kill_target(t);
      break;
    case T_ADDBOMB:
    case T_ADDPIERCE:
      if (a->team == TEAM_PLAYER) { S->next = t->type == T_ADDBOMB ? A_BOMB : A_PIERCE; e = t->type == T_ADDBOMB ? TXT_BOMB : TXT_PIERCE; }
      else if (a->team == TEAM_CHAMP) { d = 50; e = points(d, a->team); }
      kill_target(t);
      break;
    case T_EXPLOSIVE:
      kill_target(t);
      explode(x, y, a->team);
      if (!through) kill_arrow(a);
      break;
    default: break;
  }
  SOUND(hit);
  if (a->team == TEAM_PLAYER) S->score += d;
  else if (a->team == TEAM_CHAMP) S->cscore += d;
  popup(x, y, e);
}

static float target_y(const Target *t) {
  float x, y;
  pos_of(t->n, &x, &y);
  return y;
}

static void arrows_hit(void) {   /* fp */
  /* the targets, lowest first (a copy: removed ones are skipped) */
  int order[TARGET_MAX], n = 0;
  float ys[TARGET_MAX];
  for (int i = 0; i < S->ntargets; i++) {
    if (S->targets[i].dead) continue;
    float y = target_y(&S->targets[i]);
    int j = n++;
    for (; j > 0 && ys[j - 1] < y; j--) { ys[j] = ys[j - 1]; order[j] = order[j - 1]; }
    ys[j] = y;
    order[j] = i;
  }
  int na = S->narrows;
  for (int i = 0; i < na; i++) {
    Arrow *a = &S->arrows[i];
    if (a->dead) continue;
    if (nodes[a->n].y < -13) {
      if (!a->hits && a->team == TEAM_PLAYER && S->overlay && nodes[S->overlay].alpha == 0) {
        float x, y;
        pos_of(a->n, &x, &y);
        popup(x, 40, msg("MISS"));
      }
      kill_arrow(a);
      continue;
    }
    for (int k = 0; k < n; k++) {
      Target *t = &S->targets[order[k]];
      if (t->dead || a->dead) continue;
      if (overlap(t->n, a->n)) { hit(a, t); break; }
    }
  }
}

/* ---------------------------------------------------------------- cp, ep: targets */
static int target_type(uint16_t sym) {
  uint16_t T = nodes[sym].T;
  uint16_t s = comp_str(T, C_archeryTarget, F_type);
  const char *n = s != NONE16 ? str(s) : "";
  if (!strcmp(n, "normal")) return T_NORMAL;
  if (!strcmp(n, "explosive")) return T_EXPLOSIVE;
  if (!strcmp(n, "fan")) return T_FAN;
  if (!strcmp(n, "obstacle")) return T_OBSTACLE;
  if (!strcmp(n, "addBomb")) return T_ADDBOMB;
  if (!strcmp(n, "addPierce")) return T_ADDPIERCE;
  return T_OTHER;
}

static void spawn_targets(void) {   /* cp */
  float zx = (2 - S->tick / 1799.0f) * 1.2f - .2f;
  S->zx = zx = clampf(zx, 1, 2);
  S->sp[0].every = (int)floorf(100 - 38 * zx);
  S->sp[1].every = (int)floorf(450 - 90 * zx);
  S->sp[1].on = 1.05f < zx;
  S->sp[2].every = (int)floorf(60 - 25 * zx);
  S->sp[2].on = 1 < zx;
  for (int i = 0; i < 3; i++) {
    Spawner *g = &S->sp[i];
    if (!g->on) continue;
    if (g->wait == 0 && S->ntargets < TARGET_MAX) {
      uint16_t sym = g->type == T_EXPLOSIVE ? S_archery_FDa : g->type == T_FAN ? S_archery_a2 : S_archery_a_;
      NodeId k = ent_spawn(sym);
      if (k) {
        ent_add(k, S->map);
        init_frame0(k, 0);
        Rect c = {0, 0, 0, 0};
        float ax, ay;
        box_of(k, &c);
        pos_of(k, &ax, &ay);
        ax -= c.x;
        float left = ax - c.w, right = 320 + ax, x = frand_range(30, 290);
        if (g->side == 0) x = .5f < frand() ? left : right;   /* "both" */
        Target *t = &S->targets[S->ntargets++];
        *t = (Target){k, (uint8_t)target_type(k), 160 > x, false, 0};
        set_pos(k, x, frand_range(g->ymin, g->ymax));
      }
      g->wait = g->every;
    }
    g->wait = g->wait - 1 > 0 ? g->wait - 1 : 0;
  }
}

static void drift(void) {   /* ep */
  float f = powf(S->zx, .8f) * .8f;
  for (int i = 0; i < S->ntargets; i++) {
    Target *t = &S->targets[i];
    if (t->dead) continue;
    float x, y;
    pos_of(t->n, &x, &y);
    float c = (8 + y) / 80 * f;
    t->vx = t->left ? c : -c;
    if (t->type == T_FAN) t->vx *= .5f;
    Rect k;
    if (box_of(t->n, &k) && ((k.x < -k.w - 5 && t->vx < 0) || (325 < k.x && t->vx > 0))) kill_target(t);
  }
}

/* ---------------------------------------------------------------- kp: the player */
static void player_control(void) {
  float c = 2 + 2.0f * S->ut / 45;
  S->pvx = in.jx * c;
  S->pvy = 0;
  ent_label(S->player, in.jx == 0 ? "idle" : in.jx > 0 ? "right" : "left", false);
  S->pw = S->pw - 1 > 0 ? S->pw - 1 : 0;
  if (in.held[A_ACTION] && S->pw == 0) {             /* ak.kb: held */
    S->zn = (int)ceilf(30 * (1 - S->ut / 45.0f * .77f));
    S->pw = S->zn;
    float x, y;
    pos_of(S->player, &x, &y);
    SOUND(shoot);
    shoot(x, y, S->next, TEAM_PLAYER);
    S->next = A_NORMAL;
  }
}

/* ---------------------------------------------------------------- Sp, jp: Yoichi */
static void waypoints(void) {   /* Sp */
  if (!S->has_wp || !S->wps) return;
  float x, y;
  pos_of(S->champ, &x, &y);
  float dx = S->wpx - x, dy = S->wpy - y, d = sqrtf(dx * dx + dy * dy);
  if (d > S->wps) { S->cvx = dx / d * S->wps; S->cvy = dy / d * S->wps; }
  else { S->cvx = dx; S->cvy = dy; }
  if (sqrtf(S->cvx * S->cvx + S->cvy * S->cvy) < .01f) S->cvx = S->cvy = 0;
}

static void champion(void) {   /* jp */
  float mx, my;
  pos_of(S->champ, &mx, &my);
  S->cpw = fmaxf(0, S->cpw - 1);
  if (S->cpw <= 0)
    for (int i = 0; i < S->ntargets; i++) {
      Target *t = &S->targets[i];
      if (t->dead) continue;
      float hx, hy;
      pos_of(t->n, &hx, &hy);
      float d = (my - hy) / 13;
      hx += t->vx * d;
      if (fabsf(hx - mx) < 5) {
        shoot(mx, my, A_PIERCE, TEAM_CHAMP);
        NodeId bow = node_child(S->champ, "shootSprite");
        if (bow) node_goto(bow, NULL, 0, true);
        S->czn = 20 / powf(S->zx, 1.5f);
        S->cpw = S->czn;
        break;
      }
    }
  if (S->cvx == 0 && S->cvy == 0) {
    S->has_wp = true;
    if (S->caa <= 0) {
      float ax = roundf(frand_range(40, 280)), ay = 169.2f;
      if (hypotf(ax - mx, ay - my) > 20) {
        S->wpx = ax;
        S->wpy = ay;
        S->wps = 2.5f * sqrtf(S->zx);
        ent_label(S->champ, ax < mx ? "w" : "e", false);
        S->caa = 90 / powf(S->zx, 1.5f);
      }
    } else {
      S->caa--;
      ent_label(S->champ, "shoot", false);
      NodeId bow = node_child(S->champ, "shootSprite");
      if (bow) node_goto(bow, NULL, 0, false);
    }
  }
}

/* ---------------------------------------------------------------- np, fq: moving */
/* np: a collidable that would move into another stops along the axis it
 * reaches first (the other's bounds grown by its own) */
static void collisions(void) {
  float vx = S->pvx, vy = S->pvy;
  if (vx == 0 && vy == 0) return;
  Rect pb;
  bool hp = box_of(S->player, &pb);
  float ax, ay;
  pos_of(S->player, &ax, &ay);
  float hx = ax + vx, hy = ay + vy;
  for (int i = 0; i < S->nwalls; i++) {
    Rect d;
    if (!box_of(S->walls[i], &d)) continue;
    if (hp) {
      d.x += ax - pb.w - pb.x;
      d.y += ay - pb.h - pb.y;
      d.w += pb.w;
      d.h += pb.h;
    }
    if (!contains(d, hx, hy)) continue;
    float n = d.x, e = d.x + d.w, f = d.y, g = d.y + d.h, v = 1, u = 1;
    if (vx < 0) v = (e - ax) / (hx - ax);
    else if (vx > 0) v = (n - ax) / (hx - ax);
    if (vy < 0) u = (g - ay) / (hy - ay);
    else if (vy > 0) u = (f - ay) / (hy - ay);
    if ((v < 0 || v > 1 ? 1 : v) < (u < 0 || u > 1 ? 1 : u)) vx = 0;
    else vy = 0;
  }
  S->pvx = vx;
  S->pvy = vy;
}

static void velocities(void) {   /* fq */
  for (int i = 0; i < S->ntargets; i++)
    if (!S->targets[i].dead) move_by(S->targets[i].n, S->targets[i].vx, 0);
  for (int i = 0; i < S->narrows; i++)
    if (!S->arrows[i].dead) move_by(S->arrows[i].n, 0, S->arrows[i].vy);
  if (S->pvx || S->pvy) move_by(S->player, S->pvx, S->pvy);
  if (S->cvx || S->cvy) move_by(S->champ, S->cvx, S->cvy);
}

/* ---------------------------------------------------------------- ip: the HUD */
static void hud(void) {
  int secs = (S->tick + 29) / 30;
  snprintf(S->s_time, sizeof S->s_time, "%02d", secs);
  snprintf(S->s_score, sizeof S->s_score, "%d", S->score);
  snprintf(S->s_cscore, sizeof S->s_cscore, "%d", S->cscore);
  if (S->t_time) node_set_text(S->t_time, S->s_time);
  if (S->t_time2) node_set_text(S->t_time2, S->s_time);
  if (S->t_score) node_set_text(S->t_score, S->s_score);
  if (S->t_champ) node_set_text(S->t_champ, S->s_cscore);
  if (S->avatar) goto_frame(S->avatar, (int)floorf((node_frames(S->avatar) - 1) * fminf(1, S->ut / 45.0f)), &S->last[0]);
  if (S->cavatar) goto_frame(S->cavatar, (int)floorf((node_frames(S->cavatar) - 1) * fminf(1, S->cut / 30.0f)), &S->last[1]);
  if (S->arrow_inf) node_set_visible(S->arrow_inf, false);
  /* gotoAndStop(13 (1 - Pw / zN)), nothing while zN is 0 (NaN). The white
   * arrow is masked (see draw_arrow_ui): the clip stays on its first frame. */
  if (S->zn > 0) S->arrow_f = (int)floorf(13 * (1 - (float)S->pw / S->zn));
  if (S->czn > 0) S->carrow_f = (int)floorf(13 * (1 - S->cpw / S->czn));
  S->arrow_f = clampi(S->arrow_f, 0, 13);
  S->carrow_f = clampi(S->carrow_f, 0, 13);
}

/* ---------------------------------------------------------------- lp: the clock, the end */
static void clock_end(void) {
  if (S->over && --S->end_t <= 0 && !S->reported) {   /* Yo: 80 ticks later, the results */
    S->reported = true;
    menus_game_over_rated((float)S->score, S->rating);
    return;
  }
  S->tick = S->tick - 1 > 0 ? S->tick - 1 : 0;
  if (S->tick != 0) return;
  int g = S->score - S->cscore;
  S->rating = 0 < g ? 3 : -2000 < g ? 2 : -4000 < g ? 1 : 0;
  if (S->over) return;   /* update(): late arrows still count */
  S->pw = 9999999;
  S->cpw = 9999999;
  S->over = true;
  S->end_t = 80;
  SOUND(end);
  toast_style(msg(0 < g ? "YOU_WIN" : "YOICHI_WINS"), 80, 0x111111, 0x555555);
}

/* ---------------------------------------------------------------- Pp: draw order */
static float sort_key(NodeId c) {
  uint16_t T = nodes[c].T;
  if (T != NONE16 && nodes[c].ent && comp_has(T, C_drawOrderOverride)) return comp_float(T, C_drawOrderOverride, F_drawOrder, 0);
  if (!node_visible(c)) return -1e15f;
  float x, y;
  pos_of(c, &x, &y);
  return y;
}

static void sort_map(void) {
  enum { KIDS = 160 };
  NodeId kids[KIDS];
  float keys[KIDS];
  int n = 0;
  NodeId rest = 0;
  for (NodeId c = nodes[S->map].first; c; c = nodes[c].next) {
    if (n == KIDS) { rest = c; break; }
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
static void start(void) {
  S = scene_state(sizeof(State));
  S->tick = 1799;
  S->zx = 1;
  S->cd = 105;
  S->counting = true;
  for (int i = 0; i < 6; i++) S->last[i] = -1;
  for (int k = 0; k <= 40; k++)
    for (int t = 0; t < 2; t++) snprintf(S->nums[t][k], sizeof S->nums[t][k], "%d", 50 * k);
  /* bp: side ("both" 0, "random" 2), y range, what, and the first one at once */
  S->sp[0] = (Spawner){0, T_NORMAL, 35, 57, 0, 100, true};
  S->sp[1] = (Spawner){0, T_EXPLOSIVE, 45, 60, 0, 100, true};
  S->sp[2] = (Spawner){2, T_FAN, 60, 125, 0, 100, true};

  NodeId root = node_new_sym(S_archery_pT);
  if (!root) return;
  S->root = root;
  node_add(game.root, root);
  node_update(root);
  init_frame0(root, 0);
  ent_register_tree(root);
  for (NodeId c = nodes[root].first; c; c = nodes[c].next) {
    if (has(c, C_map)) S->map = c;
    if (has(c, C_archeryHud)) S->hud = c;
  }
  if (!S->map) return;
  for (NodeId c = nodes[S->map].first; c; c = nodes[c].next) {
    if (has(c, C_archeryPlayer)) S->player = c;
    if (has(c, C_archeryChampion)) S->champ = c;
    if (node_named(c, "champOverlay")) S->overlay = c;
    if (has(c, C_collidable) && !has(c, C_velocity) && S->nwalls < 2) S->walls[S->nwalls++] = c;
  }
  if (!S->player || !S->champ) { S->map = 0; return; }
  /* the sky: Gradient1 (24 x 24: six bands of four rows) stretched 13.367 x 6
   * behind the sea; drawn as six rectangles instead of a scaled bitmap */
  for (NodeId c = nodes[S->map].first; c; c = nodes[c].next)
    if (nodes[c].sym == S_archery_rba) S->back = c;
  if (S->back && (S->sky = materialize_slot(S->back, 0)) && nodes[S->sky].sym == S_archery_nEa)
    node_set_visible(S->sky, false);
  else S->sky = 0;
  if (S->hud) {
    S->t_score = node_find(S->hud, "playerScore");
    S->t_champ = node_find(S->hud, "champScore");
    S->t_time = node_find(S->hud, "time");
    S->t_time2 = node_find(S->hud, "timeShadow");
    S->avatar = node_find(S->hud, "playerAvatar");
    S->cavatar = node_find(S->hud, "champAvatar");
    S->arrow_ui = node_find(S->hud, "playerArrow");
    S->carrow_ui = node_find(S->hud, "champArrow");
    S->arrow_inf = node_find(S->hud, "playerArrowInfinite");
  }
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

#if defined(HOST) || defined(AR_AUTO)
#ifdef HOST
#include <stdlib.h>
#endif
/* tests: ARAUTO=1 (or a build with -DAR_AUTO) plays: it walks under the
 * target that will pass over it soonest and keeps OK held */
static void autopilot(void) {
#ifdef AR_AUTO
  const bool on = true;
#else
  static int on = -1;
  if (on < 0) on = getenv("ARAUTO") != NULL;
#endif
  if (!on) return;
  float px, py, best = 1e9f, gx = -1;
  pos_of(S->player, &px, &py);
  for (int i = 0; i < S->ntargets; i++) {
    Target *t = &S->targets[i];
    if (t->dead) continue;
    float x, y;
    pos_of(t->n, &x, &y);
    float ticks = (py - y) / 10, fx = x + t->vx * ticks;
    if (fx < 5 || fx > 315) continue;
    float cost = fabsf(fx - px) / 3 - ticks * .2f;
    if (cost < best) { best = cost; gx = fx; }
  }
  in.jx = gx < 0 || fabsf(gx - px) < 3 ? 0 : gx > px ? 1 : -1;
  in.held[A_ACTION] = true;
}
#else
static void autopilot(void) {}
#endif

#ifdef HOST
static void debug_log(void) {
  static int on = -1;
  if (on < 0 && (on = getenv("ARDBG") != NULL)) printf("state %u bytes\n", (unsigned)sizeof(State));
  if (!on) return;
  float px, py, cx, cy;
  pos_of(S->player, &px, &py);
  pos_of(S->champ, &cx, &cy);
  printf("T %u tick %d zx %.3f p %.2f %.2f c %.2f %.2f score %d %d ut %d %d targets %d arrows %d nodes %u\n", game.ticks, S->tick, S->zx,
         px, py, cx, cy, S->score, S->cscore, S->ut, S->cut, S->ntargets, S->narrows, node_count());
}
#else
static void debug_log(void) {}
#endif

static void tick(void) {
  if (!S || !S->map) { sys_back_pauses(); return; }
  nodes[S->map].flags |= NF_VISIBLE;
  sys_back_pauses();                                   /* Ep */
  if (menus_active()) return;
  if (!S->uo) {                                        /* Uo */
    S->uo = true;
    sys_tutorial_once();
    if (menus_active()) return;
  }
  countdown();                                         /* Wo */
  bool on = !S->counting;
  if (on) waypoints();                                 /* Sp */
  if (on) sys_ephemeral();                             /* Vp */
  autopilot();
  if (on) player_control();                            /* kp */
  if (!S->over) spawn_targets();                       /* cp */
  if (on) arrows_hit();                                /* fp */
  drift();                                             /* ep */
  hud();                                               /* ip */
  if (on) champion();                                  /* jp */
  if (on) collisions();                                /* np */
  velocities();                                        /* fq */
  if (on) clock_end();                                 /* lp */
  sort_map();                                          /* Pp */
  compact();
  debug_log();
}

/* ---------------------------------------------------------------- drawing */
/* the points that pop up: the player's in green, Yoichi's in red, MISS in white */
static uint16_t popup_color(const char *s) {
  if (s == TXT_BLOCKED || s == TXT_BOMB || s == TXT_PIERCE || (s >= S->nums[0][0] && s < S->nums[1][0])) return rgb565(0x33, 0xff, 0x33);
  if (s >= S->nums[1][0] && s <= S->nums[1][40]) return rgb565(0xff, 0x33, 0x33);
  return rgb565(0xff, 0xff, 0xff);
}

static void draw_hud_prep(void);

static void draw_sky(Mat b) {
  static const uint8_t band[6][3] = {{15, 15, 62}, {44, 44, 118}, {77, 77, 153}, {124, 124, 197}, {174, 174, 231}, {255, 255, 255}};
  Mat m = mat_mul(b, node_local(S->sky));   /* the gradient's 24 x 24 pixels on screen */
  int x0 = (int)floorf(m.tx + .5f), x1 = (int)floorf(m.tx + 24 * m.a + .5f);
  for (int i = 0; i < 6; i++) {
    int y0 = (int)floorf(m.ty + 4 * i * m.d + .5f), y1 = (int)floorf(m.ty + 4 * (i + 1) * m.d + .5f);
    gfx_rect(x0, y0, x1 - x0, y1 - y0, rgb565(band[i][0], band[i][1], band[i][2]), 255);
  }
}

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
      node_text_draw(t, mt, s, popup_color(s), (uint8_t)(al * nodes[t].alpha / 255));
    }
  }
}

/* the map, drawn here so that the popups get their team's colour; the
 * engine's own pass then skips it (draw_over shows it again) */
static void draw_under(void) {
  if (!S || !S->map) return;
  Node *mp = &nodes[S->map];
  mp->flags |= NF_VISIBLE;
  if (!node_visible_chain(S->map)) return;
  Mat m = mat_mul((Mat){1.0f / 3, 0, 0, 1.0f / 3, 0, 0}, node_global(S->map));
  for (NodeId c = mp->first; c; c = nodes[c].next) {
    if (c == S->back && S->sky && node_visible(c)) draw_sky(mat_mul(m, node_local(c)));
    if (nodes[c].sym == S_archery_f5) draw_popup(c, m);
    else node_draw(c, m);
  }
  mp->flags &= (uint8_t)~NF_VISIBLE;
  draw_hud_prep();
}

/* a text node drawn as the engine does, in another colour */
static void draw_text(NodeId t, uint16_t color) {
  if (!t || nodes[t].kind != NK_TEXT || nodes[t].ref == NONE16 || !node_visible_chain(t)) return;
  node_text_draw(t, mat_mul((Mat){1.0f / 3, 0, 0, 1.0f / 3, 0, 0}, node_global(t)), NULL, color, nodes[t].alpha);
}

/* Cba (the arrow meters): the white arrow (13 x 3 at (-6, -6)) under a mask
 * that shows its first `frame` columns */
static void draw_arrow_ui(NodeId n, int f) {
  if (!n) return;
  nodes[n].flags |= NF_VISIBLE;
  if (!node_visible_chain(n)) return;
  Mat m = mat_mul((Mat){1.0f / 3, 0, 0, 1.0f / 3, 0, 0}, node_global(n));
  SymInfo si;
  sym_info(S_archery_bwa, &si);   /* Dba: the dark arrow at (-8, -7) */
  Mat d = m;
  d.tx += -8 * m.a;
  d.ty += -7 * m.d;
  if (si.type == SYM_BITMAP) gfx_sprite((uint16_t)si.v, d, nodes[n].alpha);
  if (f <= 0) return;
  int x = (int)floorf(m.tx - 6 + .5f), y = (int)floorf(m.ty - 6 + .5f);
  uint16_t w = rgb565(255, 255, 255);
  gfx_rect(x + 1, y + 1, f - 1, 1, w, 255);
  for (int r = 0; r < 3; r += 2) {
    gfx_rect(x, y + r, f < 3 ? f : 3, 1, w, 255);
    if (f > 10) gfx_rect(x + 10, y + r, f < 12 ? f - 10 : 2, 1, w, 255);
  }
}

static void draw_over(void) {
  if (!S) return;
  if (S->map) nodes[S->map].flags |= NF_VISIBLE;
  /* the time's shadow has no colour in the library: createjs draws it black */
  if (S->t_time2 && S->t_time) {
    nodes[S->t_time2].flags |= NF_VISIBLE;
    nodes[S->t_time].flags |= NF_VISIBLE;
    draw_text(S->t_time2, 0);
    draw_text(S->t_time, rgb565(255, 255, 255));
  }
  draw_arrow_ui(S->arrow_ui, S->arrow_f);
  draw_arrow_ui(S->carrow_ui, S->carrow_f);
}

/* the engine's pass draws the HUD without the two time texts (drawn above) */
static void draw_hud_prep(void) {
  if (S->arrow_ui) nodes[S->arrow_ui].flags &= (uint8_t)~NF_VISIBLE;
  if (S->carrow_ui) nodes[S->carrow_ui].flags &= (uint8_t)~NF_VISIBLE;
  if (S->t_time2) nodes[S->t_time2].flags &= (uint8_t)~NF_VISIBLE;
  if (S->t_time) nodes[S->t_time].flags &= (uint8_t)~NF_VISIBLE;
}

static void end(void) { S = NULL; }

const SceneDef scene_archery = {"archery", start, tick, end, draw_under, draw_over, NULL};
