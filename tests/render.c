/* Renders a symbol with the host build: render SYMID frames out.ppm */
#include <stdio.h>
#include <stdlib.h>
#include "../src/ci.h"
#include "../src/node.h"
#include "../src/spr.h"
void host_shot(const char *path);
int main(int argc, char **argv) {
  int sym = atoi(argv[1]), frames = argc > 2 ? atoi(argv[2]) : 1;
  node_reset();
  spr_reset();
  NodeId root = node_new_sym((uint16_t)sym);
  printf("nodes %u\n", node_count());
  gfx_clear_color(0);
  for (int f = 0; f < frames; f++) {
    if (f) node_tick(root);
    node_update(root);
    gfx_begin();
    node_draw(root, MAT_ID);
    gfx_end();
  }
  printf("cache %u\n", spr_cache_used());
  host_shot(argc > 3 ? argv[3] : "out.ppm");
  return 0;
}
