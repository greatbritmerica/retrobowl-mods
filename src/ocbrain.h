// ocbrain.h - the player's offensive coordinator: which route to call, learned from your passes.
// Pure logic (no game calls), so tests/ocbrain_test.c can check it on any machine. Game glue: offtools.c.
#pragma once
#include "sys.h"

enum { OC_NROUTES = 7, OC_KEEP = 6 };                  // 0-5 = ROUTES[] in offtools.c, 6 = keep the game's route
enum { OC_TOP, OC_MID, OC_BOT };
enum { OC_WHY_WORKS, OC_WHY_UNSEEN, OC_WHY_SURPRISE, OC_WHY_KEEP, OC_WHY_NEW };   // UNSEEN: the defense doesn't know that zone

typedef struct { double att[7][3][3], ok[7][3][3], yds[7][3][3]; } OcBrain;     // [route][position][situation]
typedef struct { double fam[9]; double surprise, keep, counter; } OcCtx;       // fam: dbrain zone familiarity 0-1
typedef struct { const void *who[6]; int route[6], pos[6], n; } OcTable;       // this play's receivers

int  oc_situation(double togo);                        // 0 short (<=3), 1 medium (<=7), 2 long
int  oc_zone(int route, int side);                     // dbrain zone (side 0/1/2 as dbrain), -1 for keep
int  oc_pick(const OcBrain *b, int pos, int side, int sit, const OcCtx *c, const int used[7],
             double (*rnd)(void), int *why);           // route 0-6
void oc_learn(OcBrain *b, int route, int pos, int sit, int ok, double yds);
void oc_fade(OcBrain *b, double keep);                 // every count x keep (a new game)
int  oc_save(const OcBrain *b, FILE *f);               // 1 = ok
int  oc_load(OcBrain *b, FILE *f);                     // 1 = ok; on failure b is zeroed
void oc_table_reset(OcTable *t);
void oc_table_set(OcTable *t, const void *who, int route, int pos);   // add, or update if present
int  oc_table_find(const OcTable *t, const void *who);                // index or -1
