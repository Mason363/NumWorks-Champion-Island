/* The calculator: EADK display, keyboard, time and files. */
#include <eadk.h>
#include "../common/epsilon_app.h"
#include "../common/epsilon_files.h"
#include "ci.h"

#define KEY(k) ((uint64_t)1 << (k))

uint32_t plat_keys(void) {
  uint64_t k = eadk_keyboard_scan();
  uint32_t r = 0;
  if (k & (KEY(eadk_key_left) | KEY(eadk_key_four))) r |= K_LEFT;
  if (k & (KEY(eadk_key_right) | KEY(eadk_key_six))) r |= K_RIGHT;
  if (k & (KEY(eadk_key_up) | KEY(eadk_key_eight))) r |= K_UP;
  if (k & (KEY(eadk_key_down) | KEY(eadk_key_two))) r |= K_DOWN;
  if (k & (KEY(eadk_key_ok) | KEY(eadk_key_exe) | KEY(eadk_key_five))) r |= K_ACTION;
  if (k & (KEY(eadk_key_back) | KEY(eadk_key_backspace))) r |= K_BACK;
  if (k & (KEY(eadk_key_home) | KEY(eadk_key_on_off))) r |= K_HOME;
  if (k & (KEY(eadk_key_shift) | KEY(eadk_key_toolbox))) r |= K_PAUSE;
  if (k) r |= K_ANY;
  return r;
}

uint32_t plat_millis(void) { return (uint32_t)eadk_timing_millis(); }
void plat_sleep(uint32_t ms) { eadk_timing_msleep(ms); }
void plat_push(int x, int y, int w, int h, const uint16_t *px) {
  eadk_display_push_rect((eadk_rect_t){(uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h}, px);
}
void plat_fill(int x, int y, int w, int h, uint16_t c) {
  if (w > 0 && h > 0) eadk_display_push_rect_uniform((eadk_rect_t){(uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h}, c);
}
void plat_vsync(void) { eadk_display_wait_for_vblank(); }

bool plat_save(const char *name, const void *data, uint32_t len) { return ef_write(name, data, len); }
const uint8_t *plat_load(const char *name, uint32_t *len) { return ef_read(name, len); }

static uint32_t seed = 0x9E3779B9u;
uint32_t plat_random(void) {
  seed ^= seed << 13;
  seed ^= seed >> 17;
  seed ^= seed << 5;
  return seed;
}
void plat_seed(uint32_t s) { seed = s ? s : 1; }

void plat_begin(void) { np_app_begin(); plat_seed((uint32_t)eadk_timing_millis() * 2654435761u); }
int plat_end(void) { return np_app_end(); }
