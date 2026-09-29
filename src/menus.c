/* The menus overlay: the doodle's menus library (pause, results, rules,
 * the map with the seven sports, settings...), one frame label per menu,
 * driven with the arrows and OK like the doodle's keyboard navigation. */
#include <math.h>
#include <stdio.h>
#ifdef HOST
#include <stdlib.h>
#endif
#include "ent.h"
#include "font.h"

static NodeId root;                 /* the menus clip (S_menus_Obb) */
static char stack[6][24];
static int depth;
static NodeId focus;
static int held_ticks, last_dir = -1;
static bool results_counting;
static float count_from, count_to, count_best_from, count_best_to;
static int count_t;
static bool best_improved;
static char score_buf[24], best_buf[24], stat_bufs[7][16];

bool menus_active(void) { return depth > 0; }

void menus_init(void) {
  root = 0;
  depth = 0;
}

/* the menus clip at one menu's frame (only that menu's nodes exist) */
static void show_label(const char *label) {
  if (root) node_free(root);
  Clip c;
  clip_get(S_menus_Obb, &c);
  root = node_new_sym_frame(S_menus_Obb, clip_label(&c, label));
  node_update(root);
  ent_register_tree(root);
}

static NodeId current_menu(void) {
  if (!root) return 0;
  for (NodeId c = nodes[root].first; c; c = nodes[c].next)
    if ((nodes[c].flags & NF_ONSTAGE) && nodes[c].T != NONE16 && comp_has(nodes[c].T, C_menu)) return c;
  return 0;
}

static const char *menu_id(NodeId m) {
  uint16_t id = m ? comp_str(nodes[m].T, C_menu, F_id) : NONE16;
  return id == NONE16 ? "" : str(id);
}

/* a button of the menu by its event id (xi) */
static NodeId button(NodeId m, const char *ev) {
  for (NodeId c = nodes[m].first; c; c = nodes[c].next) {
    uint16_t e = nodes[c].T != NONE16 ? comp_str(nodes[c].T, C_button, F_eventId) : NONE16;
    if (e != NONE16 && !strcmp(str(e), ev)) return c;
  }
  return 0;
}

/* every text of that name under parent (a button has one per pose: idle, focus...) */
static void set_all(NodeId n, const char *name, const char *s) {
  for (NodeId c = nodes[n].first; c; c = nodes[c].next) {
    if (nodes[c].kind == NK_TEXT && node_named(c, name)) node_set_text(c, s);
    else if (nodes[c].kind == NK_CLIP && !(nodes[c].T != NONE16 && comp_has(nodes[c].T, C_button) && n != c)) set_all(c, name, s);
  }
}

static void set_text(NodeId parent, const char *name, const char *s) {
  if (!parent) return;
  if (nodes[parent].T != NONE16 && comp_has(nodes[parent].T, C_button)) { set_all(parent, name, s); return; }
  NodeId t = node_find(parent, name);
  if (t) node_set_text(t, s);
}

/* a button's own label: button.label.text or button.label.label.text */
static void set_label(NodeId b, const char *key) {
  if (!b) return;
  NodeId l = node_child(b, "label");
  if (!l) return;
  if (nodes[l].kind == NK_TEXT) node_set_text(l, msg(key));
  else set_text(l, "label", msg(key));
}

static void hide_button(NodeId m, const char *ev) {
  NodeId b = button(m, ev);
  if (b) node_set_visible(b, false);
}

static uint16_t team_color(const char *team);
static const char *const sports[7] = {"archery", "climbing", "marathon", "pingpong", "rugby", "skate", "swim"};

/* the rules of the current sport (lo) */
static const char *tutorial_of(const char *name) {
  static char l[32];
  if (!game_is_sport(name)) return NULL;
  snprintf(l, sizeof l, "tut%c%sDesktop", name[0] - 32, name + 1);
  return l;
}

static void setup(NodeId m) {
  const char *id = menu_id(m);
  focus = 0;
  if (!strcmp(id, "pause")) {
    set_text(m, "title", msg("PAUSED"));
    set_label(button(m, "resume"), "CONTINUE");
    set_label(button(m, "how"), "HOW_TO_PLAY");
    set_label(button(m, "restart"), "RESTART");
    set_label(button(m, "quit"), "QUIT");
  } else if (!strncmp(id, "tut", 3)) {
    static const char *const keys[7][4] = {
      {"ARCHERY", "TUT_ARCHERY", "TUT_ARCHERY_DESKTOP_MOVE", "TUT_ARCHERY_DESKTOP_ACTION"},
      {"CLIMBING", "TUT_CLIMBING", "TUT_DIR_TO_MOVE", "TUT_CLIMBING_DESKTOP_ACTION"},
      {"MARATHON", "TUT_MARATHON", "TUT_DIR_TO_MOVE", "TUT_MARATHON_DESKTOP_ACTION"},
      {"PINGPONG", "TUT_PINGPONG", "TUT_DIR_TO_MOVE", "TUT_PINGPONG_DESKTOP_ACTION"},
      {"RUGBY", "TUT_RUGBY", "TUT_DIR_TO_MOVE", "TUT_RUGBY_DESKTOP_ACTION"},
      {"SKATE", "TUT_SKATE", "TUT_SKATE_DESKTOP_MOVE", "TUT_SKATE_DESKTOP_ACTION"},
      {"SWIM", "TUT_SWIM", "TUT_SWIM_DESKTOP_MOVE", "TUT_SWIM_ACTION"}};
    for (int i = 0; i < 7; i++) {
      char l[32];
      snprintf(l, sizeof l, "tut%c%sDesktop", sports[i][0] - 32, sports[i] + 1);
      if (strcmp(l, id)) continue;
      set_text(m, "title", msg(keys[i][0]));
      set_text(m, "rulesLabel", msg("RULES"));
      set_text(m, "rulesContent", msg(keys[i][1]));
      set_text(m, "control1", msg(keys[i][2]));
      set_text(m, "control2", msg(keys[i][3]));
    }
    set_label(button(m, "start"), "OK");
  } else if (!strcmp(id, "results")) {
    set_text(m, "title", msg("RESULTS"));
    set_text(m, "highScoreLabel", msg("HIGH_SCORE"));
    hide_button(m, "share");
  } else if (!strcmp(id, "skip")) {
    set_text(m, "label", msg("SKIP_TUTORIAL"));
    set_label(button(m, "no"), "NO");
    set_label(button(m, "yes"), "YES");
    hide_button(m, "share");
    NodeId b;
    if ((b = button(m, "controls"))) set_text(b, "label", msg("CONTROLS"));
    if ((b = button(m, "leader"))) set_text(b, "label", msg("LEADERBOARD"));
    if ((b = button(m, "settings"))) set_text(b, "label", msg("SETTINGS"));
  } else if (!strcmp(id, "stats")) {
    set_text(m, "label", msg("CHAMPION_ISLAND"));
    hide_button(m, "share");
    NodeId b;
    if ((b = button(m, "controls"))) set_text(b, "label", msg("CONTROLS"));
    if ((b = button(m, "leader"))) set_text(b, "label", msg("LEADERBOARD"));
    if ((b = button(m, "settings"))) set_text(b, "label", msg("SETTINGS"));
    static const char *const names[7] = {"ARCHERY", "CLIMBING", "MARATHON", "PINGPONG", "RUGBY", "SKATE", "SWIM"};
    for (int i = 0; i < 7; i++) {
      b = button(m, sports[i]);
      if (b) set_text(b, "label", msg(names[i]));
    }
  } else if (!strcmp(id, "settings")) {
    set_text(m, "title", msg("SETTINGS"));
    set_label(button(m, "back"), "CLOSE");
    set_label(button(m, "newGame"), "NEW_GAME");
    /* the calculator has no speaker and one font */
    hide_button(m, "soundOn");
    hide_button(m, "soundOff");
    hide_button(m, "textRetro");
    hide_button(m, "textModern");
  } else if (!strcmp(id, "newgame")) {
    set_text(m, "title", msg("NEW_GAME"));
    set_text(m, "prompt", msg("NEW_GAME_PROMPT"));
    set_label(button(m, "no"), "NO");
    set_label(button(m, "yes"), "YES");
  } else if (!strcmp(id, "leaderboard")) {
    set_text(m, "title", msg("LEADERBOARD"));
    set_label(button(m, "back"), "CLOSE");
    /* lr: the teams' scores come from the doodle's server; on the calculator
     * there is only the player's own team and the stars they won for it */
    NodeId l = node_find(m, "loading");
    if (l) node_set_visible(l, false);
    for (int i = 1; i < 4; i++) {
      char k[16];
      snprintf(k, sizeof k, "teamName%d", i);
      if ((l = node_find(m, k))) node_set_visible(l, false);
      snprintf(k, sizeof k, "teamScore%d", i);
      if ((l = node_find(m, k))) node_set_visible(l, false);
    }
    static char team[16], score[12];
    const char *t = store_str("PLAYER_TEAM");
    snprintf(team, sizeof team, "%s", t && t[0] ? t : "NO_TEAM");
    for (char *c = team; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    snprintf(score, sizeof score, "%d", (int)store_num("SUBMITTED_SCORES", 0));
    if ((l = node_find(m, "teamName0"))) {
      node_set_text(l, msg(team));
      node_set_text_color(l, team_color(team));
    }
    if ((l = node_find(m, "teamScore0"))) node_set_text(l, score);
  } else if (!strcmp(id, "controls")) {
    set_label(button(m, "back"), "BACK");
  }
}

bool menus_open(const char *label) {
  Clip c;
  clip_get(S_menus_Obb, &c);
  if (clip_label(&c, label) < 0 || depth >= 6) return false;
  snprintf(stack[depth++], sizeof stack[0], "%s", label);
  show_label(label);
  setup(current_menu());
  return true;
}

static void close_all(void) {
  input_latch();
  depth = 0;
  if (root) node_free(root);
  root = 0;
}

void menus_close(void) {
  if (!depth) return;
  input_latch();
  depth--;
  if (depth) {
    show_label(stack[depth - 1]);
    setup(current_menu());
  } else close_all();
}

/* ---------------------------------------------------------------- events */
static void on_event(NodeId m, const char *ev) {
  const char *id = menu_id(m);
#ifdef HOST
  if (getenv("CI_LOG")) { extern uint32_t host_time; fprintf(stderr, "t%u menu %s %s\n", host_time / 33, id, ev); }
#endif
  if (!strcmp(id, "pause")) {
    if (!strcmp(ev, "resume")) menus_close();
    else if (!strcmp(ev, "how")) { const char *t = tutorial_of(game.name); if (t) menus_open(t); }
    else if (!strcmp(ev, "restart")) { game_replay(); close_all(); }
    else if (!strcmp(ev, "quit")) { game_go("overworld"); close_all(); }
  } else if (!strncmp(id, "tut", 3)) {
    if (!strcmp(ev, "start")) menus_close();
  } else if (!strcmp(id, "results")) {
    if (results_counting) return;
    if (!strcmp(ev, "overworld")) { game_go("overworld"); close_all(); }
    else if (!strcmp(ev, "replay")) { game_replay(); close_all(); }
  } else if (!strcmp(id, "skip")) {
    if (!strcmp(ev, "no")) menus_close();
    else if (!strcmp(ev, "yes")) {
      store_set_bool("TUTORIAL_BEGIN", true);
      store_set_bool("TUTORIAL_DONE", true);
      menus_close();
      extern void overworld_teleport(const char *location);
      overworld_teleport("hub");
    } else if (!strcmp(ev, "controls")) menus_open("controls");
    else if (!strcmp(ev, "leader")) menus_open("leaderboard");
    else if (!strcmp(ev, "settings")) menus_open("settings");
  } else if (!strcmp(id, "stats")) {
    if (!strcmp(ev, "close")) menus_close();
    else if (!strcmp(ev, "controls")) menus_open("controls");
    else if (!strcmp(ev, "leader")) menus_open("leaderboard");
    else if (!strcmp(ev, "settings")) menus_open("settings");
    else
      for (int i = 0; i < 7; i++)
        if (!strcmp(ev, sports[i])) {
          close_all();
          extern void overworld_teleport(const char *location);
          if (!strcmp(game.name, "overworld")) overworld_teleport(sports[i]);
          else {
            char s[40];
            snprintf(s, sizeof s, "overworld@%s", sports[i]);
            game_go(s);
          }
        }
  } else if (!strcmp(id, "settings")) {
    if (!strcmp(ev, "back")) menus_close();
    else if (!strcmp(ev, "newGame")) menus_open("newgame");
  } else if (!strcmp(id, "newgame")) {
    if (!strcmp(ev, "no")) menus_close();
    else if (!strcmp(ev, "yes")) {
      store_clear();
      store_save();
      game_go("overworld");
      close_all();
    }
  } else if (!strcmp(id, "leaderboard") || !strcmp(id, "controls")) {
    if (!strcmp(ev, "back") || !strcmp(ev, "okay")) menus_close();
  }
}

/* ---------------------------------------------------------------- navigation (Lq, Nq) */
static int nav_order(NodeId b) { return comp_int(nodes[b].T, C_keyboardNav, F_order, 0); }

static int buttons(NodeId m, NodeId *out, int max) {
  int n = 0;
  for (NodeId c = nodes[m].first; c && n < max; c = nodes[c].next)
    if (node_visible(c) && nodes[c].T != NONE16 && comp_has(nodes[c].T, C_button) && comp_has(nodes[c].T, C_keyboardNav)) out[n++] = c;
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && nav_order(out[j - 1]) > nav_order(out[j]); j--) { NodeId t = out[j]; out[j] = out[j - 1]; out[j - 1] = t; }
  return n;
}

static void menu_nav(NodeId m) {
  NodeId list[24];
  int n = buttons(m, list, 24);
  bool ok = false;
  for (int i = 0; i < n; i++) if (list[i] == focus) ok = true;
  if (!ok) focus = n ? list[0] : 0;
  for (int i = 0; i < n; i++) node_goto(list[i], list[i] == focus ? "focus" : "idle", 0, false);
  if (in.pressed[A_ACTION] && focus) {
    input_consume(A_ACTION);
    uint16_t e = comp_str(nodes[focus].T, C_button, F_eventId);
    if (e != NONE16) on_event(m, str(e));
    return;
  }
  int d = dir_of(in.jx, in.jy, true);
  if (d != last_dir) held_ticks = 0;
  held_ticks++;
  last_dir = d;
  if (d < 0 || !(held_ticks == 1 || (held_ticks > 15 && held_ticks % 3 == 0)) || !focus) return;
  if (d == DIR_N || d == DIR_S) {
    for (int i = 0; i < n; i++)
      if (list[i] == focus) {
        int j = d == DIR_N ? i - 1 : i + 1;
        if (j >= 0 && j < n) focus = list[j];
        break;
      }
  } else {
    /* sideways: the nearest button that way */
    Mat f = node_local(focus);
    NodeId best = 0;
    float bs = 1e9f;
    for (int i = 0; i < n; i++) {
      if (list[i] == focus) continue;
      Mat o = node_local(list[i]);
      float dx = o.tx - f.tx, dy = o.ty - f.ty;
      if ((d == DIR_E && dx <= 0) || (d == DIR_W && dx >= 0)) continue;
      float s = fabsf(dx) + 3 * fabsf(dy);
      if (s < bs) { bs = s; best = list[i]; }
    }
    if (best) focus = best;
  }
}

/* ---------------------------------------------------------------- results (Xo) */
void menus_game_over(float score) { menus_game_over_rated(score, -1); }

/* Xo(scene, score, rating): a sport may give its own rating (-1: the table's) */
void menus_game_over_rated(float score, int rating) {
  char key[40];
  snprintf(key, sizeof key, "%s%s%s", game.name, game.variant[0] ? ":" : "", game.variant);
  const ScoreRule *r = score_rule(key);
  if (!r) { snprintf(key, sizeof key, "%s", game.name); r = score_rule(key); }
  if (rating < 0) rating = score_rating(key, score);
  bool has;
  float best = score_best(key, &has);
  /* Do(): the best score, the rating */
  char k2[48];
  best_improved = false;
  if ((r && !r->time && (!has || best < score)) || (r && r->time && (!has || best > score))) {
    snprintf(k2, sizeof k2, "%s_score", key);
    store_set_num(k2, score);
    best_improved = true;
  }
  if (rating > 0) {
    snprintf(k2, sizeof k2, "%s_rating", key);
    if (store_num(k2, 0) < rating) store_set_num(k2, (float)rating);
    store_set_num("SUBMITTED_SCORES", store_num("SUBMITTED_SCORES", 0) + rating);
  }
  store_save();
  if (rating == 3) {
    /* a new champion: their outro, and the ending once all seven are won */
    char v[40];
    snprintf(v, sizeof v, "%soutro_VIDEO_SEEN", game.name);
    if (!store_bool(v, false)) { snprintf(v, sizeof v, "video:%soutro", game.name); game_go(v); return; }
    bool all = true;
    for (int i = 0; i < 7; i++) if (rating_of(sports[i]) < 3) all = false;
    if (all && !store_bool("outro_VIDEO_SEEN", false)) { game_go("video:outro"); return; }
  }
  menus_open("results");
  NodeId m = current_menu();
  NodeId stars = node_find(m, "ratings");
  if (stars) node_goto(stars, NULL, rating, false);
  count_from = 0; count_to = score;
  count_best_from = has ? best : 0; count_best_to = best_improved ? score : count_best_from;
  count_t = 0;
  results_counting = true;
  (void)r;
}

static void format_score(char *buf, size_t n, float v, bool time) {
  if (time) {
    int f = (int)v;
    snprintf(buf, n, "%d:%02d.%02d", f / 1800 % 60, f / 30 % 60, (int)(f % 30 / 30.0f * 100));
  } else snprintf(buf, n, "%d", (int)ceilf(v));
}

static void results_tick(NodeId m) {
  if (!results_counting) return;
  char key[40];
  snprintf(key, sizeof key, "%s%s%s", game.name, game.variant[0] ? ":" : "", game.variant);
  const ScoreRule *r = score_rule(key);
  bool time = r && r->time;
  count_t++;
  float t = count_t >= 20 ? 1 : count_t / 20.0f;
  format_score(score_buf, sizeof score_buf, count_from + (count_to - count_from) * t, time);
  format_score(best_buf, sizeof best_buf, count_best_from + (count_best_to - count_best_from) * t, time);
  set_text(m, "score", score_buf);
  set_text(m, "highScore", best_buf);
  if (count_t == 20) {
    NodeId stars = node_find(m, "ratings");
    for (int i = 0; stars && i < 4; i++) {
      char nm[12];
      snprintf(nm, sizeof nm, "rating%d", i);
      NodeId s = node_find(stars, nm);
      if (s) node_goto(s, NULL, 1, true);
    }
  }
  if (count_t > 26) results_counting = false;
}

/* the doodle's team colours (Ts) */
static uint16_t team_color(const char *team) {
  static const struct { const char *team; uint16_t c; } teams[] = {
      {"BLUE", 0x657E}, {"GREEN", 0x6F8D}, {"RED", 0xFACB}, {"YELLOW", 0xF74D}, {"NO_TEAM", 0xFFFF}};
  for (unsigned i = 0; i < sizeof teams / sizeof teams[0]; i++)
    if (!strcmp(teams[i].team, team)) return teams[i].c;
  return 0xFFFF;
}

/* Gp: the map with each sport's best, its stars, the team and where the player is */
static void stats_tick(NodeId m) {
  static const char *const nm[7] = {"archery", "climbing", "marathon", "pingpong", "rugby", "skate", "syncswim"};
  static const char *const stars[4] = {"0star", "1star", "2star", "3star"};
  for (int i = 0; i < 7; i++) {
    char k[24];
    bool has;
    float s = score_best(sports[i], &has);
    const ScoreRule *r = score_rule(sports[i]);
    if (has) format_score(stat_bufs[i], sizeof stat_bufs[i], s, r && r->time);
    else snprintf(stat_bufs[i], sizeof stat_bufs[i], "???");
    snprintf(k, sizeof k, "%sScore", nm[i]);
    NodeId sc = node_find(m, k);
    if (sc) node_set_text(sc, stat_bufs[i]);
    snprintf(k, sizeof k, "%sStars", nm[i]);
    NodeId st = node_find(m, k);
    int rt = rating_of(sports[i]);
    if (st) node_goto(st, stars[rt < 0 ? 0 : rt > 3 ? 3 : rt], 0, false);
  }
  /* the team, in its colour */
  const char *team = store_str("PLAYER_TEAM");
  static char up[16];
  snprintf(up, sizeof up, "%s", team && team[0] ? team : "NO_TEAM");
  for (char *c = up; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
  NodeId tn = node_find(m, "teamName");
  if (tn) {
    node_set_text(tn, msg(up));
    node_set_text_color(tn, team_color(up));
  }
  /* the player on the map: the island (Vs) squeezed into the map picture (Ws) */
  NodeId map = node_find(m, "map"), pl = map ? node_find(map, "player") : 0;
  if (pl && store_get("PLAYER_POS_X").type == SV_NUM) {
    float nx = (store_num("PLAYER_POS_X", 0) + 1536) / 3088, ny = (store_num("PLAYER_POS_Y", 0) + 833) / 1600;
    nodes[pl].x = 120 * clampf(nx, .1f, .9f);
    nodes[pl].y = 96 * clampf(ny, .1f, .85f);
  }
}

void menus_tick(void) {
  if (!depth || !root) return;
  node_tick(root);
  NodeId m = current_menu();
  if (!m) { depth = 0; return; }
  if (!strcmp(menu_id(m), "results")) results_tick(m);
  if (!strcmp(menu_id(m), "stats")) stats_tick(m);
  /* Back closes (not the results) */
  if (in.pressed[A_BACK] && strcmp(menu_id(m), "results")) {
    input_consume(A_BACK);
    menus_close();
    return;
  }
  menu_nav(m);
  node_update(root);
}

void menus_draw(void) {
  if (!depth || !root) return;
  gfx_overlay(true);
  node_draw(root, (Mat){1.0f / 3, 0, 0, 1.0f / 3, 0, 0});
  gfx_overlay(false);
}
