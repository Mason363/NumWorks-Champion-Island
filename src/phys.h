/* The doodle's physics world, a cannon.js world of axis-aligned boxes (and
 * ramps) that never rotate: gravity along z, a ground plane at z = 0, bodies
 * of named types (mass, damping, collision group and mask, material) and
 * contact materials (friction, restitution). Units: map pixels, per second. */
#ifndef CI_PHYS_H
#define CI_PHYS_H
#include "ci.h"

#define BODY_MAX 64
enum { SH_BOX, SH_LEFT_RAMP, SH_RIGHT_RAMP };
typedef struct {
  float x, y, z;           /* centre */
  float hx, hy, hz;        /* half extents */
  float vx, vy, vz;
  float inv_mass, damping;
  uint16_t group, mask;
  uint8_t material, shape, type;
  bool used, active;
  uint16_t user;           /* the entity (NodeId) */
  uint8_t ncontacts;
  int8_t contacts[6];      /* bodies touched in the last step (-1 = the ground) */
} Body;
extern Body bodies[BODY_MAX];

void phys_reset(void);                      /* the default world (kitsune's cannonWorld) */
void phys_gravity(float gz);
void phys_steps(int substeps, int iterations);
int phys_material(const char *name);
void phys_contact_material(const char *a, const char *b, float friction, float restitution);
void phys_body_type(const char *type, const char *material, float mass, uint16_t group, uint16_t mask, float damping);
int phys_type(const char *type);            /* -1 if unknown */
int phys_add(int type, uint8_t shape, float x, float y, float z, float hx, float hy, float hz);
void phys_remove(int body);
void phys_step(float dt);                   /* one frame (1/30 s), in substeps */
float phys_ground(float x, float y, float from_z);   /* highest surface under (x, y) below from_z */
bool phys_touching(int a, int b);
#endif
