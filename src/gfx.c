/* The renderer: a draw list rasterised in bands of BAND rows. */
#include "gfx.h"
#include <math.h>
#include "font.h"
#include "spr.h"
#include <stdlib.h>
#include <stdio.h>

#define BAND 4
#define MAX_ITEMS 288   /* 215 at most seen */
#define MAX_AFF 56      /* 41 at most seen */

enum { DI_SPRITE, DI_AFFINE, DI_RECT, DI_SHAPE, DI_TEXT, DI_STREAM, DI_TILES, DI_MASK, DI_HIDDEN /* under an overlay's panel */, DI_KINDS };
#define STREAM_OVER (16 * 1024)   /* sprites bigger than this once cached are decoded as they are drawn */
enum { DF_FLIPX = 1, DF_FLIPY = 2, DF_MISSING = 4, DF_OPAQUE = 8, DF_OVERLAY = 16 };   /* missing: not in the cache this frame; opaque: only fully opaque pixels; overlay: a menu's */
typedef struct {
  uint8_t kind, alpha, flags, pad;
  int16_t x0, y0, x1, y1;       /* covered screen area in view coordinates (x1, y1 excluded) */
  uint16_t ref;                 /* sprite, affine slot */
  uint16_t color;               /* rect/text colour, or the sprite of an affine item */
} Item;
static const void *item_ptr[MAX_AFF];   /* shape payload or text, by affine slot */
typedef struct { Mat m; uint8_t align, scale; int16_t lw, lh; } Aff;

extern uint8_t spr_rowbuf[SPRITE_W_MAX];
#define stream_buf spr_rowbuf               /* a decoded row (streams, affine sprites, scenery) */
static Item items[MAX_ITEMS];
static Aff affs[MAX_AFF];
static int nitems, naffs;
static uint16_t band[VIEW_W * BAND];
static uint16_t clear_color;
/* the stage rows on screen: the doodle's 180 (0 to 180), or, on the island,
 * the whole screen (-30 to 210) */
static int view_top = 0, view_bottom = VIEW_H;
static bool overlay;                /* drawing an overlay (gfx_overlay) */
void gfx_overlay(bool on) { overlay = on; }

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) { return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3); }

static inline uint16_t blend(uint16_t fg, uint16_t bg, unsigned a) {   /* a: 0..32 */
  uint32_t f = (fg | (uint32_t)fg << 16) & 0x07E0F81Fu, b = (bg | (uint32_t)bg << 16) & 0x07E0F81Fu;
  uint32_t r = (b + (((f - b) * a) >> 5)) & 0x07E0F81Fu;
  return (uint16_t)(r | r >> 16);
}

void gfx_begin(void) {
  nitems = 0;
  naffs = 0;
}

void gfx_clear_color(uint16_t c) { clear_color = c; }

static Item *add(void) { return nitems < MAX_ITEMS ? &items[nitems++] : NULL; }

static bool axis_aligned(Mat m) {
  return fabsf(m.b) < 1e-4f && fabsf(m.c) < 1e-4f && fabsf(fabsf(m.a) - 1) < 1e-3f && fabsf(fabsf(m.d) - 1) < 1e-3f;
}

/* bounding box of the w x h rectangle placed by m */
static void bbox(Mat m, float w, float h, int *x0, int *y0, int *x1, int *y1) {
  float xs[4] = {m.tx, m.tx + m.a * w, m.tx + m.c * h, m.tx + m.a * w + m.c * h};
  float ys[4] = {m.ty, m.ty + m.b * w, m.ty + m.d * h, m.ty + m.b * w + m.d * h};
  float lx = xs[0], hx = xs[0], ly = ys[0], hy = ys[0];
  for (int i = 1; i < 4; i++) {
    lx = xs[i] < lx ? xs[i] : lx;
    hx = xs[i] > hx ? xs[i] : hx;
    ly = ys[i] < ly ? ys[i] : ly;
    hy = ys[i] > hy ? ys[i] : hy;
  }
  *x0 = (int)floorf(lx); *y0 = (int)floorf(ly); *x1 = (int)ceilf(hx); *y1 = (int)ceilf(hy);
}

static bool on_view(int x0, int y0, int x1, int y1) { return x1 > 0 && y1 > view_top && x0 < VIEW_W && y0 < view_bottom && x1 > x0 && y1 > y0; }

static bool set_affine(Item *it, Mat m) {
  float det = m.a * m.d - m.b * m.c;
  if (fabsf(det) < 1e-6f || naffs >= MAX_AFF) return false;
  Aff *f = &affs[naffs];
  f->m = m;
  it->ref = (uint16_t)naffs++;
  return true;
}

void gfx_sprite(uint16_t sp, Mat m, uint8_t alpha) { gfx_sprite_ex(sp, m, alpha, false); }

void gfx_sprite_ex(uint16_t sp, Mat m, uint8_t alpha, bool opaque_only) {
  if (sp >= SPRITE_COUNT || !alpha) return;
  Sprite s;
  sprite_info(sp, &s);
  int x0, y0, x1, y1;
  bbox(m, s.w, s.h, &x0, &y0, &x1, &y1);
  if (!on_view(x0, y0, x1, y1)) return;
  Item *it = add();
  if (!it) return;
  *it = (Item){DI_SPRITE, alpha, 0, 0, (int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, sp, 0};
  if (axis_aligned(m)) {
    if (s.kind == 1 && s.rle > STREAM_OVER && m.d > 0) it->kind = DI_STREAM;
    /* the canvas snaps unrotated images to whole pixels */
    int x = (int)floorf((m.a < 0 ? m.tx - s.w : m.tx) + 0.5f), y = (int)floorf((m.d < 0 ? m.ty - s.h : m.ty) + 0.5f);
    it->x0 = (int16_t)x; it->y0 = (int16_t)y; it->x1 = (int16_t)(x + s.w); it->y1 = (int16_t)(y + s.h);
    it->flags = (uint8_t)((m.a < 0 ? DF_FLIPX : 0) | (m.d < 0 ? DF_FLIPY : 0) | (opaque_only ? DF_OPAQUE : 0) | (overlay ? DF_OVERLAY : 0));
    it->color = sp;
  } else {
    it->kind = DI_AFFINE;
    it->color = sp;
    it->flags = (uint8_t)((opaque_only ? DF_OPAQUE : 0) | (overlay ? DF_OVERLAY : 0));
    if (!set_affine(it, m)) nitems--;
  }
}

void gfx_rect(int x, int y, int w, int h, uint16_t c, uint8_t alpha) {
  if (!on_view(x, y, x + w, y + h) || !alpha) return;
  Item *it = add();
  if (it) *it = (Item){DI_RECT, alpha, 0, 0, (int16_t)x, (int16_t)y, (int16_t)(x + w), (int16_t)(y + h), 0, c};
}

/* one pattern a frame: a small 4-bit image at n places (gfx_tiles) */
static struct { const uint8_t *px, *pts; const uint16_t *pal; const uint8_t *al; const int16_t *pos; int n, w, h, ox, oy; } tl;
void gfx_tiles(const uint8_t *px, int w, int h, const uint16_t *pal, const uint8_t *al, const int16_t *pos, int n, int ox, int oy, uint8_t alpha) {
  if (!alpha || n <= 0 || w <= 0 || h <= 0) return;
  Item *it = add();
  if (!it) return;
  tl.px = px; tl.pts = NULL; tl.pal = pal; tl.al = al; tl.pos = pos; tl.n = n; tl.w = w; tl.h = h; tl.ox = ox; tl.oy = oy;
  *it = (Item){DI_TILES, alpha, 0, 0, 0, (int16_t)view_top, VIEW_W, (int16_t)view_bottom, (uint16_t)(uintptr_t)px, (uint16_t)((ox & 0xFF) | (oy & 0xFF) << 8)};
}

void gfx_points(const uint8_t *pts, int w, int h, const uint16_t *pal, const uint8_t *al, const int16_t *pos, int n, int ox, int oy, uint8_t alpha) {
  gfx_tiles(pts, w, h, pal, al, pos, n, ox, oy, alpha);
  if (nitems && items[nitems - 1].kind == DI_TILES) { tl.px = NULL; tl.pts = pts; }
}

void gfx_shape(const uint8_t *shape, Mat m, uint8_t alpha) {
  if (!alpha || !shape[0]) return;
  /* bounds from the points */
  float lx = 1e9f, ly = 1e9f, hx = -1e9f, hy = -1e9f;
  const uint8_t *p = shape + 1;
  bool visible = false;
  for (int s = 0; s < shape[0]; s++) {
    if (p[3]) visible = true;
    int npoly = p[4];
    p += 5;
    for (int k = 0; k < npoly; k++) {
      int n = rd16(p);
      p += 2;
      for (int i = 0; i < n; i++, p += 4) {
        float px = rds16(p) * 0.25f, py = rds16(p + 2) * 0.25f;
        float X = m.a * px + m.c * py + m.tx, Y = m.b * px + m.d * py + m.ty;
        lx = X < lx ? X : lx;
        hx = X > hx ? X : hx;
        ly = Y < ly ? Y : ly;
        hy = Y > hy ? Y : hy;
      }
    }
  }
  if (!visible) return;
  /* an overlay's backdrop over the whole stage (a menu's shade) covers the
   * whole view when it is taller than the stage */
  if (overlay && (view_top < 0 || view_bottom > VIEW_H) && lx <= 0 && ly <= 0 && hx >= VIEW_W && hy >= VIEW_H) {
    float k = (float)(view_bottom - view_top) / VIEW_H;
    m = (Mat){m.a, m.b * k, m.c, m.d * k, m.tx, m.ty * k + (float)view_top};
    ly = ly * k + (float)view_top;
    hy = hy * k + (float)view_top;
  }
  int x0 = (int)floorf(lx), y0 = (int)floorf(ly), x1 = (int)ceilf(hx), y1 = (int)ceilf(hy);
  if (!on_view(x0, y0, x1, y1)) return;
  Item *it = add();
  if (!it) return;
  *it = (Item){DI_SHAPE, alpha, 0, 0, (int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, 0, 0};
  if (!set_affine(it, m)) nitems--;
  else item_ptr[it->ref] = shape;
}

void gfx_text(const char *s, Mat m, uint16_t color, uint8_t align, int16_t lw, int16_t lh, uint8_t alpha) { gfx_text_k(s, m, color, align, lw, lh, alpha, 1); }

void gfx_text_k(const char *s, Mat m, uint16_t color, uint8_t align, int16_t lw, int16_t lh, uint8_t alpha, int k) {
  gfx_text_k3(s, m, color, align, lw, lh, alpha, 3 * k);
}

void gfx_text_k3(const char *s, Mat m, uint16_t color, uint8_t align, int16_t lw, int16_t lh, uint8_t alpha, int k3) {
  if (!s || !*s || !alpha) return;
  int w, h;
  font_scale3(k3);
  font_measure(s, lw, lh, &w, &h);
  font_scale(1);
  int ox = align == 1 ? -w / 2 : align == 2 ? -w : 0;
  int x0 = (int)floorf(m.tx) + ox, y0 = (int)floorf(m.ty);
  if (!on_view(x0, y0, x0 + w, y0 + h)) return;
  Item *it = add();
  if (!it) return;
  *it = (Item){DI_TEXT, alpha, 0, 0, (int16_t)x0, (int16_t)y0, (int16_t)(x0 + w), (int16_t)(y0 + h), 0, color};
  if (naffs < MAX_AFF) {
    item_ptr[naffs] = s;
    affs[naffs].align = align;
    affs[naffs].lw = lw;
    affs[naffs].lh = lh;
    affs[naffs].m = m;
    affs[naffs].scale = (uint8_t)k3;
    it->ref = (uint16_t)naffs++;
  } else nitems--;
}

/* ---------------------------------------------------------------- the background layer */
static uint8_t *bgmem;
static int bgw, bgh, bgx, bgy;      /* layer size; world position of the area it holds */
static bool bg_valid;
static uint8_t bgsheet;
static BgPaint bgpaint;
static int bg_cx, bg_cy;            /* camera */
static int clip_x0, clip_y0, clip_x1, clip_y1;   /* world rect being painted */
static uint32_t bg_gen;             /* changes whenever the layer's pixels do */
static int bg_see;                  /* palette index that shows the water (-1: none) */
static const uint8_t *water;        /* the water: a tile of palette indices, repeating from world (0, 0) */
static const uint16_t *water_cols;  /* or of 4-bit indices into these colours */
static int water_w, water_h;
static const uint8_t *bglut;        /* RGB444 -> palette index, to blend into the layer */
/* the see-through entries of the layer's palette: a pixel partly over the
 * water keeps the nearest one, and is blended over the water when drawn */
static uint8_t seethru[32], nseethru;

void bg_setup(uint8_t *mem, int w, int h, uint8_t sheet, BgPaint paint) {
  bgmem = mem;
  bgw = w;
  bgh = h;
  bgsheet = sheet;
  bgpaint = paint;
  bg_valid = false;
  bg_see = -1;
  water = NULL;
  water_cols = NULL;
  bglut = NULL;
  bg_gen++;
  const uint8_t *al = palalpha(sheet);
  nseethru = 0;
  for (int i = 0; i < 256; i++) {
    if (!al[i] && bg_see < 0) bg_see = i;
    if (al[i] && al[i] != 255 && nseethru < (int)sizeof seethru) seethru[nseethru++] = (uint8_t)i;
  }
}

void bg_off(void) { bgmem = NULL; bg_gen++; }
void bg_invalidate(void) { bg_valid = false; }
uint8_t *bg_layer(int *w, int *h) { *w = bgw; *h = bgh; return bgmem; }
static uint32_t water_gen;
void bg_water(const uint8_t *tile, int w, int h) { water = tile; water_cols = NULL; water_w = w; water_h = h; water_gen++; }
void bg_water4(const uint8_t *tile, int w, int h, const uint16_t *cols) { bg_water(tile, w, h); water_cols = cols; }
void bg_redraw_water(void) { water_gen++; }
void bg_blend_lut(const uint8_t *lut) { bglut = lut; }
int bg_clear_index(void) { return bg_see; }

/* layer pixel for world (x, y): the layer is a torus */
static inline uint8_t *bg_at(int x, int y) {
  int lx = x % bgw, ly = y % bgh;
  if (lx < 0) lx += bgw;
  if (ly < 0) ly += bgh;
  return bgmem + ly * bgw + lx;
}

static void bg_paint_rect(int x0, int y0, int x1, int y1) {
  if (x1 <= x0 || y1 <= y0) return;
  bg_gen++;
  clip_x0 = x0; clip_y0 = y0; clip_x1 = x1; clip_y1 = y1;
  uint8_t clear = bg_see >= 0 ? (uint8_t)bg_see : 0;
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++) *bg_at(x, y) = clear;
  bgpaint(x0, y0, x1 - x0, y1 - y0);
}

void bg_camera(int cx, int cy) {
  if (!bgmem) return;
  bg_cx = cx;
  bg_cy = cy;
  cy += view_top;                     /* the top row on screen */
  int vh = view_bottom - view_top;
  if (!bg_valid || cx < bgx - bgw || cx > bgx + 2 * bgw || cy < bgy - bgh || cy > bgy + 2 * bgh) {
    /* center the spare margin around the view */
    bgx = cx - (bgw - VIEW_W) / 2;
    bgy = cy - (bgh - vh) / 2;
    bg_paint_rect(bgx, bgy, bgx + bgw, bgy + bgh);
    bg_valid = true;
    return;
  }
  /* the view left the layer: move the layer so that the whole margin lies
   * ahead, and paint what it newly covers (a strip at a time) */
  if (cx < bgx || cx + VIEW_W > bgx + bgw) {
    int nx = cx < bgx ? cx + VIEW_W - bgw : cx;
    if (nx > bgx) bg_paint_rect(bgx + bgw > nx ? bgx + bgw : nx, bgy, nx + bgw, bgy + bgh);
    else bg_paint_rect(nx, bgy, bgx < nx + bgw ? bgx : nx + bgw, bgy + bgh);
    bgx = nx;
  }
  if (cy < bgy || cy + vh > bgy + bgh) {
    int ny = cy < bgy ? cy + vh - bgh : cy;
    if (ny > bgy) bg_paint_rect(bgx, bgy + bgh > ny ? bgy + bgh : ny, bgx + bgw, ny + bgh);
    else bg_paint_rect(bgx, ny, bgx + bgw, bgy < ny + bgh ? bgy : ny + bgh);
    bgy = ny;
  }
}

/* scenery standing in front of the actors: the layer's own pixels again where
 * the sprite drawn at world (x, y) is opaque (its mask), so only the mask needs
 * to be in the cache, and what the layer has over it stays over it */
void bg_redraw(uint16_t sp, int x, int y, uint8_t flags) {
  if (!bgmem || sp >= SPRITE_COUNT) return;
  Sprite s;
  sprite_info(sp, &s);
  int x0 = x - bg_cx, y0 = y - bg_cy;
  if (!on_view(x0, y0, x0 + s.w, y0 + s.h)) return;
  Item *it = add();
  if (it) *it = (Item){DI_MASK, 255, (uint8_t)((flags & BD_FLIPX ? DF_FLIPX : 0) | (flags & BD_FLIPY ? DF_FLIPY : 0)), 0,
                       (int16_t)x0, (int16_t)y0, (int16_t)(x0 + s.w), (int16_t)(y0 + s.h), sp, 0};
}

void bg_fill(int x, int y, int w, int h, uint8_t index) {
  int x0 = x > clip_x0 ? x : clip_x0, y0 = y > clip_y0 ? y : clip_y0;
  int x1 = x + w < clip_x1 ? x + w : clip_x1, y1 = y + h < clip_y1 ? y + h : clip_y1;
  for (int yy = y0; yy < y1; yy++)
    for (int xx = x0; xx < x1; xx++) *bg_at(xx, yy) = index;
}

/* one pixel of a sprite (palette of `sheet`) onto the layer, with alpha 0..255 */

static uint8_t nearest_seethru(uint16_t col, unsigned a) {
  const uint16_t *pal = pal565(bgsheet);
  const uint8_t *al = palalpha(bgsheet);
  int best = -1;
  unsigned bd = 0;
  for (int i = 0; i < nseethru; i++) {
    uint16_t p = pal[seethru[i]];
    int dr = (int)(p >> 11) - (int)(col >> 11), dg = (int)(p >> 5 & 63) - (int)(col >> 5 & 63), db = (int)(p & 31) - (int)(col & 31);
    int da = (int)al[seethru[i]] - (int)a;
    unsigned dd = (unsigned)(4 * dr * dr + dg * dg + 4 * db * db) + (unsigned)(da * da) / 16;
    if (best < 0 || dd < bd) { best = seethru[i]; bd = dd; }
  }
  return (uint8_t)(best < 0 ? bg_see : best);
}

static inline void bg_put(uint8_t *d, uint8_t c, const uint16_t *spal, const uint8_t *sal, uint8_t sheet, unsigned alpha) {
  unsigned a = sal[c] * alpha;          /* 0..65025 */
  if (!a) return;
  if (a >= 255 * 255 - 255 && sheet == bgsheet) { *d = c; return; }
  if (!bglut) { if (a >= 128 * 255 && sheet == bgsheet) *d = c; return; }
  uint16_t fg = spal[c];
  uint16_t col = fg;
  if (a < 255 * 255 - 255) {
    const uint8_t *al = palalpha(bgsheet);
    if (water && al[*d] != 255) {
      /* over the water (or over something see-through): stays see-through */
      unsigned a8 = (a + 127) / 255;
      if (al[*d]) {
        a8 = a8 + al[*d] * (255 - a8) / 255;
        col = blend(fg, pal565(bgsheet)[*d], (unsigned)((a + 1024) >> 11) * 255 / (a8 ? a8 : 1) > 32 ? 32 : (unsigned)(((a + 1024) >> 11) * 255 / (a8 ? a8 : 1)));
      }
      *d = a8 >= 250 ? bglut[(col >> 12 & 15) << 8 | (col >> 7 & 15) << 4 | (col >> 1 & 15)] : nearest_seethru(col, a8);
      return;
    }
    col = blend(fg, pal565(bgsheet)[*d], (a + 1024) >> 11);
  }
  *d = bglut[(col >> 12 & 15) << 8 | (col >> 7 & 15) << 4 | (col >> 1 & 15)];
}

void bg_blit_sprite(uint16_t sp, int x, int y, bool flipx, bool flipy) {
  bg_draw(sp, x, y, (flipx ? BD_FLIPX : 0) | (flipy ? BD_FLIPY : 0), 255);
}

/* a big image kept as columns (see pack.py): each column band is its own
 * stream, decoded from its top down to the last row needed */
static void bg_draw_banded(uint16_t sp, const Sprite *s, int x, int y, uint8_t flags, uint8_t alpha) {
  const uint8_t *t = ci_data + s->off;
  unsigned nb = rd16(t);
  const uint16_t *spal = pal565(s->sheet);
  const uint8_t *sal = palalpha(s->sheet);
  bool fx = flags & BD_FLIPX, fy = flags & BD_FLIPY;
  /* rows needed, in the image's own rows */
  int r0 = fy ? y + s->h - clip_y1 : clip_y0 - y, r1 = fy ? y + s->h - clip_y0 : clip_y1 - y;
  if (r0 < 0) r0 = 0;
  if (r1 > s->h) r1 = s->h;
  if (r0 >= r1) return;
  for (unsigned b = 0; b < nb; b++) {
    int u0 = (int)b * 128, u1 = u0 + 128 > s->w ? s->w : u0 + 128, bw = u1 - u0;
    /* world columns of this band */
    int wx0 = fx ? x + s->w - u1 : x + u0, wx1 = wx0 + bw;
    if (wx1 <= clip_x0 || wx0 >= clip_x1) continue;
    z_open(rd32(t + 2 + 8 * b), rd32(t + 6 + 8 * b), (uint32_t)bw * s->h);
    if (r0) z_get(NULL, (uint32_t)r0 * bw);
    for (int r = r0; r < r1; r++) {
      if (!z_get(stream_buf, (uint32_t)bw)) return;
      int wy = fy ? y + s->h - 1 - r : y + r;
      uint8_t *row = bg_at(0, wy) - (bg_at(0, wy) - bgmem) % bgw;   /* the layer row */
      for (int i = 0; i < bw; i++) {
        int wx = fx ? wx1 - 1 - i : wx0 + i;
        if (wx < clip_x0 || wx >= clip_x1) continue;
        int lx = wx % bgw;
        if (lx < 0) lx += bgw;
        bg_put(row + lx, stream_buf[i], spal, sal, s->sheet, alpha);
      }
    }
  }
}

/* bg_draw from the decoder's rows (the sprite is not in the cache) */
static void bg_draw_rows(uint16_t sp, const Sprite *s, int x, int y, uint8_t flags, uint8_t alpha) {
#ifdef HOST
  if (getenv("CI_SPR_MISS")) fprintf(stderr, "paintrows %u %dx%d rle %u\n", sp, s->w, s->h, (unsigned)s->rle);
#endif
  if (!spr_rows_open(sp, s)) return;
  const uint16_t *spal = pal565(s->sheet);
  const uint8_t *sal = palalpha(s->sheet);
  bool tr = flags & BD_TRANSPOSE, fx = flags & BD_FLIPX, fy = flags & BD_FLIPY;
  int dw = tr ? s->h : s->w, dh = tr ? s->w : s->h;
  for (int v = 0; v < s->h; v++) {
    /* the row's place across the painted strip (a column when transposed) */
    int rv = tr ? (fx ? dw - 1 - v : v) : (fy ? dh - 1 - v : v);
    int at = tr ? x + rv : y + rv, lo = tr ? clip_x0 : clip_y0, hi = tr ? clip_x1 : clip_y1;
    bool back = tr ? fx : fy;                       /* rows go the other way */
    if ((at >= hi && !back) || (at < lo && back)) return;   /* the rest is past the strip */
    bool skip = at < lo || at >= hi;
    if (!z_get(skip ? NULL : stream_buf, s->w)) return;
    if (skip) continue;
    for (int u = 0; u < s->w; u++) {
      uint8_t c = stream_buf[u];
      if (!sal[c]) continue;
      int di = tr ? v : u, dj = tr ? u : v;
      if (fx) di = dw - 1 - di;
      if (fy) dj = dh - 1 - dj;
      int wx = x + di, wy = y + dj;
      if (wx < clip_x0 || wx >= clip_x1 || wy < clip_y0 || wy >= clip_y1) continue;
      bg_put(bg_at(wx, wy), c, spal, sal, s->sheet, alpha);
    }
  }
}

void bg_draw(uint16_t sp, int x, int y, uint8_t flags, uint8_t alpha) {
  if (sp >= SPRITE_COUNT || !bgmem) return;
  Sprite s;
  sprite_info(sp, &s);
  bool tr = flags & BD_TRANSPOSE;
  int dw = tr ? s.h : s.w, dh = tr ? s.w : s.h;
  if (x >= clip_x1 || y >= clip_y1 || x + dw <= clip_x0 || y + dh <= clip_y0 || !alpha) return;
  if (s.kind == 2) { if (!tr) bg_draw_banded(sp, &s, x, y, flags, alpha); return; }
  if (s.kind == 1 && !flags) { bg_blit_stream(sp, x, y); return; }
  const uint8_t *r = spr_peek(sp);
  /* big scenery the cache has no room for (without dropping what the
   * frames draw) goes straight from the decoder into the layer */
  if (!r && !spr_room(s.rle ? s.rle : (uint32_t)s.w * s.h)) { bg_draw_rows(sp, &s, x, y, flags, alpha); return; }
  if (!r) r = spr_get(sp);
  if (!r) return;
  const uint16_t *spal = pal565(s.sheet);
  const uint8_t *sal = palalpha(s.sheet);
  bool fx = flags & BD_FLIPX, fy = flags & BD_FLIPY;
  int w = rd16(r), h = rd16(r + 2);
  for (int v = 0; v < h; v++) {
    /* a row out of the painted strip is skipped whole, a run too */
    int rv = tr ? (fx ? dw - 1 - v : v) : (fy ? dh - 1 - v : v);
    if (tr ? (x + rv < clip_x0 || x + rv >= clip_x1) : (y + rv < clip_y0 || y + rv >= clip_y1)) continue;
    const uint8_t *p = r + rd16(r + 4 + 2 * v);
    int n = rd16(p), u = 0;
    p += 2;
    for (int k = 0; k < n; k++) {
      u += *p++;
      int code = *p++, len = code & 0x80 ? (code & 0x7F) + 1 : code;
      bool fill = code & 0x80;
      int f = fx && !tr ? dw - u - len : fy && tr ? dh - u - len : u;   /* the run's first cell along the row */
      int lo = tr ? y + f : x + f, c0 = tr ? clip_y0 : clip_x0, c1 = tr ? clip_y1 : clip_x1;
      if (lo >= c1 || lo + len <= c0) { u += len; p += fill ? 1 : len; continue; }
      for (int i = 0; i < len; i++, u++) {
        /* source (u, v) -> offset (i, j) in the drawn rectangle */
        int di = tr ? v : u, dj = tr ? u : v;
        if (fx) di = dw - 1 - di;
        if (fy) dj = dh - 1 - dj;
        int wx = x + di, wy = y + dj;
        if (wx < clip_x0 || wx >= clip_x1 || wy < clip_y0 || wy >= clip_y1) continue;
        bg_put(bg_at(wx, wy), fill ? p[0] : p[i], spal, sal, s.sheet, alpha);
      }
      p += fill ? 1 : len;
    }
  }
  (void)w;
}

void bg_blit_stream(uint16_t sp, int x, int y) {
  Sprite s;
  sprite_info(sp, &s);
  if (x >= clip_x1 || y >= clip_y1 || x + s.w <= clip_x0 || y + s.h <= clip_y0) return;
  const uint8_t *st = spr_stream(sp);
  if (!st) { bg_blit_sprite(sp, x, y, false, false); return; }
  const uint8_t *alpha = palalpha(s.sheet);
  z_open(rd32(st + 2), rd32(st + 6), rd32(st + 10));
  int r0 = clip_y0 - y, r1 = clip_y1 - y;
  if (r0 < 0) r0 = 0;
  if (r1 > s.h) r1 = s.h;
  z_get(NULL, (uint32_t)r0 * s.w);
  int c0 = clip_x0 - x, c1 = clip_x1 - x;
  if (c0 < 0) c0 = 0;
  if (c1 > s.w) c1 = s.w;
  for (int row = r0; row < r1; row++) {
    /* the part of the row that is needed, straight from the decoder */
    uint32_t done = 0;
    while (done < s.w) {
      const uint8_t *p;
      uint32_t got = z_read(s.w - done, &p);
      if (!got) return;
      for (uint32_t i = 0; i < got; i++) {
        int sx = (int)(done + i);
        if (sx >= c0 && sx < c1 && alpha[p[i]]) *bg_at(x + sx, y + row) = p[i];
      }
      done += got;
    }
  }
}

/* ---------------------------------------------------------------- rasterising */
static void band_bg(int by, int rows) {
  if (!bgmem) {
    for (int i = 0; i < VIEW_W * rows; i++) band[i] = clear_color;
    return;
  }
  const uint16_t *pal = pal565(bgsheet);
  for (int r = 0; r < rows; r++) {
    uint16_t *d = band + r * VIEW_W;
    int wy = bg_cy + by + r;
    int ly = wy % bgh;
    if (ly < 0) ly += bgh;
    const uint8_t *row = bgmem + ly * bgw;
    int lx = bg_cx % bgw;
    if (lx < 0) lx += bgw;
    if (water && bg_see >= 0) {
      /* where the layer is clear, the water shows */
      int ty = wy % water_h;
      if (ty < 0) ty += water_h;
      int tx = bg_cx % water_w;
      if (tx < 0) tx += water_w;
      const uint8_t *al = palalpha(bgsheet);
      if (water_cols) {
        const uint8_t *wrow = water + ty * (water_w >> 1);
        for (int x = 0; x < VIEW_W; x++) {
          uint8_t c = row[lx];
          unsigned a = al[c];
          if (a == 255) d[x] = pal[c];
          else {
            uint16_t wc = water_cols[wrow[tx >> 1] >> ((tx & 1) << 2) & 15];
            d[x] = !a ? wc : blend(pal[c], wc, (a + 4) >> 3);
          }
          if (++lx == bgw) lx = 0;
          if (++tx == water_w) tx = 0;
        }
      } else {
        const uint8_t *wrow = water + ty * water_w;
        for (int x = 0; x < VIEW_W; x++) {
          uint8_t c = row[lx];
          unsigned a = al[c];
          d[x] = a == 255 ? pal[c] : !a ? pal[wrow[tx]] : blend(pal[c], pal[wrow[tx]], (a + 4) >> 3);
          if (++lx == bgw) lx = 0;
          if (++tx == water_w) tx = 0;
        }
      }
    } else {
      for (int x = 0; x < VIEW_W; x++) {
        d[x] = pal[row[lx]];
        if (++lx == bgw) lx = 0;
      }
    }
  }
}

static void draw_rle_row(const uint8_t *p, int x, int w, bool flipx, uint16_t *d, const uint16_t *pal,
                         const uint8_t *al, unsigned galpha) {
  int n = rd16(p), cx = 0;
  p += 2;
  for (int k = 0; k < n; k++) {
    cx += *p++;
    int code = *p++;
    bool fill = code & 0x80;
    int len = fill ? (code & 0x7F) + 1 : code;
    const uint8_t *src = p;
    p += fill ? 1 : len;
    int start = cx;
    cx += len;
    int i0 = 0, i1 = len;
    uint16_t *dd = d;
    int step, o;                      /* dd[o + i * step]: no negative index (UBSan's object-size check) */
    if (!flipx) {
      int sx = x + start;               /* screen x of the run's first pixel */
      if (sx >= VIEW_W) break;
      if (sx < 0) i0 = -sx;
      if (sx + len > VIEW_W) i1 = VIEW_W - sx;
      o = sx;
      step = 1;
    } else {
      int base = x + w - 1 - start;     /* screen x of the run's first pixel; the run goes left */
      if (base < 0) break;
      if (base >= VIEW_W) i0 = base - VIEW_W + 1;
      if (base - (len - 1) < 0) i1 = base + 1;
      o = base;
      step = -1;
    }
    if (i0 >= i1) continue;
    if (fill) {
      unsigned c = src[0], a = al[c];
      if (!a) continue;
      uint16_t col = pal[c];
      if (a == 255 && galpha == 32) {
        for (int i = i0; i < i1; i++) dd[o + i * step] = col;
      } else {
        unsigned aa = (a * galpha + 128) >> 8;
        for (int i = i0; i < i1; i++) dd[o + i * step] = blend(col, dd[o + i * step], aa);
      }
    } else if (galpha == 32) {
      for (int i = i0; i < i1; i++) {
        unsigned c = src[i], a = al[c];
        dd[o + i * step] = a == 255 ? pal[c] : blend(pal[c], dd[o + i * step], (a + 4) >> 3);
      }
    } else {
      for (int i = i0; i < i1; i++) {
        unsigned c = src[i];
        dd[o + i * step] = blend(pal[c], dd[o + i * step], (al[c] * galpha + 128) >> 8);
      }
    }
  }
}

/* the one big sprite of the frame decoded as the bands go down */
static const Item *streaming;
static int stream_row;

/* affine sprites sample the one source row they last decoded (into stream_buf) */
static const uint8_t *line_spr;
static int line_v;
static uint8_t line_mask[(SPRITE_W_MAX + 7) / 8];
static void line_decode(const uint8_t *r, int v, int w) {
  memset(line_mask, 0, (size_t)(w + 7) / 8);
  const uint8_t *p = r + rd16(r + 4 + 2 * v);
  int n = rd16(p), cx = 0;
  p += 2;
  for (int k = 0; k < n; k++) {
    cx += *p++;
    int code = *p++;
    bool fill = code & 0x80;
    int len = fill ? (code & 0x7F) + 1 : code;
    for (int i = 0; i < len && cx + i < w; i++) {
      stream_buf[cx + i] = fill ? p[0] : p[i];
      line_mask[(cx + i) >> 3] |= (uint8_t)(1 << ((cx + i) & 7));
    }
    p += fill ? 1 : len;
    cx += len;
  }
  line_spr = r;
  line_v = v;
}

static void stream_begin(const Item *it) {
  Sprite s;
  sprite_info(it->ref, &s);
  streaming = spr_rows_open(it->ref, &s) ? it : NULL;
  stream_row = 0;
}

static void draw_stream(const Item *it, int y0, int y1, int by, unsigned ga) {
  line_spr = NULL;   /* stream_buf is overwritten */
  Sprite s;
  sprite_info(it->ref, &s);
  const uint16_t *pal = pal565(s.sheet);
  const uint8_t *al = palalpha(s.sheet);
  for (int y = y0; y < y1; y++) {
    int row = y - it->y0;
    if (row < stream_row) continue;
    if (row > stream_row) z_get(NULL, (uint32_t)(row - stream_row) * s.w);
    if (!z_get(stream_buf, s.w)) return;
    stream_row = row + 1;
    uint16_t *d = band + (y - by) * VIEW_W;
    bool fx = it->flags & DF_FLIPX;
    for (int i = 0; i < s.w; i++) {
      uint8_t c = stream_buf[i];
      unsigned a = al[c];
      if (!a) continue;
      int sx = fx ? it->x0 + s.w - 1 - i : it->x0 + i;
      if ((unsigned)sx >= VIEW_W) continue;
      d[sx] = (a == 255 && ga == 32) ? pal[c] : blend(pal[c], d[sx], (a * ga + 128) >> 8);
    }
  }
}

/* A sprite the cache let go before its band (a frame needing more than the
 * cache holds, such as a menu over the island): decoded now, unless a stream
 * holds the decoder or the menu comes first (then it comes next frame). */
static bool scene_later;   /* a menu's frame too big for the cache: the scene's sprites that do not fit wait */
static const uint8_t *band_sprite(uint16_t sp, uint8_t flags) {
  const uint8_t *r = spr_peek(sp);
  if (r || streaming || (scene_later && !(flags & DF_OVERLAY))) return r;
  r = spr_get(sp);
  line_spr = NULL;   /* the cache may have moved */
  return r;
}

static __attribute__((noinline)) void draw_sprite_item(const Item *it, int y0, int y1, int by, int rows, unsigned ga) {
  const uint8_t *r = band_sprite(it->ref, it->flags);
  if (!r) return;
  Sprite s;
  sprite_info(it->ref, &s);
  const uint16_t *pal = pal565(s.sheet);
  const uint8_t *al = palalpha(s.sheet);
  int h = rd16(r + 2), w = rd16(r);
  uint8_t al_opaque[256];             /* for DF_OPAQUE: 255 stays, the rest are skipped */
  if (it->flags & DF_OPAQUE) {
    for (int i = 0; i < 256; i++) al_opaque[i] = al[i] == 255 ? 255 : 0;
    al = al_opaque;
  }
  for (int y = y0; y < y1; y++) {
    int row = y - it->y0;
    if (it->flags & DF_FLIPY) row = h - 1 - row;
    draw_rle_row(r + rd16(r + 4 + 2 * row), it->x0, w, it->flags & DF_FLIPX, band + (y - by) * VIEW_W, pal, al, ga);
  }
}

static __attribute__((noinline)) void draw_affine_item(const Item *it, int y0, int y1, int by, int rows, unsigned ga) {
  const uint8_t *r = band_sprite(it->color, it->flags);
  if (!r) return;
  Sprite s;
  sprite_info(it->color, &s);
  const uint16_t *pal = pal565(s.sheet);
  const uint8_t *al = palalpha(s.sheet);
  const Mat *m = &affs[it->ref].m;
  float det = m->a * m->d - m->b * m->c;
  float ia = m->d / det, ib = -m->b / det, ic = -m->c / det, id = m->a / det;
  float itx = -(ia * m->tx + ic * m->ty), ity = -(ib * m->tx + id * m->ty);
  int w = rd16(r), h = rd16(r + 2);
  int x0 = it->x0 < 0 ? 0 : it->x0, x1 = it->x1 > VIEW_W ? VIEW_W : it->x1;
  if (fabsf(ib) > fabsf(ia)) {
    /* turned about 90 degrees: a screen column walks one source row, so go
     * column by column; a band needs a few neighbouring pixels of that row,
     * read from its runs into a small window (no row decode per pixel) */
    int wv = -1, wu0 = 0;
    int16_t win[8];
    for (int x = x0; x < x1; x++) {
      float fu = ia * (x + 0.5f) + ic * (y0 + 0.5f) + itx, fv = ib * (x + 0.5f) + id * (y0 + 0.5f) + ity;
      for (int y = y0; y < y1; y++, fu += ic, fv += id) {
        if (fu < 0 || fv < 0 || fu >= w || fv >= h) continue;
        int v = (int)fv, u = (int)fu;
        if (v != wv || u < wu0 || u >= wu0 + 8) {
          wv = v;
          wu0 = ic < 0 ? u - 7 : u;
          for (int i = 0; i < 8; i++) win[i] = -1;
          const uint8_t *p = r + rd16(r + 4 + 2 * v);
          int n = rd16(p), cx = 0;
          p += 2;
          for (int k = 0; k < n && cx < wu0 + 8; k++) {
            cx += *p++;
            int code = *p++, len = code & 0x80 ? (code & 0x7F) + 1 : code;
            for (int i = cx > wu0 ? cx : wu0; i < cx + len && i < wu0 + 8; i++) win[i - wu0] = code & 0x80 ? p[0] : p[i - cx];
            p += code & 0x80 ? 1 : len;
            cx += len;
          }
        }
        int c = win[u - wu0];
        if (c < 0) continue;
        uint16_t *d = band + (y - by) * VIEW_W + x;
        unsigned a = al[c];
        *d = (a == 255 && ga == 32) ? pal[c] : blend(pal[c], *d, (a * ga + 128) >> 8);
      }
    }
    return;
  }
  for (int y = y0; y < y1; y++) {
    uint16_t *d = band + (y - by) * VIEW_W;
    float fu = ia * (x0 + 0.5f) + ic * (y + 0.5f) + itx, fv = ib * (x0 + 0.5f) + id * (y + 0.5f) + ity;
    for (int x = x0; x < x1; x++, fu += ia, fv += ib) {
      if (fu < 0 || fv < 0 || fu >= w || fv >= h) continue;
      int v = (int)fv, u = (int)fu;
      if (r != line_spr || v != line_v) line_decode(r, v, w);
      if (!(line_mask[u >> 3] & (1 << (u & 7)))) continue;
      unsigned c = stream_buf[u], a = al[c];
      d[x] = (a == 255 && ga == 32) ? pal[c] : blend(pal[c], d[x], (a * ga + 128) >> 8);
    }
  }
}

static __attribute__((noinline)) void draw_shape_item(const Item *it, int y0, int y1, int by, int rows, unsigned ga) {
  /* scanlines: where each row's pixel centres cross the polygons' edges (even-odd) */
  const Mat m = affs[it->ref].m;
  const uint8_t *sh = item_ptr[it->ref];
  float xs[64];
  for (int y = y0; y < y1; y++) {
    uint16_t *d = band + (y - by) * VIEW_W;
    float fy = y + 0.5f;
    const uint8_t *p = sh + 1;
    for (int k = 0; k < sh[0]; k++) {
      uint16_t col = rgb565(p[0], p[1], p[2]);
      unsigned a = (p[3] * ga + 128) >> 8;
      int npoly = p[4], nx = 0;
      p += 5;
      for (int q = 0; q < npoly; q++) {
        int n = rd16(p);
        p += 2;
        if (a && n) {
          float px = rds16(p + 4 * (n - 1)) * 0.25f, py = rds16(p + 4 * (n - 1) + 2) * 0.25f;
          float X0 = m.a * px + m.c * py + m.tx, Y0 = m.b * px + m.d * py + m.ty;
          for (int i = 0; i < n; i++) {
            px = rds16(p + 4 * i) * 0.25f;
            py = rds16(p + 4 * i + 2) * 0.25f;
            float X1 = m.a * px + m.c * py + m.tx, Y1 = m.b * px + m.d * py + m.ty;
            if ((Y1 > fy) != (Y0 > fy) && nx < 64) xs[nx++] = X0 + (fy - Y0) * (X1 - X0) / (Y1 - Y0);
            X0 = X1;
            Y0 = Y1;
          }
        }
        p += 4 * n;
      }
      if (nx < 2) continue;
      for (int i = 1; i < nx; i++) {
        float t = xs[i];
        int j = i;
        for (; j > 0 && xs[j - 1] > t; j--) xs[j] = xs[j - 1];
        xs[j] = t;
      }
      for (int i = 0; i + 1 < nx; i += 2) {
        int xa = (int)ceilf(xs[i] - 0.5f), xb = (int)ceilf(xs[i + 1] - 0.5f);
        if (xa < 0) xa = 0;
        if (xb > VIEW_W) xb = VIEW_W;
        for (int x = xa; x < xb; x++) d[x] = a >= 32 ? col : blend(col, d[x], a);
      }
    }
  }
}

static __attribute__((noinline)) void draw_mask_item(const Item *it, int y0, int y1, int by) {
  const uint8_t *m = spr_peek_mask(it->ref);
  if (!m && !streaming) { m = spr_mask(it->ref); line_spr = NULL; }
  if (!m || !bgmem) return;
  const uint16_t *pal = pal565(bgsheet);
  const uint8_t *al = palalpha(bgsheet);
  int w = rd16(m), h = rd16(m + 2);
  bool fx = it->flags & DF_FLIPX;
  for (int y = y0; y < y1; y++) {
    int row = y - it->y0;
    if (it->flags & DF_FLIPY) row = h - 1 - row;
    const uint8_t *p = m + rd16(m + 4 + 2 * row);
    int n = rd16(p), x = 0;
    p += 2;
    uint16_t *d = band + (y - by) * VIEW_W;
    int ly = (bg_cy + y) % bgh;
    if (ly < 0) ly += bgh;
    const uint8_t *lrow = bgmem + ly * bgw;
    for (int k = 0; k < n; k++, p += 2) {
      x += p[0];
      int a = x, b = x + p[1];
      x = b;
      if (fx) { int t = w - b; b = w - a; a = t; }
      a += it->x0;
      b += it->x0;
      if (a < 0) a = 0;
      if (b > VIEW_W) b = VIEW_W;
      int lx = (bg_cx + a) % bgw;
      if (lx < 0) lx += bgw;
      for (int sx = a; sx < b; sx++) {
        uint8_t c = lrow[lx];
        if (al[c] == 255) d[sx] = pal[c];
        if (++lx == bgw) lx = 0;
      }
    }
  }
}

static __attribute__((noinline)) void draw_tiles_item(int y0, int y1, int by, unsigned ga) {
  for (int i = 0; i < tl.n; i++) {
    int tx = tl.ox + tl.pos[2 * i], ty = tl.oy + tl.pos[2 * i + 1];
    int a = ty > y0 ? ty : y0, b = ty + tl.h < y1 ? ty + tl.h : y1;
    if (a >= b || tx >= VIEW_W || tx + tl.w <= 0) continue;
    if (tl.pts) {   /* a few pixels: u16 count, then (x, y, colour) */
      int np = rd16(tl.pts);
      for (const uint8_t *q = tl.pts + 2; np--; q += 3) {
        int x = tx + q[0], y = ty + q[1];
        if (y < a || y >= b || x < 0 || x >= VIEW_W) continue;
        uint16_t *d = band + (y - by) * VIEW_W + x;
        *d = blend(tl.pal[q[2]], *d, (tl.al[q[2]] * ga + 16) >> 5);
      }
      continue;
    }
    int x0 = tx < 0 ? -tx : 0, x1 = tx + tl.w > VIEW_W ? VIEW_W - tx : tl.w;
    for (int y = a; y < b; y++) {
      const uint8_t *row = tl.px + (y - ty) * (tl.w >> 1);
      uint16_t *d = band + (y - by) * VIEW_W;
      for (int bx = x0 >> 1; bx < (x1 + 1) >> 1; bx++) {   /* two pixels a byte, mostly clear */
        unsigned v = row[bx];
        if (!v) continue;
        int x = 2 * bx;
        unsigned c = v & 15;
        if (c && x >= x0) d[tx + x] = blend(tl.pal[c], d[tx + x], (tl.al[c] * ga + 16) >> 5);
        c = v >> 4;
        if (c && x + 1 < x1) d[tx + x + 1] = blend(tl.pal[c], d[tx + x + 1], (tl.al[c] * ga + 16) >> 5);
      }
    }
  }
}

static __attribute__((noinline)) void draw_text_item(const Item *it, int y0, int y1, int by, int rows, unsigned ga) {
  const Aff *f = &affs[it->ref];
  font_scale3(f->scale);
  font_draw(item_ptr[it->ref], f->align, f->lw, f->lh, (int)floorf(f->m.tx), (int)floorf(f->m.ty), band, by, rows, VIEW_W, it->color, ga);
  font_scale(1);
}

static void draw_item(const Item *it, int by, int rows) {
  int y0 = it->y0 > by ? it->y0 : by, y1 = it->y1 < by + rows ? it->y1 : by + rows;
  if (y0 >= y1) return;
  unsigned ga = (it->alpha * 32u + 127) / 255;
  switch (it->kind) {
    case DI_STREAM:
      /* another big one this frame is drawn from the cache if it fits */
      if (it == streaming) draw_stream(it, y0, y1, by, ga);
      else draw_sprite_item(it, y0, y1, by, rows, ga);
      break;
    case DI_RECT:
      for (int y = y0; y < y1; y++) {
        uint16_t *d = band + (y - by) * VIEW_W;
        int x0 = it->x0 < 0 ? 0 : it->x0, x1 = it->x1 < VIEW_W ? it->x1 : VIEW_W;
        if (ga >= 32) for (int x = x0; x < x1; x++) d[x] = it->color;
        else for (int x = x0; x < x1; x++) d[x] = blend(it->color, d[x], ga);
      }
      break;
    case DI_SPRITE: draw_sprite_item(it, y0, y1, by, rows, ga); break;
    case DI_AFFINE: draw_affine_item(it, y0, y1, by, rows, ga); break;
    case DI_SHAPE: draw_shape_item(it, y0, y1, by, rows, ga); break;
    case DI_TEXT: draw_text_item(it, y0, y1, by, rows, ga); break;
    case DI_TILES: draw_tiles_item(y0, y1, by, ga); break;
    case DI_MASK: draw_mask_item(it, y0, y1, by); break;
  }
}

/* Bands whose content is the same as last frame's are not drawn again: the
 * screen keeps them. A band's content is summed up as a hash of its items. */
#define NBANDS ((SCREEN_H + BAND - 1) / BAND)
static uint32_t band_hash[NBANDS];
static bool redraw_all = true;
void gfx_redraw_all(void) { redraw_all = true; }

static inline uint32_t mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
static uint32_t mixf(uint32_t h, float f) {
  uint32_t v;
  memcpy(&v, &f, 4);
  return mix(h, v);
}
/* missing: whether DF_MISSING counts (the band drawn has it: when the sprite
 * comes, the band is drawn again) */
static uint32_t item_hash(uint32_t h, const Item *it, bool missing) {
  unsigned kind = it->kind == DI_STREAM ? DI_SPRITE : it->kind, flags = missing ? it->flags : it->flags & ~DF_MISSING;
  h = mix(h, (uint32_t)kind | (uint32_t)it->alpha << 8 | (uint32_t)flags << 16);
  h = mix(h, (uint32_t)(uint16_t)it->x0 | (uint32_t)(uint16_t)it->y0 << 16);
  h = mix(h, (uint32_t)(uint16_t)it->x1 | (uint32_t)(uint16_t)it->y1 << 16);
  h = mix(h, (uint32_t)it->ref | (uint32_t)it->color << 16);
  if (it->kind == DI_AFFINE || it->kind == DI_SHAPE || it->kind == DI_TEXT) {
    const Aff *f = &affs[it->ref];
    h = mixf(mixf(mixf(h, f->m.a), f->m.b), f->m.c);
    h = mixf(mixf(mixf(h, f->m.d), f->m.tx), f->m.ty);
    if (it->kind != DI_AFFINE) h = mix(h, (uint32_t)(uintptr_t)item_ptr[it->ref]);
    if (it->kind == DI_TEXT) {
      h = mix(h, (uint32_t)f->align | (uint32_t)f->scale << 8 | (uint32_t)(uint16_t)f->lw << 16);
      for (const char *c = item_ptr[it->ref]; *c; c++) h = mix(h, (uint8_t)*c);
    }
  }
  return h;
}

static uint16_t letter_color;
static bool letter_dirty = true;
void gfx_letterbox(uint16_t c) {
  if (c != letter_color) letter_dirty = true;
  letter_color = c;
}

void gfx_view(int top, int bottom) {
  if (top == view_top && bottom == view_bottom) return;
  view_top = top;
  view_bottom = bottom;
  letter_dirty = redraw_all = true;
}
int gfx_view_top(void) { return view_top; }
int gfx_view_bottom(void) { return view_bottom; }

/* sprites whose items all end above y: the cache may let them go first */
static void release_done(int y) {
  for (int i = 0; i < nitems; i++) {
    const Item *it = &items[i];
    if (it->y1 <= y - BAND || it->y1 > y) continue;
    if (it->kind == DI_SPRITE || it->kind == DI_MASK || it->kind == DI_STREAM) spr_release(it->ref);
    else if (it->kind == DI_AFFINE) spr_release(it->color);
  }
}

#ifdef HOST
int gfx_peak_items, gfx_peak_affs;   /* tests: the most a frame used */
#endif
void gfx_end(void) {
#ifdef HOST
  if (nitems > gfx_peak_items) gfx_peak_items = nitems;
  if (naffs > gfx_peak_affs) gfx_peak_affs = naffs;
#endif
  if (letter_dirty) {
    if (VIEW_Y + view_top > 0) plat_fill(0, 0, SCREEN_W, VIEW_Y + view_top, letter_color);
    if (VIEW_Y + view_bottom < SCREEN_H) plat_fill(0, VIEW_Y + view_bottom, SCREEN_W, SCREEN_H - VIEW_Y - view_bottom, letter_color);
    letter_dirty = false;
  }
#ifdef HOST
  if (getenv("CI_STATS")) {
    long area[DI_KINDS] = {0};
    int cnt[DI_KINDS] = {0};
    for (int i = 0; i < nitems; i++) {
      const Item *it = &items[i];
      int x0 = it->x0 < 0 ? 0 : it->x0, x1 = it->x1 > VIEW_W ? VIEW_W : it->x1;
      int y0 = it->y0 < view_top ? view_top : it->y0, y1 = it->y1 > view_bottom ? view_bottom : it->y1;
      if (x1 > x0 && y1 > y0) area[it->kind] += (long)(x1 - x0) * (y1 - y0);
      cnt[it->kind]++;
    }
    fprintf(stderr, "items %d: sprite %d/%ld affine %d/%ld rect %d/%ld shape %d/%ld text %d/%ld stream %d/%ld\n", nitems,
            cnt[0], area[0], cnt[1], area[1], cnt[2], area[2], cnt[3], area[3], cnt[4], area[4], cnt[5], area[5]);
  }
#endif
#ifdef HOST
  if (getenv("CI_ITEMS"))
    for (int i = 0; i < nitems; i++)
      fprintf(stderr, "item %d kind %d a %d f %d box %d %d %d %d ref %u col %u\n", i, items[i].kind, items[i].alpha, items[i].flags,
              items[i].x0, items[i].y0, items[i].x1, items[i].y1, items[i].ref, items[i].color);
#endif
  /* decode what the frame needs before drawing, so bands only read the cache;
   * a frame needing more than the cache holds (a menu over the island, a busy
   * street) is decoded band by band instead, from the top, each sprite let go
   * once its last band is drawn (release_done) */
  streaming = NULL;
  line_spr = NULL;
  /* what a menu's opaque panel hides is not drawn (nor decoded) */
  for (int j = 0; j < nitems; j++) {
    const Item *o = &items[j];
    if (o->kind != DI_SPRITE || !(o->flags & DF_OVERLAY) || o->alpha != 255 || (o->flags & (DF_FLIPX | DF_FLIPY | DF_OPAQUE)) ||
        (o->x1 - o->x0) * (o->y1 - o->y0) < VIEW_W * VIEW_H / 4)
      continue;
    int16_t b[4];
    if (!spr_opaque_box(o->ref, b)) continue;
    int bx0 = o->x0 + b[0], by0 = o->y0 + b[1], bx1 = o->x0 + b[2], by1 = o->y0 + b[3];
    for (int i = 0; i < j; i++) {
      Item *it = &items[i];
      if (it->x0 >= bx0 && it->y0 >= by0 && it->x1 <= bx1 && it->y1 <= by1) it->kind = DI_HIDDEN;
    }
  }
  /* the bands to draw: those whose items changed since they were drawn (the
   * others keep their pixels); only what they show is decoded */
  uint32_t base = bgmem ? mix(mix(mix(mix(2166136261u, (uint32_t)bg_cx), (uint32_t)bg_cy), bg_gen), water_gen)
                        : mix(1, clear_color);
  bool dirty[NBANDS];
  for (int by = view_top; by < view_bottom; by += BAND) {
    int rows = view_bottom - by < BAND ? view_bottom - by : BAND;
    uint32_t h = base;
    for (int i = 0; i < nitems; i++) {
      const Item *it = &items[i];
      if (it->y1 > by && it->y0 < by + rows && it->kind != DI_HIDDEN) h = item_hash(h, it, false);
    }
    dirty[(by - view_top) / BAND] = redraw_all || h != band_hash[(by - view_top) / BAND];
  }
  bool shown[MAX_ITEMS];
  for (int i = 0; i < nitems; i++) {
    const Item *it = &items[i];
    int y0 = it->y0 > view_top ? it->y0 : view_top, y1 = it->y1 < view_bottom ? it->y1 : view_bottom;
    shown[i] = false;
    for (int b = (y0 - view_top) / BAND; y0 < y1 && b <= (y1 - 1 - view_top) / BAND && !shown[i]; b++) shown[i] = dirty[b];
  }
  uint8_t seen[(SPRITE_COUNT + 7) / 8];
  memset(seen, 0, sizeof seen);
  uint32_t need = 0;   /* cache the frame's sprites take */
  for (int i = 0; i < nitems; i++) {
    Item *it = &items[i];
    if ((it->kind != DI_SPRITE && it->kind != DI_AFFINE) || !shown[i]) continue;
    uint16_t sp = it->kind == DI_SPRITE ? it->ref : it->color;
    if (!(seen[sp >> 3] & (1 << (sp & 7))) && !spr_pinned(sp)) {   /* pinned ones take no room */
      Sprite t;
      sprite_info(sp, &t);
      need += (t.rle + 3) & ~3u;
      seen[sp >> 3] |= (uint8_t)(1 << (sp & 7));
#ifdef HOST
      if (getenv("CI_NEED") && atoi(getenv("CI_NEED")) > 1) fprintf(stderr, "  counts %u (%u B) item %d kind %d box %d %d %d %d\n", sp, (unsigned)t.rle, i, it->kind, it->x0, it->y0, it->x1, it->y1);
#endif
    }
  }
  bool over = need + 1024 > spr_capacity();
  /* more than the cache holds: the biggest sprite drawn as it decodes (top
   * down, once) may leave room for the others; with a menu, the menu's
   * biggest, and the scene's sprites that do not fit then come in the next
   * frames (only their bands are drawn again) */
  scene_later = false;
  if (over) {
    uint32_t need_menu = 0;   /* the menu's part of need */
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < nitems; i++) {
      const Item *it = &items[i];
      if ((it->kind != DI_SPRITE && it->kind != DI_AFFINE) || !shown[i] || !(it->flags & DF_OVERLAY)) continue;
      uint16_t sp = it->kind == DI_SPRITE ? it->ref : it->color;
      if (!(seen[sp >> 3] & (1 << (sp & 7))) && !spr_pinned(sp)) {
        Sprite t;
        sprite_info(sp, &t);
        need_menu += (t.rle + 3) & ~3u;
        seen[sp >> 3] |= (uint8_t)(1 << (sp & 7));
      }
    }
    for (int menu = 0; menu < 2 && over; menu++) {
      if (menu && !need_menu) break;
      int big = -1;
      uint32_t bigsz = 0;
      bool stream_already = false;
      for (int i = 0; i < nitems; i++) {
        const Item *it = &items[i];
        if (it->kind == DI_STREAM && shown[i]) stream_already = true;
        if (it->kind != DI_SPRITE || !shown[i] || (it->flags & (DF_FLIPY | DF_OPAQUE)) || spr_pinned(it->ref) ||
            (menu && !(it->flags & DF_OVERLAY)))
          continue;
        Sprite t;
        sprite_info(it->ref, &t);
        if (t.kind <= 1 && t.rle > bigsz) { big = i; bigsz = t.rle; }
      }
      for (int i = 0; big >= 0 && i < nitems; i++)   /* one item: a stream is read once */
        if (i != big && (items[i].kind == DI_SPRITE || items[i].kind == DI_AFFINE) &&
            (items[i].kind == DI_SPRITE ? items[i].ref : items[i].color) == items[big].ref)
          big = -1;
      uint32_t bs = big >= 0 && !stream_already && !spr_peek(items[big].ref) ? (bigsz + 3) & ~3u : 0;
      /* (a little too much: the last small ones come next frame) */
      if (menu ? need_menu - bs <= spr_capacity() : bs && need - bs <= spr_capacity() + 1024) {
        if (bs) { items[big].kind = DI_STREAM; need -= bs; }
        over = false;
        scene_later = menu;
      }
    }
  }
#ifdef HOST
  if (getenv("CI_NEED")) fprintf(stderr, "need %u of %u%s%s\n", (unsigned)need, (unsigned)spr_capacity(), over ? " (by band)" : "",
                                 scene_later ? " (scene later)" : "");
#endif
  /* the menus' first; then the scene's, as long as the menus' can stay (too
   * many: some of the scene's come next frame) */
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < nitems && !over; i++) {
      Item *it = &items[i];
      if ((it->kind != DI_SPRITE && it->kind != DI_AFFINE) || !shown[i] || !(it->flags & DF_OVERLAY) != pass) continue;
      uint16_t sp = it->kind == DI_SPRITE ? it->ref : it->color;
      if (pass) spr_get_soft(sp);
      else spr_get(sp);
    }
  /* then the masks, which may take the place of what this frame does not draw */
  for (int i = 0; i < nitems && !over; i++)
    if (items[i].kind == DI_MASK && shown[i]) spr_mask(items[i].ref);
  /* big sprites are cached too while everything fits; otherwise the first
   * one keeps the decoder while the bands go down */
  for (int i = 0; i < nitems; i++) {
    Item *it = &items[i];
    if (it->kind != DI_STREAM || !shown[i]) continue;
    Sprite t;
    sprite_info(it->ref, &t);
    uint32_t sz = (t.rle + 3) & ~3u;
    if (!over && need + sz + 1024 <= spr_capacity() && spr_get(it->ref)) {
      it->kind = DI_SPRITE;
      need += sz;
    } else if (!streaming) stream_begin(it);
  }
  /* with a stream holding the decoder nothing is decoded while the bands go
   * down: what is not cached is drawn next frame */
  for (int i = 0; i < nitems && (streaming || !over); i++) {
    Item *it = &items[i];
    if (!shown[i]) continue;
    if ((it->kind == DI_SPRITE || it->kind == DI_AFFINE || (it->kind == DI_STREAM && it != streaming)) &&
        !spr_peek(it->kind == DI_AFFINE ? it->color : it->ref))
      it->flags |= DF_MISSING;
    if (it->kind == DI_MASK && !spr_peek_mask(it->ref)) it->flags |= DF_MISSING;
  }
  for (int by = view_top; by < view_bottom; by += BAND) {
    int rows = view_bottom - by < BAND ? view_bottom - by : BAND;
    if (!dirty[(by - view_top) / BAND]) { if (over) release_done(by + rows); continue; }
    uint32_t h = base;
    for (int i = 0; i < nitems; i++) {
      const Item *it = &items[i];
      if (it->y1 > by && it->y0 < by + rows && it->kind != DI_HIDDEN) h = item_hash(h, it, true);
    }
    band_hash[(by - view_top) / BAND] = h;
#ifdef HOST
    if (getenv("CI_BANDS")) fprintf(stderr, "band %d\n", by);
#endif
    band_bg(by, rows);
    for (int i = 0; i < nitems; i++) {
      const Item *it = &items[i];
      if (it->y1 > by && it->y0 < by + rows) draw_item(it, by, rows);
    }
    plat_push(0, VIEW_Y + by, VIEW_W, rows, band);
    if (over) release_done(by + rows);
  }
  redraw_all = false;
  spr_tick();
}
