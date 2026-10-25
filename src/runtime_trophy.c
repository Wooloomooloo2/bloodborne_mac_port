/* SPDX-License-Identifier: GPL-2.0-or-later
 * Local achievements, compatible with pyL1nx/bloodborne_pc's trophies.log and
 * launcher/bbport_trophies.py (64919f7da13a24bac53578acec5f74f959fe5c4e).
 * Ported from evill33t/bloodborne_mac; immutable source links in docs/ACHIEVEMENTS.md.
 * Game calls produce durable unlocks and queued UI events; no UI on guest threads.
 */
#include "runtime.h"
#include "runtime_trophy.h"
#include "../gpu/bbgpu_trophy.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <stdlib.h>

static const RuntimeTrophy catalog[]={
#include "bloodborne_trophies.inc"
};
_Static_assert(sizeof(catalog)/sizeof(*catalog)==BB_TROPHY_COUNT,"Bloodborne trophy catalog");
_Static_assert(sizeof(RuntimeTrophyDetails)==1176,"PS4 trophy details ABI");
_Static_assert(sizeof(RuntimeTrophyData)==24,"PS4 trophy data ABI");
static pthread_mutex_t trophy_lock=PTHREAD_MUTEX_INITIALIZER;
static RuntimeTrophySnapshot state;
static RuntimeTrophyEvent events[BB_TROPHY_COUNT];
static size_t head,count;
static uint64_t notifications; /* live unlocks in this session, never restored history */
static char log_path[1200];
const RuntimeTrophy *runtime_trophy_catalog(size_t *size) {
    if (size) *size=BB_TROPHY_COUNT;
    return catalog;
}
static int append(int id,int64_t timestamp,int announce) {
    FILE *file=*log_path ? fopen(log_path,"a") : NULL;
    if (!file) {
        snprintf(state.error,sizeof(state.error),"Could not save achievement: %s",*log_path ? strerror(errno) : "save folder is not configured");
        ++state.revision; return 0;
    }
    int ok=fprintf(file,"%d %lld\n",id,(long long)timestamp)>0;
    if (fclose(file)) ok=0;
    if (!ok) {
        snprintf(state.error,sizeof(state.error),"Could not finish saving achievement."); ++state.revision; return 0;
    }
    state.unlocked[id]=timestamp; state.error[0]=0; ++state.revision;
    if (announce && count<BB_TROPHY_COUNT) {
        events[(head+count)%BB_TROPHY_COUNT]=(RuntimeTrophyEvent){id,timestamp}; ++count;
        notifications |= UINT64_C(1) << id;
        printf("Runtime: trophy %d unlocked\n",id);
    }
    return 1;
}
static int award_platinum(int announce) {
    if (state.unlocked[0]) return 0;
    int64_t latest=0;
    for (int id=1;id<=33;++id) {
        if (!state.unlocked[id]) return 0;
        if (state.unlocked[id]>latest) latest=state.unlocked[id];
    }
    return append(0,latest,announce);
}
void runtime_trophy_configure(const char *user) {
    pthread_mutex_lock(&trophy_lock);
    const uint64_t revision=state.revision+1;
    memset(&state,0,sizeof(state)); state.revision=revision; head=count=0; notifications=0;
    if (!user || snprintf(log_path,sizeof(log_path),"%s/trophies.log",user)>=(int)sizeof(log_path)) log_path[0]=0;
    FILE *file=*log_path ? fopen(log_path,"r") : NULL;
    if (file) {
        char line[512];
        while (fgets(line,sizeof(line),file)) {
            char *end,*start=line; errno=0;
            long id=strtol(start,&end,10);
            if (end==start || errno || !isspace((unsigned char)*end)) continue;
            start=end; long long when=strtoll(start,&end,10);
            if (end!=start && !errno && (!*end || isspace((unsigned char)*end)) && id>=0 && id<BB_TROPHY_COUNT && when>0 && when<=INT64_C(253402300799)) {
                if (!state.unlocked[id] || when<state.unlocked[id]) state.unlocked[id]=when;
            }
        }
        fclose(file);
    }
    award_platinum(0); /* old launcher histories get the same platinum, silently */
    bbgpu_trophy_update(&state,notifications);
    pthread_mutex_unlock(&trophy_lock);
}
void runtime_trophy_snapshot(RuntimeTrophySnapshot *out) {
    if (!out) return;
    pthread_mutex_lock(&trophy_lock); *out=state; pthread_mutex_unlock(&trophy_lock);
}
int runtime_trophy_take_event(RuntimeTrophyEvent *out) {
    if (!out) return 0;
    pthread_mutex_lock(&trophy_lock);
    int have=count!=0;
    if (have) { *out=events[head]; head=(head+1)%BB_TROPHY_COUNT; --count; }
    pthread_mutex_unlock(&trophy_lock); return have;
}
int32_t runtime_trophy_unlock(int id,int32_t *platinum) {
    if (platinum) *platinum=-1;
    if (id<0 || id>=BB_TROPHY_COUNT) return (int32_t)0x8055160a;
    pthread_mutex_lock(&trophy_lock);
    int32_t result=0;
    if (state.unlocked[id]) result=(int32_t)0x8055160c;
    else if (!id) result=(int32_t)0x8055160d;
    else if (append(id,(int64_t)time(NULL),1)) {
        if (award_platinum(1) && platinum) *platinum=0;
    }
    /* Local persistence failures stay in state.error for the UI, never the game's result. */
    bbgpu_trophy_update(&state,notifications);
    pthread_mutex_unlock(&trophy_lock); return result;
}
