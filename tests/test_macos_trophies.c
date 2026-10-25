/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_trophy.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>
void runtime_restart(void) { abort(); }
static atomic_int wins;
static void *unlock_same(void *unused) {
    (void)unused; int32_t platinum;
    int32_t result=runtime_trophy_unlock(14,&platinum);
    if (!result) atomic_fetch_add(&wins,1); else assert(result==(int32_t)0x8055160c);
    return NULL;
}
int main(void) {
    char folder[]="/tmp/bb-trophies-XXXXXX"; assert(mkdtemp(folder));
    char path[512]; snprintf(path,sizeof(path),"%s/trophies.log",folder);
    runtime_trophy_configure(folder);
    size_t size=0; const RuntimeTrophy *catalog=runtime_trophy_catalog(&size);
    assert(size==40 && catalog[0].grade==1 && !strcmp(catalog[30].name,"Rune Contact") && catalog[34].group==1);
    int32_t platinum=99;
    typedef int32_t (ABI *Unlock)(int32_t,int32_t,int32_t,int32_t*);
    Unlock unlock=(Unlock)runtime_services_resolve("28xmRUFao68#l#m"); assert(unlock);
    assert(runtime_trophy_unlock(40,&platinum)==(int32_t)0x8055160a && platinum==-1);
    assert(runtime_trophy_unlock(0,&platinum)==(int32_t)0x8055160d);
    assert(!unlock(1,1,30,&platinum) && platinum==-1);
    RuntimeTrophyEvent event; assert(runtime_trophy_take_event(&event) && event.id==30 && event.timestamp>0);
    assert(!runtime_trophy_take_event(&event));
    assert(runtime_trophy_unlock(30,&platinum)==(int32_t)0x8055160c && !runtime_trophy_take_event(&event));
    pthread_t threads[8];
    for (int i=0;i<8;++i) assert(!pthread_create(threads+i,NULL,unlock_same,NULL));
    for (int i=0;i<8;++i) assert(!pthread_join(threads[i],NULL));
    assert(atomic_load(&wins)==1 && runtime_trophy_take_event(&event) && event.id==14 && !runtime_trophy_take_event(&event));
    FILE *file=fopen(path,"w"); assert(file);
    fputs("14 200\n14 100\nbroken\n999 10\n30 -5\n30 10junk\n",file); fclose(file);
    runtime_trophy_configure(folder);
    RuntimeTrophySnapshot snapshot; runtime_trophy_snapshot(&snapshot);
    assert(snapshot.unlocked[14]==100 && !snapshot.unlocked[30] && !runtime_trophy_take_event(&event));
    file=fopen(path,"w"); assert(file);
    for (int id=1;id<=32;++id) fprintf(file,"%d %d\n",id,100+id);
    fprintf(file,"34 500\n"); fclose(file); runtime_trophy_configure(folder);
    runtime_trophy_snapshot(&snapshot); assert(!snapshot.unlocked[0]);
    assert(!runtime_trophy_unlock(33,&platinum) && platinum==0);
    assert(runtime_trophy_take_event(&event) && event.id==33);
    assert(runtime_trophy_take_event(&event) && event.id==0 && !runtime_trophy_take_event(&event));
    runtime_trophy_snapshot(&snapshot); assert(snapshot.unlocked[0]==snapshot.unlocked[33]);
    typedef int32_t (ABI *GetInfo)(int32_t,int32_t,RuntimeTrophyGameDetails*,RuntimeTrophyGameData*);
    GetInfo info=(GetInfo)runtime_services_resolve("YYP3f2W09og#l#m"); assert(info);
    RuntimeTrophyGameDetails details={.size=sizeof(details)};
    RuntimeTrophyGameData data={.size=sizeof(data)};
    assert(!info(1,1,&details,&data) && details.total==40 && data.total==35 && data.platinum==1 && data.progress==87);
    typedef int32_t (ABI *GetUnlocks)(int32_t,int32_t,uint32_t*,uint32_t*);
    GetUnlocks state=(GetUnlocks)runtime_services_resolve("LHuSmO3SLd8#l#m"); assert(state);
    uint32_t bits[4],unlocked;
    assert(!state(1,1,bits,&unlocked) && unlocked==35 && bits[0]==UINT32_MAX && bits[1]==7 && bits[2]==0 && bits[3]==0);
    typedef int32_t (ABI *GetTrophy)(int32_t,int32_t,int32_t,RuntimeTrophyDetails*,RuntimeTrophyData*);
    GetTrophy trophy=(GetTrophy)runtime_services_resolve("qqUVGDgQBm0#l#m"); assert(trophy);
    RuntimeTrophyDetails trophy_details={.size=sizeof(trophy_details)};
    RuntimeTrophyData trophy_data={.size=sizeof(trophy_data)};
    assert(!trophy(1,1,30,&trophy_details,&trophy_data) && trophy_data.unlocked && trophy_details.grade==4);
    assert(!strcmp(trophy_details.name,"Rune Contact") && trophy_data.timestamp==UINT64_C(0xdcbffeff2bc000)+UINT64_C(130000000));
    runtime_trophy_configure(folder); assert(!runtime_trophy_take_event(&event));
    file=fopen(path,"w"); assert(file);
    for (int id=1;id<=33;++id) fprintf(file,"%d %d\n",id,100+id);
    fclose(file); runtime_trophy_configure(folder);
    runtime_trophy_snapshot(&snapshot); assert(snapshot.unlocked[0]==133 && !runtime_trophy_take_event(&event));
    char bad[512]; snprintf(bad,sizeof(bad),"%s/missing",folder); runtime_trophy_configure(bad);
    runtime_trophy_snapshot(&snapshot); const uint64_t before_failure=snapshot.revision;
    platinum=99;
    /* Exercise the guest ABI: local I/O failures must not change the game's control flow. */
    assert(!unlock(1,1,1,&platinum) && platinum==-1);
    runtime_trophy_snapshot(&snapshot);
    assert(snapshot.revision>before_failure && !snapshot.unlocked[1] && snapshot.error[0] && !runtime_trophy_take_event(&event));
    assert(!trophy(1,1,1,NULL,&trophy_data) && !trophy_data.unlocked && !trophy_data.timestamp);
    assert(!mkdir(bad,0700));
    assert(!unlock(1,1,1,&platinum) && platinum==-1);
    runtime_trophy_snapshot(&snapshot);
    assert(snapshot.unlocked[1]>0 && !snapshot.error[0]);
    assert(runtime_trophy_take_event(&event) && event.id==1 && !runtime_trophy_take_event(&event));
    assert(unlock(1,1,1,&platinum)==(int32_t)0x8055160c);
    runtime_trophy_configure(bad); runtime_trophy_snapshot(&snapshot);
    assert(snapshot.unlocked[1]==event.timestamp && !snapshot.error[0] && !runtime_trophy_take_event(&event));
    char recovered[1024]; snprintf(recovered,sizeof(recovered),"%s/trophies.log",bad);
    unlink(recovered); rmdir(bad);
    unlink(path); rmdir(folder);
    puts("Achievements: persistent history, concurrent deduplication, platinum, event queue and PS4 queries passed");
    return 0;
}
