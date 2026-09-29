/* Artistic swimming: the doodle's scene Hu (library 81A4DDA62E6C4693B3A00E8A0484897E).
 *
 * A rhythm game: arrows come down four lanes (left, up, down, right) and the
 * player presses that direction as each one reaches its circle. The doodle
 * times everything by its song; the calculator has no sound, so the song's
 * clock is the frame clock: the song starts 3.5 s after the scene, as the
 * countdown says GO, and every tick is 1000/30 ms of it. Each variant is one
 * song with its own chart and scroll speed: "ballad", "rock", and the default
 * ("disco"). Systems, in the doodle's order: Ep (pause), Wo (countdown), Uo
 * (rules), ku (the song's start and end), lu (the clock), mu (new arrows), pu
 * (arrows move, late ones miss), ru (the swimmers bob), uu (keys, the
 * player's moves, hits), tu (the others dance just before each beat), yu (the
 * score), Vp, Up. The arrows have no nodes: they are drawn in their lanes. */
#include <math.h>
#include <stdio.h>
#include "ent.h"

/* the doodle's charts: direction (0 up, 1 right, 2 down, 3 left, 10 the end) and time (ms) */
/* Eu: 91 moves, times shifted by 50 ms */
static const uint16_t eu_t[] = {100, 3680, 4383, 5114, 5846, 6548, 7616, 8017, 8185, 8754, 9468, 9799, 10600, 11685, 12399, 12736, 13299, 13468, 14582, 15180, 15348, 15482, 15650, 15819, 16382, 16747, 17113, 17450, 18251, 18953, 19656, 20364, 21101, 21444, 22181, 23284, 24021, 24358, 24735, 25101, 26256, 26970, 27336, 28050, 28415, 28752, 29129, 29797, 30563, 31231, 31968, 32729, 33065, 33733, 34934, 35683, 35979, 36612, 36780, 37866, 38197, 38580, 38946, 39323, 40078, 40728, 41070, 41442, 41813, 42150, 42551, 43265, 43636, 44344, 45128, 45796, 46533, 46933, 47299, 47607, 47949, 48350, 48721, 49099, 50434, 50805, 52053, 52396, 53441, 53812, 54000};
static const uint8_t eu_k[] = {0, 0, 0, 0, 0, 0, 3, 1, 1, 1, 1, 3, 3, 1, 0, 0, 3, 2, 2, 2, 2, 1, 1, 1, 3, 0, 1, 3, 2, 1, 3, 0, 0, 3, 1, 0, 0, 1, 1, 3, 0, 2, 2, 0, 0, 0, 3, 1, 1, 0, 3, 3, 1, 0, 3, 3, 3, 1, 1, 0, 3, 2, 2, 2, 2, 3, 2, 1, 1, 2, 2, 3, 2, 2, 3, 2, 2, 1, 1, 3, 3, 0, 1, 3, 2, 2, 2, 2, 2, 2, 10};
/* Fu: 159 moves, times shifted by 120 ms */
static const uint16_t fu_t[] = {100, 2757, 2926, 3094, 3262, 3396, 3558, 3727, 3895, 4058, 4226, 4394, 4557, 4690, 4894, 5062, 5224, 5393, 5590, 5759, 5892, 6060, 6223, 6391, 6560, 8092, 8226, 8394, 8557, 8725, 8893, 9062, 9195, 9398, 9561, 9700, 9868, 10106, 10240, 10408, 10681, 11012, 11146, 12655, 13038, 13380, 13746, 14083, 14216, 14524, 14861, 14994, 15464, 16028, 16393, 16562, 16997, 17984, 18361, 18663, 19029, 19360, 19528, 19859, 20660, 21037, 21415, 21745, 21914, 23342, 23713, 24015, 24381, 24718, 24886, 25165, 25530, 25664, 26070, 26680, 27011, 27179, 27678, 28648, 29013, 29379, 29710, 30012, 30244, 30679, 31376, 32078, 32247, 32583, 32885, 33228, 34029, 34331, 34696, 35062, 35230, 35730, 35927, 36293, 36426, 36595, 36763, 36897, 37065, 37227, 37431, 38063, 38528, 39329, 39695, 40031, 40635, 40844, 41047, 41244, 42028, 42742, 43056, 43224, 43595, 43926, 44681, 45047, 45383, 45755, 45923, 46695, 47026, 47398, 47694, 48060, 48396, 48570, 48739, 48878, 49244, 49580, 49778, 49946, 50242, 50445, 50747, 51078, 51415, 51746, 51914, 52680, 53377, 53743, 54079, 54248, 54579, 55380, 56000};
static const uint8_t fu_k[] = {2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 3, 3, 3, 3, 0, 0, 0, 0, 0, 0, 1, 0, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 1, 1, 1, 2, 1, 0, 3, 0, 1, 0, 3, 3, 1, 0, 3, 1, 0, 0, 0, 0, 1, 0, 3, 0, 1, 0, 3, 0, 1, 0, 0, 0, 1, 0, 3, 0, 1, 0, 3, 0, 1, 0, 3, 0, 0, 0, 1, 0, 3, 0, 1, 2, 3, 2, 2, 2, 3, 2, 1, 2, 3, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 3, 0, 1, 2, 3, 0, 1, 0, 0, 3, 0, 1, 0, 3, 0, 1, 0, 0, 3, 0, 1, 2, 2, 3, 2, 1, 0, 3, 0, 1, 0, 3, 0, 1, 1, 1, 2, 3, 0, 3, 3, 1, 1, 1, 0, 10};
/* Gu: 140 moves, times shifted by -3900 ms */
static const uint16_t gu_t[] = {3951, 5320, 6735, 7470, 8156, 9580, 11003, 12749, 13308, 13683, 14457, 14883, 15248, 15579, 15947, 16216, 16447, 16846, 17027, 17371, 17606, 17856, 18053, 18384, 18747, 18987, 19272, 19824, 20206, 20504, 20871, 21249, 21572, 21840, 22085, 22451, 22638, 22980, 23246, 23484, 23716, 24057, 24422, 24670, 24922, 26850, 27216, 27552, 27757, 28103, 28276, 28484, 28821, 29010, 29350, 30072, 30423, 30760, 32140, 32871, 33384, 33738, 33912, 34305, 34623, 35004, 35350, 35707, 36058, 36224, 37800, 38155, 38517, 38704, 39044, 39903, 40274, 40614, 40985, 41172, 41690, 41885, 42403, 42780, 43121, 43472, 43802, 44171, 44350, 44647, 45569, 45958, 46282, 46454, 46646, 46839, 47017, 47204, 47374, 47527, 47709, 48412, 49115, 49453, 49827, 49999, 50321, 51221, 51554, 51938, 52287, 52475, 52818, 52990, 53171, 53546, 53725, 53905, 54267, 54442, 54624, 54808, 55147, 55503, 55871, 56204, 56558, 56909, 57246, 57590, 57956, 58323, 58679, 59028, 59701, 60390, 60971, 61508, 63259, 64259};
static const uint8_t gu_k[] = {2, 2, 3, 2, 1, 0, 3, 0, 0, 0, 0, 0, 3, 3, 1, 1, 1, 2, 2, 0, 0, 0, 3, 3, 0, 0, 0, 1, 2, 2, 3, 3, 1, 1, 1, 0, 0, 3, 3, 3, 2, 2, 0, 0, 0, 1, 1, 2, 2, 0, 0, 0, 0, 0, 1, 3, 3, 1, 1, 0, 3, 1, 1, 3, 3, 1, 1, 1, 1, 1, 3, 0, 1, 0, 0, 3, 1, 0, 0, 0, 0, 0, 1, 3, 1, 0, 0, 3, 0, 0, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 3, 0, 1, 0, 0, 0, 3, 3, 3, 1, 1, 1, 0, 0, 0, 3, 3, 3, 0, 0, 1, 1, 0, 0, 3, 3, 2, 1, 0, 3, 2, 1, 0, 0, 0, 0, 10};

typedef struct { const uint16_t *t; const uint8_t *k; int n; int shift; float speed; } Chart;

enum { ST_WAIT, ST_MISSED, ST_HIT };   /* danceMoveArrow.state */
enum { L_DEFAULT, L_SUCCESS, L_MISS };
#define ARROW_MAX 48

typedef struct {
  int32_t time;
  float y;
  int16_t anim;           /* frames into its sprite's current animation */
  uint8_t type, state, look;
  bool opa;               /* the others already danced to it */
} Arrow;

typedef struct {
  NodeId root, map, track[4], target[4], player, champion, oldman, turtle, red, score_text, combo, combo_text;
  NodeId chars[4];        /* the swimmers, left to right */
  float base_x[4], base_y[4];
  int nchars;
  Chart chart;
  /* the arrows' drawing, by direction and look: the animation and its offset */
  uint16_t look_sym[4][3];
  Mat look_m[4][3];
  int16_t look_end[4][3];
  Arrow arrows[ARROW_MAX];
  int narrows;
  /* clockState, playbackState, swimState */
  bool clock_started;
  uint32_t ticks, t0;
  int frame;              /* currentFrame */
  float RB;               /* the song's time (ms) */
  int rY;
  int wp, ut, sqa, GQ;
  float xL[4];            /* when each direction was last pressed (song time), NAN: never */
  int V6;                 /* the player's move, -1: none */
  Countdown cd;
  /* the doodle's centred message for Miss / Good / Perfect */
  int judge, judge_t;     /* -1: none */
  bool ended;
  int end_t, rating;
  char score_buf[16], combo_buf[16];
} State;

static State *S;

static const char *const dir_label[4] = {"up", "right", "down", "left"};
static const uint8_t dir_input[4] = {A_UP, A_RIGHT, A_DOWN, A_LEFT};
static const uint8_t iu[4] = {0, 2, 1, 3};

static bool has(NodeId n, int c) { return n && nodes[n].T != NONE16 && comp_has(nodes[n].T, c); }

/* ---------------------------------------------------------------- the arrows' looks */
/* a clip's timeline child at a frame (the first one, or the first that is a
 * sprite): its symbol and its matrix in the clip */
static const uint8_t *varint(const uint8_t *p, unsigned *v) {
  unsigned r = 0, sh = 0;
  for (int i = 0; i < 5; i++) {
    uint8_t c = *p++;
    r |= (unsigned)(c & 0x7F) << sh;
    if (!(c & 0x80)) break;
    sh += 7;
  }
  *v = r;
  return p;
}

typedef struct { unsigned start; uint8_t flags, kind; uint16_t ref, mat; float x, y; int16_t rx4, ry4; } TKey;

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
  if (k->flags & K_ALPHA) p++;
  if (k->flags & K_CLIP) p += 4;
  if (k->flags & K_NAME) p += 2;
  return p;
}

static bool child_key(uint16_t sym, unsigned frame, bool sprite, uint16_t *out, Mat *m) {
  Clip c;
  if (!clip_get(sym, &c)) return false;
  const uint8_t *p = c.slots;
  for (unsigned s = 0; s < c.nslots; s++) {
    unsigned nk;
    p = varint(p, &nk);
    TKey cur = {0}, k;
    int idx = -1;
    for (unsigned j = 0; j < nk; j++) {
      p = tkey(p, &k);
      if (k.start <= frame) { cur = k; idx = (int)j; }
    }
    if (idx < 0 || (cur.flags & K_ABSENT) || cur.kind != CK_SYM) continue;
    Clip cc;
    if (!clip_get(cur.ref, &cc) || (sprite && !(cc.T != NONE16 && comp_has(cc.T, C_sprite)))) continue;
    float a = 1, b = 0, d0 = 0, d = 1;
    if (cur.mat) { const float *mm = mat(cur.mat); a = mm[0]; b = mm[1]; d0 = mm[2]; d = mm[3]; }
    *m = (Mat){a, b, d0, d, cur.x - (cur.rx4 * .25f * a + cur.ry4 * .25f * d0), cur.y - (cur.rx4 * .25f * b + cur.ry4 * .25f * d)};
    *out = cur.ref;
    return true;
  }
  return false;
}

/* the frame an animation stops at or reports that it is over */
static int16_t anim_end(uint16_t sym, int look) {
  Clip c;
  if (!clip_get(sym, &c)) return 0;
  for (int i = 0; i < c.nacts; i++) {
    const uint8_t *a = c.acts + 5 * i;
    if ((look == L_SUCCESS && a[2] == ACT_DISPATCH_PARENT) || (look == L_MISS && a[2] == ACT_STOP)) return (int16_t)rd16(a);
  }
  return (int16_t)(c.nframes - 1);
}

static void looks_setup(void) {
  Clip ya;
  if (!clip_get(S_swim_ysa, &ya)) return;
  static const char *const looks[3] = {"default", "success", "miss"};
  for (int d = 0; d < 4; d++) {
    int f = clip_label(&ya, dir_label[d]);
    uint16_t s1, s2;
    Mat m1, m2;
    Clip sc;
    if (f < 0 || !child_key(S_swim_ysa, (unsigned)f, true, &s1, &m1) || !clip_get(s1, &sc)) continue;
    for (int l = 0; l < 3; l++) {
      int g = clip_label(&sc, looks[l]);
      if (g < 0 || !child_key(s1, (unsigned)g, false, &s2, &m2)) continue;
      S->look_sym[d][l] = s2;
      S->look_m[d][l] = mat_mul(m1, m2);
      S->look_end[d][l] = anim_end(s2, l);
    }
  }
}

static void arrow_look(Arrow *a, int look) {
  a->look = (uint8_t)look;
  a->anim = 0;
}

/* ---------------------------------------------------------------- the map, drawn by code for the arrows */
static void map_hook(NodeId n, Mat m, uint8_t al) {
  for (NodeId c = nodes[n].first; c; c = nodes[c].next) {
    node_draw_in(c, m, al);
    int d = -1;
    for (int k = 0; k < 4; k++) if (S->track[k] == c) d = k;
    if (d < 0 || !node_visible(c)) continue;
    Mat mt = mat_mul(m, node_local(c));
    for (int i = 0; i < S->narrows; i++) {
      Arrow *a = &S->arrows[i];
      if (a->type != d || !S->look_sym[d][a->look]) continue;
      Mat ma = mt;
      ma.tx += mt.c * a->y;
      ma.ty += mt.d * a->y;
      if (ma.ty < -40 || ma.ty > VIEW_H + 40) continue;
      ma = mat_mul(ma, S->look_m[d][a->look]);
      node_draw_sym(S->look_sym[d][a->look], (unsigned)a->anim, ma, al);
    }
  }
}

/* ---------------------------------------------------------------- the swimmers */
static void dance(NodeId c, const char *label) {
  ent_label(c, label, false);
  for (NodeId s = nodes[c].first; s; s = nodes[s].next)
    if ((nodes[s].flags & NF_ONSTAGE) && has(s, C_sprite)) { node_goto(s, NULL, 0, true); break; }
}

static void on_move_finished(NodeId c, uint16_t ev, void *ctx) {
  (void)ev; (void)ctx;
  dance(c, "idle");
  if (c == S->player) S->V6 = -1;
}

static void on_beat_finished(NodeId c, uint16_t ev, void *ctx) { (void)ev; (void)ctx; node_goto(c, NULL, 0, false); }

/* ---------------------------------------------------------------- scoring (qu, xu, wu) */
static void judge(Arrow *a, int points) {
  bool hit = points == 50 || points == 100;
  if (hit) {
    a->state = ST_HIT;
    arrow_look(a, L_SUCCESS);
  } else {
    a->state = ST_MISSED;
    arrow_look(a, L_MISS);
  }
  /* wu: the message (it replaces the one showing) */
  S->judge = points == 100 ? 2 : points == 50 ? 1 : 0;
  S->judge_t = 0;
  toast("");
  /* xu */
  int mult = 1;
  if (hit && S->ut > 10 && S->ut <= 20) mult = 3;
  else if (hit && S->ut > 20) mult = 5;
  S->wp = S->wp + points * mult;
  if (S->wp < 0) S->wp = 0;
  if (hit) {
    S->sqa++;
    S->ut++;
    S->GQ = S->GQ + 1 > 5 ? 5 : S->GQ + 1;
  } else {
    S->ut = 0;
    S->GQ = S->GQ - 1 < -10 ? -10 : S->GQ - 1;
  }
}

/* ---------------------------------------------------------------- start */
static void start(void) {
  S = scene_state(sizeof(State));
  if (!strcmp(game.variant, "rock")) S->chart = (Chart){fu_t, fu_k, (int)sizeof fu_t / 2, 120, 130};
  else if (!strcmp(game.variant, "ballad")) S->chart = (Chart){eu_t, eu_k, (int)sizeof eu_t / 2, 50, 100};
  else S->chart = (Chart){gu_t, gu_k, (int)sizeof gu_t / 2, -3900, 120};
  NodeId root = node_new_sym(S_swim_Gcb);
  if (!root) return;
  node_add(game.root, root);
  S->root = root;
  ent_register_tree(root);
  S->map = ent_first(C_map);
  if (!S->map) return;
  static const char *const names[4] = {"up", "right", "down", "left"};
  for (int d = 0; d < 4; d++) {
    S->track[d] = node_child(S->map, names[d]);
    S->target[d] = S->track[d] ? node_child(S->track[d], "target") : 0;
    /* createjs.ButtonHelper: out, over, down = frames 0, 1, 2 */
    if (S->target[d]) node_goto(S->target[d], NULL, 0, false);
    S->xL[d] = NAN;
  }
  S->player = ent_first2(C_character, C_player);
  S->champion = ent_first2(C_character, C_champion);
  S->oldman = ent_first2(C_character, C_oldMan);
  S->turtle = ent_first2(C_character, C_turtle);
  S->red = ent_first(C_redBg);
  NodeId score = node_find(root, "score");
  if (score) {
    S->score_text = node_child(score, "scoreText");
    S->combo = node_child(score, "combo");
    S->combo_text = S->combo ? node_child(S->combo, "comboText") : 0;
  }
  /* zu: the swimmers, their places, their moves' ends (ru sorts them by x) */
  for (int i = 0; i < ent_count(); i++) {
    NodeId e = ent_at(i);
    if (!has(e, C_character) || S->nchars >= 4) continue;
    float x, y;
    ent_pos(e, &x, &y);
    int j = S->nchars++;
    while (j > 0 && S->base_x[j - 1] > x) {
      S->chars[j] = S->chars[j - 1]; S->base_x[j] = S->base_x[j - 1]; S->base_y[j] = S->base_y[j - 1];
      j--;
    }
    S->chars[j] = e; S->base_x[j] = x; S->base_y[j] = y;
    node_on(e, "move_finished", on_move_finished, NULL);
  }
  /* Au: beat targets wait on their first frame */
  for (int i = 0; i < ent_count(); i++) {
    NodeId e = ent_at(i);
    if (!has(e, C_beatTarget)) continue;
    node_goto(e, NULL, 0, false);
    node_on(e, "beat_finished", on_beat_finished, NULL);
  }
  looks_setup();
  S->V6 = -1;
  S->judge = -1;
  S->frame = -1;
  sys_countdown_start(&S->cd);
  node_draw_hook_id = S->map;
  node_draw_hook = map_hook;
}

static void end(void) {
  node_draw_hook = NULL;
  node_draw_hook_id = 0;
}

/* ---------------------------------------------------------------- systems */
static int chart_time(int i) { return (int)S->chart.t[i] + S->chart.shift; }

/* ku: the song starts with the scene (3.5 s of countdown), and its end marker ends the game */
static void sys_song(void) {
  if (S->ended) {
    if (--S->end_t == 0) menus_game_over_rated((float)S->wp, S->rating);
  }
  if (!S->clock_started) {
    S->clock_started = true;
    S->t0 = S->ticks;
  }
  const Chart *c = &S->chart;
  if (S->rY < c->n && c->k[S->rY] == 10 && S->RB >= chart_time(S->rY) && !S->ended) {
    S->rating = (int)floorf((float)S->sqa / (float)(c->n - 1) * 3 + .5f);
    S->ended = true;
    S->end_t = 80;
    SOUND();
    S->judge = -1;
    toast(msg("NICE_MOVES"));
  }
}

/* lu: the song's clock */
static void sys_clock(void) {
  S->frame++;
  S->RB = (float)(S->ticks - S->t0) * (1000.0f / FPS) - 3500;
}

/* mu: the arrows of the next 2 s */
static void sys_spawn(void) {
  const Chart *c = &S->chart;
  while (S->rY < c->n) {
    int t = chart_time(S->rY);
    if (t > S->RB + 2000 || c->k[S->rY] == 10) break;
    if (S->narrows < ARROW_MAX && c->k[S->rY] < 4) {
      Arrow *a = &S->arrows[S->narrows++];
      memset(a, 0, sizeof *a);
      a->time = t;
      a->type = c->k[S->rY];
      a->y = -1000;
      arrow_look(a, L_DEFAULT);
    }
    S->rY++;
  }
}

/* pu: arrows come down; one 130 ms late is missed */
static void sys_arrows(void) {
  for (int i = 0; i < S->narrows; i++) {
    Arrow *a = &S->arrows[i];
    if (a->state == ST_HIT) continue;
    float n = S->RB - (float)a->time;
    a->y = n / 1000 * S->chart.speed;
    if (n > 130 && a->state == ST_WAIT) judge(a, -5);
  }
}

/* ru: the swimmers bob (out of step when the dancing goes badly), the red light */
static void sys_bob(void) {
  static const int8_t k[4] = {1, 0, -2, 3};
  for (int i = 0; i < S->nchars; i++) {
    NodeId c = S->chars[i];
    float h = 0;
    if (S->GQ <= 0 && c != S->player) h = fabsf((float)S->GQ) / 10 * k[i];
    float b = 20 * sinf(3.14159265f / 60 * S->frame + h);
    ent_set_pos(c, S->base_x[i], S->base_y[i] + (b > 0 ? floorf(b) : ceilf(b)));
  }
  if (S->red) nodes[S->red].alpha = S->GQ < -5 ? (uint8_t)((sinf(3.14159265f / 60 * S->frame) + 1) / 2 * 255 + .5f) : 0;
}

/* uu: the circles, the player's moves, hits */
static void sys_keys(void) {
  for (int i = 0; i < 4; i++) {
    int d = iu[i], e = dir_input[d];
    if (in.pressed[e]) {
      if (S->target[d]) node_goto(S->target[d], NULL, 2, false);
      S->xL[d] = S->RB;
    } else if (!in.held[e] && S->target[d]) node_goto(S->target[d], NULL, 0, false);
  }
  int n = -1;
  for (int i = 0; i < 4; i++)
    if (fabsf(S->RB - S->xL[iu[i]]) < 50) { n = iu[i]; break; }
  if (n >= 0 && n != S->V6 && S->player) {
    S->V6 = n;
    dance(S->player, dir_label[n]);
  }
  /* the arrows by y (vu), the lowest on screen last */
  int order[ARROW_MAX];
  for (int i = 0; i < S->narrows; i++) {
    int j = i;
    while (j > 0 && S->arrows[order[j - 1]].y > S->arrows[i].y) { order[j] = order[j - 1]; j--; }
    order[j] = i;
  }
  for (int i = 0; i < S->narrows; i++) {
    Arrow *a = &S->arrows[order[i]];
    if (a->state != ST_WAIT) continue;
    float d = fabsf((float)a->time - S->xL[a->type]);
    if (d < 70) { judge(a, 100); S->xL[a->type] = 0; }
    else if (d < 130) { judge(a, 50); S->xL[a->type] = 0; }
  }
}

/* tu: the others dance 150 ms before the next arrow */
static void sys_others(void) {
  Arrow *next = NULL;
  for (int i = 0; i < S->narrows; i++) {
    Arrow *a = &S->arrows[i];
    if (!a->opa && (float)a->time > S->RB && (!next || a->time < next->time)) next = a;
  }
  if (!next || (float)next->time - S->RB >= 150) return;
  next->opa = true;
  NodeId who[3] = {S->champion, S->oldman, S->turtle};
  for (int i = 0; i < 3; i++)
    if (who[i]) dance(who[i], dir_label[next->type]);
}

/* yu: the score and the combo */
static void sys_hud(void) {
  snprintf(S->score_buf, sizeof S->score_buf, "%d", S->wp);
  if (S->score_text) node_set_text(S->score_text, S->score_buf);
  if (!S->combo) return;
  node_set_visible(S->combo, S->ut >= 3);
  if (S->ut >= 3 && S->combo_text) {
    snprintf(S->combo_buf, sizeof S->combo_buf, "%d", S->ut);
    node_set_text(S->combo_text, S->combo_buf);
  }
}

/* the arrows' animations; a hit one goes when its animation is over, a missed
 * one once far below the stage (Up) */
static void animate(void) {
  float track_y = S->track[0] ? node_global(S->track[0]).ty : 0;
  for (int i = 0; i < S->narrows; i++) {
    Arrow *a = &S->arrows[i];
    int16_t e = S->look_end[a->type][a->look];
    bool gone = false;
    if (a->look == L_SUCCESS && a->anim >= e) gone = true;
    else if (a->look == L_MISS && a->anim >= e) a->anim = e;
    else a->anim++;
    if (track_y + 3 * a->y >= 540 + 1440) gone = true;
    if (gone) {
      S->arrows[i] = S->arrows[--S->narrows];
      i--;
    }
  }
}

static void tick(void) {
  sys_back_pauses();
  if (menus_active() || !S || !S->map) return;
  sys_countdown(&S->cd);
  sys_tutorial_once();
  if (menus_active()) return;
  S->ticks++;
  animate();
  if (S->judge >= 0) S->judge_t++;
  sys_song();
  sys_clock();
  sys_spawn();
  sys_arrows();
  sys_bob();
  sys_keys();
  sys_others();
  sys_hud();
  sys_ephemeral();
}

/* ---------------------------------------------------------------- the judgement (To, "scale") */
static void draw_over(void) {
  if (!S || S->judge < 0) return;
  static const char *const keys[3] = {"MISS", "GOOD", "PERFECT"};
  static const uint8_t col[3][6] = {{0xf0, 0x1d, 0x1d, 0x6e, 0x05, 0x05}, {0xff, 0xd5, 0x00, 0x96, 0x50, 0x00}, {0x0b, 0xd6, 0x0b, 0x05, 0x5e, 0x05}};
  static const float scale0[3] = {1.2f, 1.4f, 1.5f};
  float ms = S->judge_t * (1000.0f / FPS);
  /* 500 ms from the big size to 1 (quintOut), 200 ms still, 300 ms to nothing */
  float t = ms < 500 ? ms / 500 : 1, e = 1 - powf(1 - t, 5);
  float sc = scale0[S->judge] + (1 - scale0[S->judge]) * e;
  float alpha = ms <= 700 ? 1 : ms >= 1000 ? 0 : 1 - (ms - 700) / 300;
  if (alpha <= 0) return;
  const char *s = msg(keys[S->judge]);
  int k3 = (int)(40 * sc / 10 + .5f);   /* 40 px on the stage, scaled: in thirds of the font's 10 px */
  Mat m = MAT_ID;
  m.tx = VIEW_W / 2;
  m.ty = floorf((540 * .28f - 20 * sc) / 3 + .5f) - k3 / 3;
  uint8_t a = (uint8_t)(alpha * 255);
  uint16_t fg = rgb565(col[S->judge][0], col[S->judge][1], col[S->judge][2]);
  uint16_t ol = rgb565(col[S->judge][3], col[S->judge][4], col[S->judge][5]);
  static const int8_t off[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
  for (int i = 0; i < 8; i++) {
    Mat o = m;
    o.tx += off[i][0];
    o.ty += off[i][1];
    gfx_text_k3(s, o, ol, 1, 0, 0, a, k3);
  }
  gfx_text_k3(s, m, fg, 1, 0, 0, a, k3);
}

const SceneDef scene_swim = {"swim", start, tick, end, NULL, draw_over, NULL};
