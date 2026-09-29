/* Plays Champion Island on a computer, with scripted keys, and saves frames.
 *
 *   play [--scene SPEC] [--frames N] [--keys "10-40:right,50:ok,..."] [--shots 30,60] [--gif 0-300:2]
 *        [--out DIR] [--saves DIR] [--fresh]
 *
 * Keys: left right up down ok back home pause. "a-b:key" holds the key from frame a to b, "a:key" for one frame.
 * Screenshots are DIR/shot_<frame>.ppm; --gif saves every k-th frame of a range as DIR/f_<frame>.ppm. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
void ent_stats(int *live, int *states);

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
  if (getenv("CI_ENDING")) store_set_bool("outro_VIDEO_SEEN", true);   /* the island after the ending */
  if (scene) game_go(scene);
  for (int f = 0; f < frames && game_running; f++) {
    host_keys = 0;
    for (int i = 0; i < nholds; i++)
      if (f >= holds[i].a && f <= holds[i].b) host_keys |= holds[i].k;
    extern uint64_t z_bytes;
    uint64_t zb0 = z_bytes;
    game_tick();
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
  printf("frames done: %u ticks, nodes %u (peak %u), entities peak %d (states %d), bodies %d, items %d affs %d, cache %u, scene %s:%s\n",
         game.ticks, node_count(), peak_nodes, peak_live, peak_states, peak_bodies, gfx_peak_items, gfx_peak_affs, spr_cache_used(), game.name,
         game.variant);
  if (getenv("CI_ZSTAT")) {
    extern uint64_t z_bytes, z_opens;
    fprintf(stderr, "decoded %.1f KB a frame in %.1f opens, at most %.0f KB, %d frames over 60 KB\n", z_bytes / 1024.0 / frames,
            (double)z_opens / frames, z_max / 1024.0, z_heavy);
  }
  if (getenv("CI_CENSUS")) world_census();
  return 0;
}
