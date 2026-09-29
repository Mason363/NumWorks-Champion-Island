/* Champion Island: shared declarations.
 *
 * The game runs the Doodle's own content: its Animate libraries (baked by
 * tools/extract.js, packed by tools/pack.py into data.bin) drive a small
 * CreateJS-like display tree (node.c), drawn at the doodle's 320x180 in the
 * middle of the 320x240 screen (gfx.c). Game logic follows the doodle's
 * systems, at its 30 frames per second. */
#ifndef CI_H
#define CI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "data.h"

#define SCREEN_W 320
#define SCREEN_H 240
#define VIEW_W 320
#define VIEW_H 180
#define VIEW_Y ((SCREEN_H - VIEW_H) / 2)
#define FPS 30

/* ---------------------------------------------------------------- data.bin */
extern const uint8_t ci_data[];
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline int16_t rds16(const uint8_t *p) { return (int16_t)rd16(p); }
static inline uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static inline float rdf(const uint8_t *p) { uint32_t v = rd32(p); float f; memcpy(&f, &v, 4); return f; }
#define HDR(i) rd32(ci_data + 4 * (i))
#define NONE16 0xFFFF

const char *str(uint16_t id);                   /* identifiers and short strings */
int dtext(uint16_t id, char *buf, int size);    /* a dialogue line (decompressed into buf), its length */
int str_find(const char *s);                    /* id of a string, or -1 */
uint16_t str_id(const char *s);                 /* id of a string that must exist */

/* sprites */
typedef struct { uint16_t w, h; uint8_t sheet, kind; uint16_t bank; uint32_t off, rle; } Sprite;   /* rle: bytes once cached */
void sprite_info(uint16_t id, Sprite *s);
const uint16_t *pal565(uint8_t sheet);
const uint8_t *palalpha(uint8_t sheet);

/* symbols */
typedef struct {
  uint8_t type, lib;
  uint32_t v;      /* bitmap: sprite; shape: payload; clip: offset of its data */
} SymInfo;
void sym_info(uint16_t sym, SymInfo *s);
/* a clip's header */
typedef struct {
  uint16_t nframes, nslots;
  uint8_t nlabels, nacts;
  uint16_t T;                 /* index in the component table or NONE16 */
  int16_t nb[4];              /* nominal bounds x, y, w, h */
  const uint8_t *labels, *acts, *slots;
} Clip;
bool clip_get(uint16_t sym, Clip *c);
int clip_label(const Clip *c, const char *label);   /* frame or -1 */
const uint8_t *payload(uint16_t id);
const float *mat(uint16_t id);                       /* a, b, c, d */

/* components (Animate "this.T") */
const uint8_t *comp_find(uint16_t T, int comp);      /* NULL if the symbol lacks it */
bool comp_has(uint16_t T, int comp);
int32_t comp_int(uint16_t T, int comp, int field, int32_t def);
float comp_float(uint16_t T, int comp, int field, float def);
bool comp_bool(uint16_t T, int comp, int field, bool def);
uint16_t comp_str(uint16_t T, int comp, int field);  /* NONE16 if absent */
bool comp_vec(uint16_t T, int comp, int field, float *x, float *y);
uint16_t comp_sym(uint16_t T, int comp, int field);   /* a symbol named by a component, or NONE16 */
const char *msg(const char *key);                      /* the doodle's UI texts (messages.en-GB) */

/* ---------------------------------------------------------------- platform */
enum {
  K_LEFT = 1 << 0, K_RIGHT = 1 << 1, K_UP = 1 << 2, K_DOWN = 1 << 3, K_ACTION = 1 << 4, K_BACK = 1 << 5,
  K_HOME = 1 << 6, K_PAUSE = 1 << 7, K_ANY = 1 << 8
};
uint32_t plat_keys(void);                 /* game keys held now */
uint32_t plat_millis(void);
void plat_sleep(uint32_t ms);
void plat_push(int x, int y, int w, int h, const uint16_t *px);
void plat_fill(int x, int y, int w, int h, uint16_t c);
void plat_vsync(void);
bool plat_save(const char *name, const void *data, uint32_t len);
const uint8_t *plat_load(const char *name, uint32_t *len);
uint32_t plat_random(void);

/* ---------------------------------------------------------------- math */
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
float frand(void);                                 /* [0, 1) */
float frand_range(float a, float b);
#endif
