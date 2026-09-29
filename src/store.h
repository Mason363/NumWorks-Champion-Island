/* The doodle keeps all progress as saved values (localStorage "KITSUNE_*"):
 * quests ("RAIN" = "complete"), team, scores, ratings, what was seen. The same
 * values are kept here and saved in one file on the calculator. */
#ifndef CI_STORE_H
#define CI_STORE_H
#include "ci.h"

enum { SV_NONE, SV_NULL, SV_BOOL, SV_NUM, SV_STR };
typedef struct {
  uint8_t type;
  bool b;
  float num;
  uint16_t s;            /* string id */
} Value;

Value store_get(const char *key);                 /* SV_NONE if never set */
bool store_bool(const char *key, bool def);        /* JS truthiness */
float store_num(const char *key, float def);
const char *store_str(const char *key);            /* NULL unless a string */
bool store_is(const char *key, const char *s);     /* key == "s" */
void store_set(const char *key, Value v);
void store_set_bool(const char *key, bool b);
void store_set_num(const char *key, float n);
void store_set_str(const char *key, const char *s);   /* s must be one of data.bin's strings */
void store_set_sid(const char *key, uint16_t s);
void store_clear(void);                            /* new game */
bool store_dirty(void);
bool store_save(void);
void store_load(void);

/* quest conditions: the doodle's expressions, compiled by tools/pack.py */
bool cond_eval(uint16_t condition_string);         /* id of the condition's text */
bool cond_eval_str(const char *condition);
#endif
