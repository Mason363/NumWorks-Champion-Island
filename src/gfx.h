#ifndef CI_GFX_H
#define CI_GFX_H
#include "ci.h"

/* A frame is a draw list (built while walking the display tree, in painting
 * order) rasterised band by band into the 320x180 view. */
typedef struct { float a, b, c, d, tx, ty; } Mat;   /* x' = a x + c y + tx, y' = b x + d y + ty */
static inline Mat mat_mul(Mat m, Mat n) {
  return (Mat){m.a * n.a + m.c * n.b, m.b * n.a + m.d * n.b, m.a * n.c + m.c * n.d, m.b * n.c + m.d * n.d,
               m.a * n.tx + m.c * n.ty + m.tx, m.b * n.tx + m.d * n.ty + m.ty};
}
static const Mat MAT_ID = {1, 0, 0, 1, 0, 0};

void gfx_begin(void);                                     /* new draw list */
void gfx_sprite(uint16_t sprite, Mat m, uint8_t alpha);  /* a sprite placed by m */
void gfx_sprite_ex(uint16_t sprite, Mat m, uint8_t alpha, bool opaque_only);   /* opaque_only: skips see-through pixels */
void gfx_shape(const uint8_t *shape, Mat m, uint8_t alpha);  /* filled polygons (payload) */
void gfx_rect(int x, int y, int w, int h, uint16_t c, uint8_t alpha);
/* one small image at n places (pos: x, y pairs, moved by ox, oy): 4-bit pixels,
 * index 0 clear, pal/al: RGB565 and alpha (0..32) of each index (the island's rain) */
void gfx_tiles(const uint8_t *px, int w, int h, const uint16_t *pal, const uint8_t *al, const int16_t *pos, int n, int ox, int oy, uint8_t alpha);
void gfx_points(const uint8_t *pts, int w, int h, const uint16_t *pal, const uint8_t *al, const int16_t *pos, int n, int ox, int oy, uint8_t alpha);   /* the same with pixels as (x, y, colour) */
/* text: font size in px of the doodle's layout, colour RGB565 */
void gfx_text(const char *s, Mat m, uint16_t color, uint8_t align, int16_t line_w, int16_t line_h, uint8_t alpha);
void gfx_text_k(const char *s, Mat m, uint16_t color, uint8_t align, int16_t line_w, int16_t line_h, uint8_t alpha, int scale);
void gfx_text_k3(const char *s, Mat m, uint16_t color, uint8_t align, int16_t line_w, int16_t line_h, uint8_t alpha, int k3);   /* scale in thirds */
void gfx_end(void);                                       /* rasterise and push the bands that changed */
void gfx_redraw_all(void);                                /* next frame: every band */
void gfx_clear_color(uint16_t c);                        /* what shows where nothing is drawn */
void gfx_letterbox(uint16_t c);                          /* fills the bands above and below the view */
/* the stage rows the screen shows: 0 to VIEW_H (bands above and below), or
 * more (-VIEW_Y to VIEW_H + VIEW_Y: the whole screen); a scene's own choice,
 * back to the stage at each new scene */
void gfx_view(int top, int bottom);
int gfx_view_top(void);
int gfx_view_bottom(void);
void gfx_overlay(bool on);                               /* menus: a shape over the whole stage covers the whole view */

/* The background layer: an 8-bit image of the scene's static backdrop in
 * world coordinates, redrawn only where the camera uncovers something. */
typedef void (*BgPaint)(int x0, int y0, int w, int h);  /* paint world rect into the layer via bg_blit* */
void bg_setup(uint8_t *mem, int w, int h, uint8_t sheet, BgPaint paint);
void bg_off(void);
void bg_invalidate(void);
void bg_camera(int x, int y);                             /* world position of the view's top-left */
void bg_blit_sprite(uint16_t sprite, int x, int y, bool flipx, bool flipy);
/* any image (banked, streamed or banded) with mirrors or a quarter turn, and
 * alpha blended into the palette through bg_blend_lut's table */
enum { BD_FLIPX = 1, BD_FLIPY = 2, BD_TRANSPOSE = 4 };
void bg_draw(uint16_t sprite, int x, int y, uint8_t flags, uint8_t alpha);
void bg_blend_lut(const uint8_t *lut444);                 /* RGB444 -> palette index (pack.py) */
void bg_water(const uint8_t *tile, int w, int h);         /* shown where the layer is clear (NULL: none) */
void bg_water4(const uint8_t *tile, int w, int h, const uint16_t *colours);   /* the same, 4 bits a pixel (low first) */
void bg_redraw_water(void);                               /* the water tile's pixels changed */
int bg_clear_index(void);                                 /* the palette's clear index, -1 if none */
void bg_blit_stream(uint16_t sprite, int x, int y);
void bg_fill(int x, int y, int w, int h, uint8_t index);
void bg_redraw(uint16_t sprite, int x, int y, uint8_t flags);   /* the layer again where the sprite at world x, y is opaque */
uint8_t *bg_layer(int *w, int *h);

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b);
#endif
