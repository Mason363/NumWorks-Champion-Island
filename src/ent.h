/* Entities: nodes carrying the doodle's components ("this.T" in Animate), and
 * the doodle's shared systems. A port of kitsune's entity helpers:
 *
 *   kitsune           here
 *   N(e)              ent_pos(e)           position in the map's space
 *   Fj(e, p)          ent_set_pos(e, x, y)
 *   Gj(e)             ent_bounds(e, &r)    bounds (its "bounds" child) in map space
 *   Q(scene, C)       ent_first(C)         first entity with a component
 *   Y(scene, C, fn)   for (i = ent_iter_begin(); ...)  see ent_next()
 *   Kj/Nj(e, label)   ent_label(e, label, keep_frame)
 *   Lj(e, dir)        ent_dir_label(e, dir)
 *   Pj+Qj / Rj        ent_spawn(sym, parent) / ent_spawn_at(sym, x, y)
 *   co(e)             ent_remove(e)
 *   Ij(cam, e, ...)   ent_in_view(e, margin, use_bounds)
 *
 * Mutable component state (velocity, direction, z, triggers, physics) lives
 * in an Ent record per entity that needs one; sports keep their own records
 * keyed by NodeId. */
#ifndef CI_ENT_H
#define CI_ENT_H
#include "game.h"

typedef struct { float x, y, w, h; } Rect;
static inline bool rect_contains(Rect r, float x, float y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
static inline bool rect_intersects(Rect a, Rect b) { return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h; }
static inline Rect rect_pad(Rect r, float l, float t, float rt, float b) { return (Rect){r.x - l, r.y - t, r.w + l + rt, r.h + t + b}; }

/* directions, as the doodle names them */
enum { DIR_E, DIR_SE, DIR_S, DIR_SW, DIR_W, DIR_NW, DIR_N, DIR_NE };
extern const char *const dir_names[8];
int dir_of(float x, float y, bool four);             /* mh(): 8 (or 4) way name of a vector, -1 if zero */
int dir_parse(const char *s);
float dir_angle(int d);                              /* degrees */

#define ENT_MAX 48   /* records: only what moves, turns or holds a trigger (34 at most seen) */
#define TRIG_MAX 4
typedef struct {
  NodeId n;
  float vx, vy, vz;           /* velocity component (px per tick), z velocity (pC) */
  float z, ground;            /* zObject z; KT: the ground height under it */
  bool has_ground;
  int8_t dir;                 /* direction component */
  uint8_t ntrig;
  NodeId trig[TRIG_MAX];      /* trigger: entities inside it now (ex) */
  int16_t body;               /* physics body or -1 */
  bool bounds_valid, pos_valid;
  Rect bounds;                /* cached (Bb) */
  float px, py;               /* cached position (kb) */
  uint16_t anim_t;            /* free timers for systems */
} Ent;

/* the scene's map (the entity with the "map" component) and camera */
extern NodeId ent_map, ent_camera;
void ent_reset(void);
void ent_register_tree(NodeId root);                 /* Mj on a subtree: finds entities */
void ent_unregister(NodeId n);                       /* on removal (node.c calls it on free) */
Ent *ent_get(NodeId n);                              /* state record (made on demand), NULL if not an entity */
Ent *ent_peek(NodeId n);                             /* the state record if it has one (never makes one) */
bool ent_has(NodeId n, int comp);
uint16_t ent_T(NodeId n);
NodeId ent_first(int comp);
NodeId ent_first2(int comp, int comp2);
int ent_count(void);
NodeId ent_at(int i);                                /* i-th registered entity (may be 0 after removals) */

/* positions: all in the map's space */
void ent_pos(NodeId e, float *x, float *y);
void ent_set_pos(NodeId e, float x, float y);
void ent_local_pos(NodeId e, float *x, float *y);   /* Cj: origin in the parent's space */
void ent_set_local_pos(NodeId e, float x, float y);  /* Dj */
void ent_moved(NodeId e);                            /* forget cached positions and bounds (xj) */
bool ent_bounds(NodeId e, Rect *r);                  /* Gj */
bool ent_overlap(NodeId a, NodeId b);                /* Sj */
Rect ent_viewport(void);                             /* the camera's view in map space */
bool ent_in_view(NodeId e, float margin, bool use_bounds);
bool ent_in_view4(NodeId e, float l, float t, float r, float b, bool use_bounds);

/* labels */
bool ent_label(NodeId e, const char *label, bool keep_frame);   /* Kj: gotoAndStop(label) */
void ent_dir_label(NodeId e, int dir);                          /* Lj */
bool ent_play_label(NodeId e, const char *label);               /* gotoAndPlay */

/* making and removing entities */
NodeId ent_spawn(uint16_t sym);                      /* Pj: new instance, registered */
void ent_add(NodeId e, NodeId parent);               /* Qj: add under parent (addFx hooks run) */
NodeId ent_spawn_at(uint16_t sym, float x, float y); /* Rj: spawn, add to the map at (x, y) */
void ent_remove(NodeId e);                           /* co: detach and free */

/* ---------------------------------------------------------------- shared systems */
void sys_back_pauses(void);                  /* Ep: Back opens the pause menu */
void sys_tutorial_once(void);                /* Uo: first visit shows the sport's rules */
typedef struct { int t; bool active; } Countdown;
void sys_countdown_start(Countdown *c);
bool sys_countdown(Countdown *c);            /* Wo: 3, 2, 1, GO; true once it is over */
void sys_ephemeral(void);                    /* Vp */
void sys_sprite_dirs(void);                  /* aq */
void sys_walk_idle(void);                    /* cq */
void sys_jump_to_frame(void);                /* bq */
void sys_velocity(void);                     /* fq: velocity moves entities without a body */
void sys_move_direct(void);                  /* zr */
void sys_sort_draw(void);                    /* Pp: map children by draw order or y */
void sys_sort_positional(void);              /* Qp: by the bottom of their bounds, with z */
void sys_zsprite(void);                      /* dq */
void sys_delete_offscreen(void);             /* Up */
void sys_triggers(void);                     /* Hr: trigger areas, "trigger"/"untrigger" events */
void sys_player_movement(void);              /* Rp */
void sys_waypoints(void);                    /* Sp */
void sys_player_dir(void);                   /* Tp */
void sys_visibility(void);                   /* Ls: hides what is far from the camera */
void sys_tile_backgrounds(void);             /* Os */
void sys_camera_target(void);                /* Cq */
void sys_camera_move(void);                  /* Dq */
void sys_camera_snap(void);                  /* Eq: at start */
void sys_ground_height(void);                /* Ar */
void sys_physics(void);                      /* yr: the cannon world steps; bodies follow entities */
int ent_contacts(NodeId e, NodeId *out, int max);   /* what it touched (0 = the ground) */
void camera_set_follow(bool follow);         /* Baa */

/* the trigger system calls these (JS: entity.on("trigger"/"untrigger")) */
typedef void (*TriggerFn)(NodeId trigger, NodeId other, bool entered);
void ent_on_trigger(TriggerFn fn);

/* per-scene hooks for spawned entities (addFx / removeFx) */
void sys_add_fx(NodeId e);                   /* Xp */
void ent_translate(NodeId e);                /* Oq: translatable texts */
void sys_remove_fx(NodeId e);                /* Yp */
#endif
