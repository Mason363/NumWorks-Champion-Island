/* The dialogue box (kitsune's dialog scene: Wq, Yq, Zq). */
#include <math.h>
#include <stdio.h>
#ifdef HOST
#include <stdlib.h>
#endif
#include "ent.h"
#include "font.h"

typedef struct {
  uint16_t name, npc, avatar, text;
  uint8_t nopts, setv;
  const uint8_t *opts;          /* nopts x (next, text) */
  const uint8_t *set;           /* var id, then a value (type + 4 bytes) */
} DNode;

static NodeId root, box;         /* S_dialog_Spa and its dialog child (Xda) */
static bool active;
static DNode cur;
static uint16_t cur_npc;
static char wrapped[400];
static int shown, opt_shown[3];
static NodeId focus;
static int held_ticks, last_dir = -1;

static bool dnode_at(uint32_t i, DNode *d) {
  if (i >= HDR(H_NDLG)) return false;
  const uint8_t *p = ci_data + rd32(ci_data + HDR(H_DLG) + 4 * i);
  d->name = rd16(p); d->npc = rd16(p + 2); d->avatar = rd16(p + 4); d->text = rd16(p + 6);
  d->nopts = p[8]; d->setv = p[9];
  d->opts = p + 10;
  d->set = d->opts + 4 * d->nopts;
  return true;
}

/* the node npc+name (Yk) */
static bool dnode_find(uint16_t npc, uint16_t name, DNode *d) {
  uint32_t n = HDR(H_NDLG);
  for (uint32_t i = 0; i < n; i++)
    if (dnode_at(i, d) && d->npc == npc && d->name == name) return true;
  return false;
}

/* an NPC's first node (Zk) */
bool dialog_first_node(uint16_t npc, uint16_t *name) {
  DNode d;
  uint32_t n = HDR(H_NDLG);
  for (uint32_t i = 0; i < n; i++)
    if (dnode_at(i, &d) && d.npc == npc) { *name = d.name; return true; }
  return false;
}

bool dialog_active(void) { return active; }

void dialog_init(void) {
  root = node_new_sym(S_dialog_Spa);
  box = root ? nodes[root].first : 0;
  active = false;
}

static NodeId options[3];
static int find_options(void) {
  /* dialogOption buttons sorted by event id (option1..3) */
  int n = 0;
  for (NodeId c = nodes[box].first; c; c = nodes[c].next)
    if (nodes[c].T != NONE16 && comp_has(nodes[c].T, C_dialogOption) && n < 3) options[n++] = c;
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && strcmp(str(comp_str(nodes[options[j - 1]].T, C_button, F_eventId)), str(comp_str(nodes[options[j]].T, C_button, F_eventId))) > 0; j--) {
      NodeId t = options[j]; options[j] = options[j - 1]; options[j - 1] = t;
    }
  return n;
}

static NodeId child_with(int comp) {
  for (NodeId c = nodes[box].first; c; c = nodes[c].next)
    if (nodes[c].T != NONE16 && comp_has(nodes[c].T, comp)) return c;
  return 0;
}

static bool single_next(const DNode *d) {   /* bl: one option with no text */
  return d->nopts == 1 && rd16(d->opts + 2) == NONE16;
}

static int text_lw;
static char shown_buf[400];
static void show(uint16_t npc, uint16_t name) {
  if (!dnode_find(npc, name, &cur)) { active = false; return; }
  cur_npc = npc;
  NodeId avatar = child_with(C_dialogAvatar), txt = node_child(box, "text");
  const char *av = str(cur.avatar);
  if (avatar && av[0] && node_has_label(avatar, av)) {
    node_set_visible(avatar, true);
    ent_label(avatar, av, false);
    if (txt) nodes[txt].x = 60;
    text_lw = 167;
  } else {
    if (avatar) node_set_visible(avatar, false);
    if (txt) nodes[txt].x = 10;
    text_lw = 217;
  }
  if (txt) node_set_text_width(txt, (int16_t)text_lw);
  /* the text, broken into lines as the box shows it (getMetrics: the font's
   * 10 px against the text's lineWidth, both in the text's own space) */
  dtext(cur.text, shown_buf, sizeof shown_buf);   /* shown_buf is free until the text types */
  const char *t = shown_buf;
  int k = 0;
  const char *s = t;
  int lw_px = text_lw;
  while (*s && k < (int)sizeof wrapped - 2) {
    /* reuse the font's wrapping: copy line by line */
    const char *nl = strchr(s, '\n');
    size_t pl = nl ? (size_t)(nl - s) : strlen(s);
    char para[400];
    if (pl >= sizeof para) pl = sizeof para - 1;
    memcpy(para, s, pl);
    para[pl] = 0;
    const char *a = para;
    while (*a) {
      const char *e = a + strlen(a);
      if (font_width(a, e) > lw_px) {
        const char *last = NULL;
        for (const char *p = a; p < e; p++) if (*p == ' ' && font_width(a, p) <= lw_px) last = p;
        if (last) e = last;
      }
      size_t l = (size_t)(e - a);
      if (k + (int)l + 1 >= (int)sizeof wrapped) break;
      memcpy(wrapped + k, a, l);
      k += (int)l;
      wrapped[k++] = '\n';
      a = e;
      while (*a == ' ') a++;
    }
    if (!nl) break;
    s = nl + 1;
  }
  wrapped[k] = 0;
  shown = 0;
  memset(opt_shown, 0, sizeof opt_shown);
  NodeId base = node_child(box, "base");
  if (base) {
    float sx, sy, rot;
    node_get_xform(base, &sx, &sy, &rot);
    node_xform(base, sx, 1, rot);        /* qK.scaleY = 1 */
  }
  if (txt) node_set_text(txt, "");
  NodeId nx = child_with(C_dialogNext);
  if (nx) node_set_visible(nx, false);
  NodeId ptr = node_child(box, "pointer");
  if (ptr) node_set_visible(ptr, false);
  int no = find_options();
  for (int i = 0; i < no; i++) node_set_visible(options[i], false);
  focus = 0;
  /* the node's saved value (tags.zd) */
  if (cur.setv) {
    extern const char *const var_names[];
    uint16_t var = rd16(cur.set);
    uint8_t type = cur.set[2];
    const uint8_t *v = cur.set + 3;
    if (type == 0) store_set(var_names[var], (Value){SV_NULL, false, 0, NONE16});
    else if (type == 1) store_set_bool(var_names[var], rd32(v) != 0);
    else if (type == 2) store_set_num(var_names[var], rdf(v));
    else store_set_sid(var_names[var], (uint16_t)rd32(v));
  }
}

void dialog_start(uint16_t npc, uint16_t node) {
  DNode d;
  if (!dnode_find(npc, node, &d)) return;
#ifdef HOST
  if (getenv("CI_LOG")) { extern uint32_t host_time; fprintf(stderr, "t%u dialog %s %s\n", host_time / 33, str(npc), str(node)); }
#endif
  active = true;
  show(npc, node);
}

static char opt_buf[3][96];

static void next(int opt) {
  int len = (int)strlen(wrapped);
  if (shown < len - 1) { shown = len - 1; return; }
  if (opt < cur.nopts) show(cur_npc, rd16(cur.opts + 4 * opt));
  else active = false;
  if (!active) input_latch();          /* the OK that ended it opens no door */
}

void dialog_tick(void) {
  if (!active) return;
  node_tick(root);
  int len = (int)strlen(wrapped);
  if (shown < len - 1) shown += len - shown - 1 < 2 ? len - shown - 1 : 2;
  NodeId txt = node_child(box, "text");
  memcpy(shown_buf, wrapped, (size_t)shown);
  shown_buf[shown] = 0;
  if (txt) node_set_text(txt, shown_buf);
  /* getMeasuredHeight: its lines at its lineHeight */
  int lines = font_lines(shown_buf, 0);
  float lh = txt && nodes[txt].ref != NONE16 ? rds16(payload(nodes[txt].ref) + 10) * .25f : 12;
  float m = fmaxf(lines * lh + 20, 68);
  NodeId nx = child_with(C_dialogNext), ptr = node_child(box, "pointer");
  if (nx) node_set_visible(nx, true);
  int no = find_options();
  bool done = shown >= len - 1, can_go = false;
  if (done) {
    if (cur.nopts == 0 || single_next(&cur)) {
      if (ptr) { nodes[ptr].y = m - 5; node_set_visible(ptr, true); }
      can_go = true;
    } else {
      if (nx) node_set_visible(nx, false);
      float c = 0;
      for (int a = 0; a < cur.nopts && a < no; a++) {
        NodeId o = options[a];
        nodes[o].y = m - 8 + 12 * a;
        c = nodes[o].y;
        char ot[96];
        dtext(rd16(cur.opts + 4 * a + 2), ot, sizeof ot);
        if (node_visible(o)) {
          if (opt_shown[a] < (int)strlen(ot)) {
            opt_shown[a]++;
            int l = opt_shown[a] < (int)sizeof opt_buf[a] - 1 ? opt_shown[a] : (int)sizeof opt_buf[a] - 1;
            memcpy(opt_buf[a], ot, (size_t)l);
            opt_buf[a][l] = 0;
            NodeId ttx = node_child(o, "text");
            if (ttx) node_set_text(ttx, opt_buf[a]);
          }
        } else {
          char pt[96];
          if (a == 0 || opt_shown[a - 1] >= dtext(rd16(cur.opts + 4 * (a - 1) + 2), pt, sizeof pt)) {
            node_set_visible(o, true);
            opt_shown[a] = 0;
            NodeId ttx = node_child(o, "text");
            if (ttx) node_set_text(ttx, "");
          }
          break;
        }
      }
      m = c + 8;
      int last = cur.nopts - 1;
      char lt[96];
      can_go = last >= 0 && last < no && node_visible(options[last]) && opt_shown[last] >= dtext(rd16(cur.opts + 4 * last + 2), lt, sizeof lt);
    }
  }
  /* the box grows to the text (qK: base) */
  NodeId base = node_child(box, "base");
  if (base) {
    float sx, sy, rot;
    node_get_xform(base, &sx, &sy, &rot);
    float bh = 68;   /* base art height at scale 1 */
    float target = m / bh;
    node_xform(base, sx, sy + (target - sy) * .8f, rot);
  }
  /* keys: OK goes on (or picks the focused option), arrows move the focus */
  if (!done) {
    if (in.pressed[A_ACTION]) { input_consume(A_ACTION); next(0); }
    node_update(root);
    return;
  }
  if (cur.nopts > 1 || (cur.nopts == 1 && !single_next(&cur))) {
    bool ok = false;
    for (int a = 0; a < no; a++) if (options[a] == focus && node_visible(focus)) ok = true;
    if (!ok) focus = no ? options[0] : 0;
    for (int a = 0; a < no; a++) node_goto(options[a], options[a] == focus ? "focus" : "idle", 0, false);
    int d = dir_of(in.jx, in.jy, true);
    if (d != last_dir) held_ticks = 0;
    held_ticks++;
    last_dir = d;
    if ((d == DIR_N || d == DIR_S) && (held_ticks == 1 || (held_ticks > 15 && held_ticks % 3 == 0)))
      for (int a = 0; a < no; a++)
        if (options[a] == focus) {
          int b = d == DIR_N ? a - 1 : a + 1;
          if (b >= 0 && b < cur.nopts && node_visible(options[b])) focus = options[b];
          break;
        }
    if (in.pressed[A_ACTION] && can_go) {
      input_consume(A_ACTION);
      for (int a = 0; a < no; a++) if (options[a] == focus) { next(a); break; }
    }
  } else if (in.pressed[A_ACTION] && can_go) {
    input_consume(A_ACTION);
    next(0);
  }
  if (active) node_update(root);
}

void dialog_draw(void) {
  if (!active) return;
  /* at the top of the screen, as at the top of the stage */
  node_draw(root, (Mat){1.0f / 3, 0, 0, 1.0f / 3, 0, (float)gfx_view_top()});
}
