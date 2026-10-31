/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BB_RUNTIME_TROPHY_H
#define BB_RUNTIME_TROPHY_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define BB_TROPHY_COUNT 40
typedef struct {
    int id,grade,group; /* grade: 1 platinum, 2 gold, 3 silver, 4 bronze; group: 0 base, 1 DLC */
    const char *name,*description;
} RuntimeTrophy;
typedef struct {
    int64_t unlocked[BB_TROPHY_COUNT]; /* Unix seconds, zero while locked */
    uint64_t revision;
    char error[256];
} RuntimeTrophySnapshot;
typedef struct { int id; int64_t timestamp; } RuntimeTrophyEvent;
const RuntimeTrophy *runtime_trophy_catalog(size_t *count);
/* Load existing log entries quietly; never announce historical unlocks. */
void runtime_trophy_configure(const char *user);
void runtime_trophy_snapshot(RuntimeTrophySnapshot *out);
int runtime_trophy_take_event(RuntimeTrophyEvent *out);
/* Log failures return success to the game; the snapshot carries the UI error. */
int32_t runtime_trophy_unlock(int id,int32_t *platinum);

/* PS4 ABI used by the local trophy service. */
typedef struct {
    uint64_t size; int32_t trophy_id,grade,group_id;
    uint8_t hidden,reserved[3]; char name[128],description[1024];
} RuntimeTrophyDetails;
typedef struct {
    uint64_t size; int32_t trophy_id; uint8_t unlocked,reserved[3]; uint64_t timestamp;
} RuntimeTrophyData;
typedef struct {
    uint64_t size; uint32_t groups,total,platinum,gold,silver,bronze;
    char title[128],description[1024];
} RuntimeTrophyGameDetails;
typedef struct {
    uint64_t size; uint32_t total,platinum,gold,silver,bronze,progress;
} RuntimeTrophyGameData;
#ifdef __cplusplus
}
#endif
#endif
