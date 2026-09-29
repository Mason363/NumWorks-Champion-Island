/* Reading data.bin: strings, sprites, symbols, clips, components. */
#include "ci.h"

const char *str(uint16_t id) {
  if (id >= HDR(H_NSTRINGS)) return "";
  return (const char *)ci_data + HDR(H_STRDATA) + rd32(ci_data + HDR(H_STRTAB) + 4u * id);
}
/* the dialogue's lines: byte-pair encoded (pack.py: bpe); a code byte stands
 * for two bytes, each a code or a character */
int dtext(uint16_t id, char *buf, int size) {
  const uint8_t *t = ci_data + HDR(H_DTEXT);
  if (!size) return 0;
  buf[0] = 0;
  if (id >= rd16(t)) return 0;
  const uint8_t *codes = t + 4, *pairs = t + 36, *offs = t + 36 + 512;
  unsigned n = rd16(t);
  const uint8_t *data = offs + 4 * (n + 1);
  const uint8_t *p = data + rd32(offs + 4 * id), *end = data + rd32(offs + 4 * id + 4);
  uint8_t stack[32];
  int len = 0;
  while (p < end) {
    int sp = 0;
    stack[sp++] = *p++;
    while (sp) {
      uint8_t c = stack[--sp];
      if (codes[c >> 3] & (1 << (c & 7))) {
        if (sp + 2 > (int)sizeof stack) break;
        stack[sp++] = pairs[2 * c + 1];
        stack[sp++] = pairs[2 * c];
      } else if (len < size - 1) buf[len++] = (char)c;
    }
  }
  buf[len] = 0;
  return len;
}

/* binary search in the ids sorted by their bytes */
int str_find(const char *s) {
  const uint8_t *sorted = ci_data + HDR(H_STRSORT);
  int lo = 0, hi = (int)HDR(H_NSTRINGS) - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    uint16_t id = rd16(sorted + 2 * mid);
    int c = strcmp(str(id), s);
    if (!c) return id;
    if (c < 0) lo = mid + 1; else hi = mid - 1;
  }
  return -1;
}

uint16_t str_id(const char *s) {
  int i = str_find(s);
  return i < 0 ? NONE16 : (uint16_t)i;
}

void sprite_info(uint16_t id, Sprite *s) {
  const uint8_t *p = ci_data + HDR(H_SPRITES) + 16u * id;
  s->rle = rd32(p + 12);
  s->w = rd16(p);
  s->h = rd16(p + 2);
  s->sheet = p[4];
  s->kind = p[5];
  s->bank = rd16(p + 6);
  s->off = rd32(p + 8);
}

const uint16_t *pal565(uint8_t sheet) { return (const uint16_t *)(const void *)(ci_data + HDR(H_PAL) + 768u * sheet); }
const uint8_t *palalpha(uint8_t sheet) { return ci_data + HDR(H_PAL) + 768u * sheet + 512; }

void sym_info(uint16_t sym, SymInfo *s) {
  const uint8_t *p = ci_data + HDR(H_SYMS) + 8u * sym;
  s->type = p[0];
  s->lib = p[1];
  s->v = rd32(p + 4);
}

bool clip_get(uint16_t sym, Clip *c) {
  SymInfo si;
  if (sym >= SYM_COUNT) return false;
  sym_info(sym, &si);
  if (si.type != SYM_CLIP) return false;
  const uint8_t *p = ci_data + si.v;
  c->nframes = rd16(p);
  c->nslots = rd16(p + 2);
  c->nlabels = p[4];
  c->nacts = p[5];
  c->T = rd16(p + 6);
  for (int i = 0; i < 4; i++) c->nb[i] = rds16(p + 8 + 2 * i);
  c->labels = p + 16;
  c->acts = c->labels + 4 * c->nlabels;
  c->slots = c->acts + 5 * c->nacts;
  return true;
}

int clip_label(const Clip *c, const char *label) {
  for (int i = 0; i < c->nlabels; i++)
    if (!strcmp(str(rd16(c->labels + 4 * i + 2)), label)) return rd16(c->labels + 4 * i);
  return -1;
}

const uint8_t *payload(uint16_t id) { return ci_data + rd32(ci_data + HDR(H_PAYLOADS) + 4u * id); }

const float *mat(uint16_t id) { return (const float *)(const void *)(ci_data + HDR(H_MATS) + 16u * id); }

/* ---------------------------------------------------------------- components */
static unsigned field_size(uint8_t type) {
  switch (type) {
    case T_INT: case T_FLOAT: return 4;
    case T_BOOL: return 1;
    case T_STR: case T_SYM: return 2;
    case T_VEC: return 8;
  }
  return 0;
}

const uint8_t *comp_find(uint16_t T, int comp) {
  if (T == NONE16) return NULL;
  const uint8_t *p = ci_data + rd32(ci_data + HDR(H_TTAB) + 4u * T);
  unsigned n = *p++;
  for (unsigned i = 0; i < n; i++) {
    if (p[0] == comp) return p;
    unsigned nf = p[1];
    p += 2;
    for (unsigned f = 0; f < nf; f++) p += 2 + field_size(p[1]);
  }
  return NULL;
}

bool comp_has(uint16_t T, int comp) { return comp_find(T, comp) != NULL; }

static const uint8_t *field(uint16_t T, int comp, int fid, uint8_t *type) {
  const uint8_t *p = comp_find(T, comp);
  if (!p) return NULL;
  unsigned nf = p[1];
  p += 2;
  for (unsigned f = 0; f < nf; f++) {
    if (p[0] == fid) {
      *type = p[1];
      return p + 2;
    }
    p += 2 + field_size(p[1]);
  }
  return NULL;
}

int32_t comp_int(uint16_t T, int comp, int f, int32_t def) {
  uint8_t t;
  const uint8_t *p = field(T, comp, f, &t);
  if (!p) return def;
  if (t == T_INT) return (int32_t)rd32(p);
  if (t == T_FLOAT) return (int32_t)rdf(p);
  if (t == T_BOOL) return *p;
  return def;
}

float comp_float(uint16_t T, int comp, int f, float def) {
  uint8_t t;
  const uint8_t *p = field(T, comp, f, &t);
  if (!p) return def;
  if (t == T_INT) return (float)(int32_t)rd32(p);
  if (t == T_FLOAT) return rdf(p);
  if (t == T_BOOL) return *p;
  return def;
}

bool comp_bool(uint16_t T, int comp, int f, bool def) {
  uint8_t t;
  const uint8_t *p = field(T, comp, f, &t);
  if (!p) return def;
  if (t == T_BOOL) return *p != 0;
  if (t == T_INT) return rd32(p) != 0;
  return def;
}

uint16_t comp_str(uint16_t T, int comp, int f) {
  uint8_t t;
  const uint8_t *p = field(T, comp, f, &t);
  return p && t == T_STR ? rd16(p) : NONE16;
}

uint16_t comp_sym(uint16_t T, int comp, int f) {
  uint8_t t;
  const uint8_t *p = field(T, comp, f, &t);
  return p && t == T_SYM ? rd16(p) : NONE16;
}

/* the doodle's translated texts ("messages"), by key */
const char *msg(const char *key) {
  uint32_t lo = 0, hi = HDR(H_NMSGS);
  const uint8_t *t = ci_data + HDR(H_MSGS);
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    int c = strcmp(str(rd16(t + 4 * mid)), key);
    if (!c) return str(rd16(t + 4 * mid + 2));
    if (c < 0) lo = mid + 1; else hi = mid;
  }
  return key;
}

bool comp_vec(uint16_t T, int comp, int f, float *x, float *y) {
  uint8_t t;
  const uint8_t *p = field(T, comp, f, &t);
  if (!p || t != T_VEC) return false;
  *x = rdf(p);
  *y = rdf(p + 4);
  return true;
}

/* ---------------------------------------------------------------- random */
float frand(void) { return (plat_random() >> 8) * (1.0f / 16777216.0f); }
float frand_range(float a, float b) { return a + frand() * (b - a); }
