/* The cutscene and the doodle's films.
 *
 * The one cutscene (the cutscene library's "intro", Qq) is an Animate movie
 * with subtitles: it plays to its end (Gq), or Back or OK on its close button
 * leave it (Iq, Pq), and the island comes back.
 *
 * The films (the intro, each champion's intro and outro, the ending) are
 * videos in the doodle; the calculator has no room for them, so they are
 * marked as seen and the game goes on as it does after one. */
#include <stdio.h>
#include "ent.h"

static NodeId film;

static void translate(NodeId n) {
  ent_translate(n);
  for (NodeId c = nodes[n].first; c; c = nodes[c].next) translate(c);
}

static void came_on(NodeId n) {
  ent_register_tree(n);
  translate(n);
}

static void start_cutscene(void) {
  film = 0;
  node_dynamic_sym = S_cutscene_nbb;   /* the movie inside the cutscene entity */
  NodeId n = node_new_sym(S_cutscene_pab);
  node_dynamic_sym = NONE16;
  if (!n) return;
  node_stream_hook = came_on;
  node_add(game.root, n);
  ent_register_tree(n);
  translate(n);
  film = ent_first(C_cutscene);
  if (film) node_goto(film, NULL, 0, true);
}

static void tick_cutscene(void) {
  /* Pq, Iq: Back, or OK on the close button */
  if (in.pressed[A_BACK] || in.pressed[A_ACTION]) {
    input_consume(A_BACK);
    input_consume(A_ACTION);
    game_go("overworld");
    return;
  }
  /* Gq: the end of the movie */
  if (film && nodes[film].frame + 1 >= node_frames(film)) game_go("overworld");
}

static void end_cutscene(void) {
  film = 0;
  node_stream_hook = NULL;
}

static void start_video(void) {
  char k[48];
  snprintf(k, sizeof k, "%s_VIDEO_SEEN", game.variant);
  store_set_bool(k, true);
  /* Iu: after a sport's first film comes the sport; after a champion's
   * outro, the ending once all seven are won, else the island */
  size_t n = strlen(game.variant);
  if (n > 5 && !strcmp(game.variant + n - 5, "outro") && strcmp(game.variant, "outro")) {
    static const char *const sports[7] = {"archery", "climbing", "marathon", "pingpong", "rugby", "skate", "swim"};
    bool all = true;
    for (int i = 0; i < 7; i++) if (rating_of(sports[i]) < 3) all = false;
    if (all) store_set_bool("outro_VIDEO_SEEN", true);
  }
  if (n > 5 && !strcmp(game.variant + n - 5, "intro") && strcmp(game.variant, "intro")) {
    char s[32];
    snprintf(s, sizeof s, "%.*s", (int)(n - 5), game.variant);
    game_go(s);
  } else game_go("overworld");
}

static void tick_video(void) {}

const SceneDef scene_cutscene = {"cutscene", start_cutscene, tick_cutscene, end_cutscene, NULL, NULL, NULL};
const SceneDef scene_video = {"video", start_video, tick_video, NULL, NULL, NULL, NULL};
