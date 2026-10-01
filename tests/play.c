/* Plays Champion Island on a computer, with scripted keys, and saves frames.
 *
 *   play [--scene SPEC] [--frames N] [--keys "10-40:right,50:ok,..."] [--shots 30,60] [--gif 0-300:2]
 *        [--out DIR] [--saves DIR] [--fresh]
 *
 * Keys: left right up down ok back home pause. "a-b:key" holds the key from frame a to b, "a:key" for one frame.
 * Screenshots are DIR/shot_<frame>.ppm; --gif saves every k-th frame of a range as DIR/f_<frame>.ppm.
 * CI_WANDER=seed plays keys itself (in and out of doors), CI_STUCK=1 says when the player cannot move,
 * CI_TUTORIAL_DONE=1 starts past the tutorial.
 * CI_CAM moves the camera of the sports that have one (see host_cam), CI_DROPS tells the frames whose
 * draw list was full. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <math.h>
#include "../src/ent.h"
#include "../src/spr.h"
#include "../src/phys.h"

extern uint32_t host_keys, host_time;
void host_shot(const char *path);
void host_save_dir(const char *d);
void ci_init(void);

typedef struct { int a, b; uint32_t k; } Hold;
static Hold holds[512];
static int nholds;

static uint32_t key_bit(const char *s) {
  if (!strcmp(s, "left")) return K_LEFT;
  if (!strcmp(s, "right")) return K_RIGHT;
  if (!strcmp(s, "up")) return K_UP;
  if (!strcmp(s, "down")) return K_DOWN;
  if (!strcmp(s, "ok")) return K_ACTION;
  if (!strcmp(s, "back")) return K_BACK;
  if (!strcmp(s, "home")) return K_HOME;
  if (!strcmp(s, "pause")) return K_PAUSE;
  return 0;
}

void world_census(void);
int world_redo(char *where, int size);
int world_layer_diff(int box[4]);
extern uint16_t host_fb[SCREEN_W * SCREEN_H];

/* CI_DIFF=k: every k frames on the island, the frame kept up as the camera
 * moved against one made from scratch there (the layer painted again, the
 * live children streamed again); prints where they differ (CI_DIFF_OUT=DIR:
 * saves both) */
static void diff_check(int f) {
  static uint16_t kept[SCREEN_W * SCREEN_H];
  extern int gfx_missing;
  int missing = gfx_missing;
  memcpy(kept, host_fb, sizeof kept);
  char where[96];
  int kids = world_redo(where, sizeof where);
  if (kids < 0) return;
  for (int k = 0; k < 2 || (gfx_missing && k < 6); k++) {   /* what one leaves for the next frame (not cached) comes */
    gfx_redraw_all();
    game_draw();
  }
  if (gfx_missing) return;   /* (the frame made from scratch did not fit in the cache either) */
  int n = 0, x0 = SCREEN_W, y0 = SCREEN_H, x1 = -1, y1 = -1;
  for (int y = 0; y < SCREEN_H; y++)
    for (int x = 0; x < SCREEN_W; x++)
      if (kept[y * SCREEN_W + x] != host_fb[y * SCREEN_W + x]) {
        n++;
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
      }
  int lb[4], ln = world_layer_diff(lb);
  if (!n && !kids && !ln) return;
  fprintf(stderr, "diff f%d: %d px in %d,%d-%d,%d, %d children missing, %d items left for later, layer %d px in %d,%d-%d,%d, %s\n", f, n,
          x0, y0, x1, y1, kids, missing, ln, lb[0], lb[1] + VIEW_Y, lb[2], lb[3] + VIEW_Y, where);
  const char *dir = getenv("CI_DIFF_OUT");
  if (dir && n) {
    char p[512];
    snprintf(p, sizeof p, "%s/diff_%05d_ref.ppm", dir, f);
    host_shot(p);
    uint16_t ref[SCREEN_W * SCREEN_H];
    memcpy(ref, host_fb, sizeof ref);
    memcpy(host_fb, kept, sizeof kept);
    snprintf(p, sizeof p, "%s/diff_%05d_kept.ppm", dir, f);
    host_shot(p);
    memcpy(host_fb, ref, sizeof ref);
  }
}
void ent_stats(int *live, int *states);
extern unsigned gfx_drops;

/* CI_CAM="f0:hold:x0:x1:dx:y0:y1:dy": from frame f0 the scene's camera puts
 * the map at each (x, y) of the grid (rows of x first), `hold` frames each
 * (the sports with a camera ask through host_cam) */
static int frame_now;
bool host_cam(float *x, float *y) {
  const char *s = getenv("CI_CAM");
  int f0, hold;
  float x0, x1, dx, y0, y1, dy;
  if (!s || sscanf(s, "%d:%d:%f:%f:%f:%f:%f:%f", &f0, &hold, &x0, &x1, &dx, &y0, &y1, &dy) != 8 || hold < 1 || frame_now < f0) return false;
  int nx = dx ? (int)((x1 - x0) / dx + 1.001f) : 1, ny = dy ? (int)((y1 - y0) / dy + 1.001f) : 1;
  int i = (frame_now - f0) / hold;
  if (nx < 1 || ny < 1 || i >= nx * ny) i = nx * ny - 1;
  *x = x0 + dx * (i % nx);
  *y = y0 + dy * (i / nx);
  return true;
}

/* CI_WANDER=seed: the keys of a player going in and out of the doors near
 * them, at random (tests/check.py): arrows held through a scene change, OK
 * tapped, held or pressed early at a door, the map opened as a room fades in,
 * a walk or a talk in the room, then an exit, or the same door again at once */
static uint32_t rng = 1;
static int rnd(int n) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (int)(rng % (uint32_t)n); }
static const uint32_t DIR_KEYS[8] = {K_RIGHT, K_RIGHT | K_DOWN, K_DOWN, K_DOWN | K_LEFT, K_LEFT, K_LEFT | K_UP, K_UP, K_UP | K_RIGHT};
static struct {
  const SceneDef *def;
  uint32_t ticks;
  int phase, t, len;          /* phase: 0 carry on, 1 wander, 2 to a door, 3 at the door */
  uint32_t carry, keys;       /* keys kept through a scene change, keys now */
  int back_at, back_close;    /* Back pressed at, closed at (frames of the phase) */
  NodeId door;
  float best;
  int noprog, jig, jig_keys, ok_mode, ok_t;
} pilot;

static NodeId player_ent(void) { return game.def && game_is_world(game.name) ? ent_first(C_overworldPlayer) : 0; }

static const char *door_name(NodeId n) {
  uint16_t T = nodes[n].T, s = NONE16;
  if (comp_has(T, C_scenePortal)) s = comp_str(T, C_scenePortal, F_name);
  else if (comp_has(T, C_sceneTrigger)) s = comp_str(T, C_sceneTrigger, F_name);
  return s != NONE16 ? str(s) : NULL;
}

/* a door of the island or the room (not a sport) near the player: the nearest or the next */
static NodeId pick_door(NodeId p) {
  float px, py;
  ent_pos(p, &px, &py);
  NodeId best[2] = {0, 0};
  float bd[2] = {300 * 300, 300 * 300};
  for (int i = 0; i < ent_count(); i++) {
    NodeId n = ent_at(i);
    const char *nm = n ? door_name(n) : NULL;
    if (!nm || !node_visible_chain(n) || (strncmp(nm, "interior", 8) && strncmp(nm, "overworld", 9))) continue;
    float x, y;
    ent_pos(n, &x, &y);
    float d = (x - px) * (x - px) + (y - py) * (y - py);
    if (d < bd[0]) { bd[1] = bd[0]; best[1] = best[0]; bd[0] = d; best[0] = n; }
    else if (d < bd[1]) { bd[1] = d; best[1] = n; }
  }
  return best[1] && rnd(2) ? best[1] : best[0];
}

static uint32_t wander_keys(void) {
  NodeId p = player_ent();
  if (!p) return 0;
  if (game.def != pilot.def || game.ticks < pilot.ticks) {   /* a new scene */
    pilot.def = game.def;
    pilot.carry = rnd(2) ? pilot.keys & (K_LEFT | K_RIGHT | K_UP | K_DOWN | K_ACTION) : 0;
    pilot.phase = 0;
    pilot.t = 0;
    pilot.len = rnd(3) ? rnd(25) : 0;
    pilot.back_at = rnd(4) ? -1 : rnd(12);
    pilot.back_close = pilot.back_at + 2 + rnd(30);
    pilot.door = 0;
  }
  pilot.ticks = game.ticks;
  uint32_t k = 0;
  if (pilot.back_at >= 0 && (pilot.t == pilot.back_at || pilot.t == pilot.back_close)) k |= K_BACK;
  if (menus_active()) { pilot.t++; return k | (pilot.t > pilot.back_close + 5 && pilot.t % 7 == 0 ? K_BACK : 0); }
  if (dialog_active()) { pilot.t++; return k | (pilot.t % 6 < 2 ? K_ACTION : 0); }
  pilot.t++;
  switch (pilot.phase) {
  case 0:   /* the keys held through the change */
    k |= pilot.carry;
    if (pilot.t >= pilot.len) { pilot.phase = 1; pilot.t = 0; pilot.len = rnd(3) ? rnd(!strcmp(game.name, "interior") ? 90 : 20) : 0; pilot.jig = 0; }
    break;
  case 1:   /* a walk, a talk */
    if (pilot.jig <= 0) { pilot.jig = 3 + rnd(20); pilot.jig_keys = rnd(3) ? DIR_KEYS[rnd(8)] : 0; }
    pilot.jig--;
    k |= pilot.jig_keys | (rnd(12) ? 0 : K_ACTION);
    if (pilot.t >= pilot.len) {
      pilot.door = pick_door(p);
      pilot.phase = 2; pilot.t = 0; pilot.best = 1e9f; pilot.noprog = 0; pilot.jig = 0;
      pilot.ok_mode = rnd(4);            /* 0 tap, 1 held from afar, 2 held long with arrows, 3 double tap */
      pilot.ok_t = 0;
    }
    break;
  case 2:   /* to the door */
  case 3: {
    if (!pilot.door || !(nodes[pilot.door].flags & NF_USED) || !door_name(pilot.door)) { pilot.phase = 1; pilot.t = 0; pilot.len = 10; break; }
    float px, py, dx, dy;
    ent_pos(p, &px, &py);
    ent_pos(pilot.door, &dx, &dy);
    dx -= px; dy -= py;
    float d = dx * dx + dy * dy;
    Ent *de = ent_peek(pilot.door);
    bool in_door = de && de->ntrig > 0;
    if (in_door && comp_has(nodes[pilot.door].T, C_scenePortal)) {
      pilot.phase = 3;
      pilot.ok_t++;
      /* OK let go in the door opens it */
      if (pilot.ok_mode == 0) k |= pilot.ok_t == 2 ? K_ACTION : 0;
      else if (pilot.ok_mode == 1) k |= pilot.ok_t < 3 ? K_ACTION : 0;
      else if (pilot.ok_mode == 2) k |= (pilot.ok_t < 25 ? K_ACTION : 0) | (pilot.ok_t < 30 ? DIR_KEYS[rnd(8)] : 0);
      else k |= pilot.ok_t == 2 || pilot.ok_t == 5 ? K_ACTION : 0;
      if (pilot.ok_t > 60) { pilot.phase = 1; pilot.t = 0; pilot.len = 5; }
      break;
    }
    if (pilot.ok_mode == 1 && d < 60 * 60) k |= K_ACTION;   /* OK held on the way */
    if (d < pilot.best - 1) { pilot.best = d; pilot.noprog = 0; } else pilot.noprog++;
    if (pilot.noprog > 8 && pilot.jig <= 0) { pilot.jig = 5 + rnd(15); pilot.jig_keys = DIR_KEYS[rnd(8)]; pilot.noprog = 0; pilot.best = 1e9f; }
    if (pilot.jig > 0) { pilot.jig--; k |= pilot.jig_keys; break; }
    if (dx > 2) k |= K_RIGHT;
    if (dx < -2) k |= K_LEFT;
    if (dy > 2) k |= K_DOWN;
    if (dy < -2) k |= K_UP;
    if (pilot.t > 400) { pilot.phase = 1; pilot.t = 0; pilot.len = 20; }
    break;
  }
  }
  pilot.keys = k;
  return k;
}

/* CI_STUCK: says when the player cannot move (STUCK): arrows held for 30
 * frames or more (all four ways, so not a wall), no dialogue or menu, and he
 * stays where he is; or he has no physics body. CI_STUCK=2 also prints where
 * he is every 10 frames */
static struct { const SceneDef *def; uint32_t ticks; NodeId p; float x, y; int still, dirs[4], nobody; } probe;
static int stuck_count;
static void stuck_probe(int f) {
  NodeId p = player_ent();
  if (!p || game.def != probe.def || game.ticks < probe.ticks || p != probe.p) {   /* a new scene */
    memset(&probe, 0, sizeof probe);
    probe.def = game.def;
    probe.p = p;
    if (p) ent_pos(p, &probe.x, &probe.y);
    probe.ticks = game.ticks;
    return;
  }
  probe.ticks = game.ticks;
  float x, y;
  ent_pos(p, &x, &y);
  Ent *e = ent_peek(p);
  int nb = 0;
  for (int b = 0; b < BODY_MAX; b++) {
    nb += bodies[b].used;
    /* a body is the scene's: its entity's (it knows it) or a wall's */
    NodeId u = bodies[b].user, r = u;
    while (r && nodes[r].parent) r = nodes[r].parent;
    Ent *ue = u ? ent_peek(u) : NULL;
    if (bodies[b].used && u && (r != game.root || !ue || ue->body != b)) printf("BODY f%d %d of node %u not the scene's\n", f, b, u);
  }
  bool overlay = dialog_active() || menus_active();   /* the scene's systems wait */
  if (!overlay) {
    if (e && e->body >= 0) probe.nobody = 0;
    else if (++probe.nobody == 3) {
      stuck_count++;
      printf("STUCK f%d %s:%s no body, bodies %d\n", f, game.name, game.variant, nb);
    }
  }
  if (atoi(getenv("CI_STUCK")) > 1 && f % 10 == 0)
    printf("f%d %s:%s keys %x at %.1f,%.1f body %d\n", f, game.name, game.variant, host_keys, x, y, e ? e->body : -9);
  if (overlay || fabsf(x - probe.x) + fabsf(y - probe.y) > .01f) {
    probe.still = 0;
    memset(probe.dirs, 0, sizeof probe.dirs);
  } else if (host_keys & (K_LEFT | K_RIGHT | K_UP | K_DOWN)) {
    probe.still++;
    for (int d = 0; d < 4; d++) if (host_keys & (1u << d)) probe.dirs[d]++;   /* K_LEFT, K_RIGHT, K_UP, K_DOWN */
    if (probe.still >= 30 && probe.dirs[0] >= 4 && probe.dirs[1] >= 4 && probe.dirs[2] >= 4 && probe.dirs[3] >= 4) {
      stuck_count++;
      printf("STUCK f%d %s:%s at %.1f,%.1f for %d frames, body %d, bodies %d\n", f, game.name, game.variant, x, y, probe.still,
             e ? e->body : -9, nb);
      probe.still = -1000000;   /* told once */
    }
  }
  probe.x = x;
  probe.y = y;
}

int main(int argc, char **argv) {
  const char *scene = NULL, *out = "build/play", *saves = "build/play/saves", *shots = "", *gif = NULL;
  int frames = 300;
  unsigned peak_nodes = 0;
  uint64_t z_max = 0;
  int z_heavy = 0;   /* frames decoding more than 60 KB */
  int peak_live = 0, peak_states = 0, peak_bodies = 0;
  bool fresh = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--scene") && i + 1 < argc) scene = argv[++i];
    else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    else if (!strcmp(argv[i], "--saves") && i + 1 < argc) saves = argv[++i];
    else if (!strcmp(argv[i], "--shots") && i + 1 < argc) shots = argv[++i];
    else if (!strcmp(argv[i], "--gif") && i + 1 < argc) gif = argv[++i];
    else if (!strcmp(argv[i], "--fresh")) fresh = true;
    else if (!strcmp(argv[i], "--keys") && i + 1 < argc) {
      char *s = strdup(argv[++i]);
      for (char *t = strtok(s, ","); t && nholds < 512; t = strtok(NULL, ",")) {
        int a, b;
        char k[16];
        if (sscanf(t, "%d-%d:%15s", &a, &b, k) == 3) holds[nholds++] = (Hold){a, b, key_bit(k)};
        else if (sscanf(t, "%d:%15s", &a, k) == 2) holds[nholds++] = (Hold){a, a, key_bit(k)};
      }
    }
  }
  mkdir(out, 0755);
  mkdir(saves, 0755);
  host_save_dir(saves);
  if (fresh) { char p[512]; snprintf(p, sizeof p, "%s/champion.sav", saves); remove(p); }
  int g0 = 0, g1 = -1, gs = 1;
  if (gif) sscanf(gif, "%d-%d:%d", &g0, &g1, &gs);
  ci_init();
  if (getenv("CI_ENDING")) {   /* the island after the ending: seven golds, every film seen */
    static const char *const sports[7] = {"archery", "climbing", "marathon", "pingpong", "rugby", "skate", "swim"};
    char k[48];
    for (int i = 0; i < 7; i++) {
      snprintf(k, sizeof k, "%s_rating", sports[i]);
      store_set_num(k, 3);
      snprintf(k, sizeof k, "%sintro_VIDEO_SEEN", sports[i]);
      store_set_bool(k, true);
      snprintf(k, sizeof k, "%soutro_VIDEO_SEEN", sports[i]);
      store_set_bool(k, true);
    }
    store_set_bool("TUTORIAL_DONE", true);
    store_set_bool("intro_VIDEO_SEEN", true);
    store_set_bool("outro_VIDEO_SEEN", true);
  }
  if (getenv("CI_TUTORIAL_DONE")) {   /* a player past the tutorial, the game opening on the island */
    store_set_bool("TUTORIAL_BEGIN", true);
    store_set_bool("TUTORIAL_DONE", true);
    store_set_bool("intro_VIDEO_SEEN", true);
  }
  /* CI_STORE="swim_rating=3,RAIN=complete": saved values to start with (numbers, else strings) */
  if (getenv("CI_STORE")) {
    char *s = strdup(getenv("CI_STORE"));
    for (char *t = strtok(s, ","); t; t = strtok(NULL, ",")) {
      char *eq = strchr(t, '='), *end;
      if (!eq) continue;
      *eq = 0;
      double v = strtod(eq + 1, &end);
      if (*end || end == eq + 1) store_set_str(t, eq + 1);
      else store_set_num(t, (float)v);
    }
    free(s);
  }
  if (scene) game_go(scene);
  const char *wander = getenv("CI_WANDER"), *stuck = getenv("CI_STUCK");
  if (wander) rng = 2654435761u * (uint32_t)(atoi(wander) + 1);
  for (int f = 0; f < frames && game_running; f++) {
    /* CI_DLG=npc:node starts that dialogue (its layout, without walking to them) */
    if (getenv("CI_DLG") && f == 5) {
      char b[64], *c;
      snprintf(b, sizeof b, "%s", getenv("CI_DLG"));
      if ((c = strchr(b, ':'))) { *c = 0; dialog_start(str_id(b), str_id(c + 1)); }
    }
    frame_now = f;
    host_keys = 0;
    for (int i = 0; i < nholds; i++)
      if (f >= holds[i].a && f <= holds[i].b) host_keys |= holds[i].k;
    if (wander) host_keys |= wander_keys();
    extern uint64_t z_bytes;
    uint64_t zb0 = z_bytes;
    game_tick();
    if (stuck) stuck_probe(f);
    /* CI_POS="x,y": the player moved there (map space) after the first tick */
    if (f == 0 && getenv("CI_POS") && ent_first(C_overworldPlayer)) {
      float x = 0, y = 0;
      sscanf(getenv("CI_POS"), "%f,%f", &x, &y);
      ent_set_pos(ent_first(C_overworldPlayer), x, y);
      sys_camera_snap();
    }
    if (getenv("CI_CONSIST")) {
      extern NodeId node_unlisted(void);
      static bool told;
      NodeId u = node_unlisted();
      if (u && !told) {
        told = true;
        fprintf(stderr, "f%d: node %u (sym %u) is not listed by its parent %u (sym %u), scene %s\n", f, u, nodes[u].sym, nodes[u].parent,
                nodes[nodes[u].parent].sym, game.name);
      }
    }
    game_draw();
    static unsigned drops;
    if (gfx_drops != drops && getenv("CI_DROPS")) fprintf(stderr, "f%d: %u items not drawn\n", f, gfx_drops - drops);
    drops = gfx_drops;
    if (getenv("CI_DIFF") && f % atoi(getenv("CI_DIFF")) == 0) diff_check(f);
    extern int fly_held;
    if (fly_held >= 0) {   /* CI_PATH with CI_HOLD: the view once still at a point (its hash, what it left out) */
      extern int gfx_missing;
      uint32_t h = 2166136261u;
      for (int i = 0; i < SCREEN_W * SCREEN_H; i++) h = (h ^ host_fb[i]) * 16777619u;
      char p[512];
      snprintf(p, sizeof p, "%s/held_%03d.ppm", out, fly_held);
      if (getenv("CI_HOLDSHOT")) host_shot(p);
      fprintf(stderr, "held %d held_%03d.ppm %08x left %d\n", fly_held, fly_held, h, gfx_missing);
      fly_held = -1;
    }
    uint64_t zf = z_bytes - zb0;
    if (getenv("CI_ZFRAME") && zf) fprintf(stderr, "f%d decoded %.1f KB\n", f, zf / 1024.0);
    if (zf > z_max) z_max = zf;
    if (zf > 60 * 1024) z_heavy++;
    if (node_count() > peak_nodes) peak_nodes = node_count();
    if (getenv("CI_NODES") && f % atoi(getenv("CI_NODES")) == 0) fprintf(stderr, "f%d nodes %u scene %s\n", f, node_count(), game.name);
    int live, states;
    ent_stats(&live, &states);
    if (live > peak_live) peak_live = live;
    if (states > peak_states) peak_states = states;
    int nb = 0;
    for (int b = 0; b < BODY_MAX; b++) nb += bodies[b].used;
    if (nb > peak_bodies) peak_bodies = nb;
    host_time += 33;
    char p[512];
    bool shot = false;
    for (const char *s = shots; *s;) {
      if (atoi(s) == f) shot = true;
      const char *c = strchr(s, ',');
      if (!c) break;
      s = c + 1;
    }
    if (shot) { snprintf(p, sizeof p, "%s/shot_%d.ppm", out, f); host_shot(p); }
    if (gif && f >= g0 && f <= g1 && (f - g0) % gs == 0) { snprintf(p, sizeof p, "%s/f_%05d.ppm", out, f); host_shot(p); }
  }
  extern int gfx_peak_items, gfx_peak_affs;
  extern unsigned node_alloc_fails;
  if (node_alloc_fails) printf("NODE POOL FULL: %u nodes not made\n", node_alloc_fails);
  if (gfx_drops) printf("DRAW LIST FULL: %u items not drawn\n", gfx_drops);
  printf("frames done: %u ticks, nodes %u (peak %u), entities peak %d (states %d), bodies %d, items %d affs %d, cache %u, scene %s:%s\n",
         game.ticks, node_count(), peak_nodes, peak_live, peak_states, peak_bodies, gfx_peak_items, gfx_peak_affs, spr_cache_used(), game.name,
         game.variant);
  if (getenv("CI_ZSTAT")) {
    extern uint64_t z_bytes, z_opens;
    fprintf(stderr, "decoded %.1f KB a frame in %.1f opens, at most %.0f KB, %d frames over 60 KB\n", z_bytes / 1024.0 / frames,
            (double)z_opens / frames, z_max / 1024.0, z_heavy);
  }
  if (getenv("CI_CENSUS")) world_census();
  if (stuck) printf("stuck %d\n", stuck_count);
  return 0;
}
