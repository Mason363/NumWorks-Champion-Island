/* The HUD: the compass (the map button) in the world and the pause button
 * in the sports, where the doodle shows them on computers (Jp, Kp). */
#include "ent.h"

static NodeId root, pause_btn, world_btn;

void hud_init(void) {
  root = node_new_sym(S_hud_Ap);
  pause_btn = world_btn = 0;
  for (NodeId c = root ? nodes[root].first : 0; c; c = nodes[c].next) {
    if (nodes[c].T == NONE16) continue;
    /* createjs.ButtonHelper stops a button on its first frame (the last one is its green hit area) */
    NodeId b = node_child(c, "button");
    if (b) node_goto(b, NULL, 0, false);
    if (comp_has(nodes[c].T, C_pauseButton)) pause_btn = c;
    if (comp_has(nodes[c].T, C_overworldButton)) world_btn = c;
  }
}

void hud_tick(void) {
  if (!root) return;
  bool calm = !dialog_active() && !menus_active();
  bool tut = !strcmp(game.variant, "tutorial");
  if (pause_btn) node_set_visible(pause_btn, calm && game_is_sport(game.name) && !tut);
  if (world_btn) node_set_visible(world_btn, calm && game_is_world(game.name));
  node_tick(root);
  node_update(root);
}

void hud_draw(void) {
  /* in the screen's top left corner, above the island too */
  if (root) node_draw(root, (Mat){1.0f / 3, 0, 0, 1.0f / 3, 0, (float)gfx_view_top()});
}
