/* A computer: the screen goes to a frame buffer that tests save as images,
 * keys come from a script, time is simulated. */
#include <stdio.h>
#include <stdlib.h>
#include "ci.h"

uint16_t host_fb[SCREEN_W * SCREEN_H];
uint32_t host_time;               /* ms */
uint32_t host_keys;
static uint32_t seed = 12345;

uint32_t plat_keys(void) { return host_keys; }
uint32_t plat_millis(void) { return host_time; }
void plat_sleep(uint32_t ms) { host_time += ms; }
void plat_push(int x, int y, int w, int h, const uint16_t *px) {
  for (int r = 0; r < h; r++)
    for (int c = 0; c < w; c++)
      if ((unsigned)(x + c) < SCREEN_W && (unsigned)(y + r) < SCREEN_H) host_fb[(y + r) * SCREEN_W + x + c] = px[r * w + c];
}
void plat_fill(int x, int y, int w, int h, uint16_t c) {
  for (int r = 0; r < h; r++)
    for (int k = 0; k < w; k++)
      if ((unsigned)(x + k) < SCREEN_W && (unsigned)(y + r) < SCREEN_H) host_fb[(y + r) * SCREEN_W + x + k] = c;
}
void plat_vsync(void) {}

static char save_dir[256] = "build/host-saves";
void host_save_dir(const char *d) { snprintf(save_dir, sizeof save_dir, "%s", d); }
bool plat_save(const char *name, const void *data, uint32_t len) {
  char p[512];
  snprintf(p, sizeof p, "%s/%s", save_dir, name);
  FILE *f = fopen(p, "wb");
  if (!f) return false;
  fwrite(data, 1, len, f);
  fclose(f);
  return true;
}
static uint8_t loaded[65536];
const uint8_t *plat_load(const char *name, uint32_t *len) {
  char p[512];
  snprintf(p, sizeof p, "%s/%s", save_dir, name);
  FILE *f = fopen(p, "rb");
  if (!f) return NULL;
  *len = (uint32_t)fread(loaded, 1, sizeof loaded, f);
  fclose(f);
  return loaded;
}
uint32_t plat_random(void) {
  seed ^= seed << 13;
  seed ^= seed >> 17;
  seed ^= seed << 5;
  return seed;
}
void plat_seed(uint32_t s) { seed = s ? s : 1; }
void plat_begin(void) {}
int plat_end(void) { return 0; }

/* writes the screen as a PPM image */
void host_shot(const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
  for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
    uint16_t c = host_fb[i];
    uint8_t rgb[3] = {(uint8_t)((c >> 11) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63), (uint8_t)((c & 31) * 255 / 31)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}
