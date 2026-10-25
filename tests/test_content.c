#include "runtime.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
typedef int32_t (ABI *Module)(uint32_t);
typedef int32_t (ABI *Init)(const void *,void *);
typedef int32_t (ABI *Param)(uint32_t,int32_t *);
typedef int32_t (ABI *List)(uint32_t,void *,uint32_t,uint32_t *);
#define GET(t,n) ((t)runtime_content_resolve(n))
static Module stop_load,stop_loaded,stop_unload;
static Init stop_init;
static int stop_appcontent_loaded;
/* exit(21) runs this check in the same process, before the records disappear.
 * Exercise the public lifecycle to check exact reference counts and the
 * initialized provider state without exposing runtime internals to the test. */
static void check_stopped_records(void) {
    assert((uint32_t)stop_loaded(0)==0x805a1000);
    assert((uint32_t)stop_loaded(0xb6)==0x805a1001);
    assert(stop_loaded(0xb5)==0 && stop_unload(0xb5)==0);
    assert(stop_loaded(0xb5)==0 && stop_unload(0xb5)==0);
    assert((uint32_t)stop_loaded(0xb5)==0x805a1001);
    assert(stop_loaded(UINT16_MAX)==0 && stop_unload(UINT16_MAX)==0);
    assert((uint32_t)stop_loaded(UINT16_MAX)==0x805a1001);
    if (stop_appcontent_loaded) {
        unsigned char initial[32]={0},boot[40];
        assert(stop_loaded(0xb4)==0);
        assert((uint32_t)stop_init(initial,boot)==0x80d90003);
        assert(stop_unload(0xb4)==0 && stop_loaded(0xb4)==0);
        assert((uint32_t)stop_init(initial,boot)==0x80d90003);
        assert(stop_unload(0xb4)==0 && (uint32_t)stop_loaded(0xb4)==0x805a1001);
        assert(stop_load(0xb4)==0 && stop_init(initial,boot)==0 && stop_unload(0xb4)==0);
    } else {
        assert((uint32_t)stop_loaded(0xb4)==0x805a1001);
        assert((uint32_t)stop_unload(0xb4)==0x805a1001);
    }
    puts("PASS: system-module records preserved after unsupported full-width request");
}
int main(int argc,char **argv) {
    Module load=GET(Module,"g8cM39EUZ6o#M#N");
    Module loaded=GET(Module,"fMP5NHUOaMk#M#N");
    Module unload=GET(Module,"eR2bZFAAU0Q#M#N");
    Init init=GET(Init,"R9lA82OraNs#c#d");
    Param param=GET(Param,"99b82IKXpH4#c#d");
    List list=GET(List,"xnd8BJzAxmk#c#d");
    assert(load && loaded && unload && init && param && list);
    assert(!runtime_content_resolve("g8cM39EUZ6o#I#J"));
    if (argc>1 && !strcmp(argv[1],"--missing")) { load(0xb4); return 99; }
    const uint32_t profile[]={1,13,0x80000000,0,7};
    runtime_content_configure(profile);
    if (argc==5 && !strcmp(argv[1],"--wide")) {
        char *end;
        unsigned long long id=strtoull(argv[3],&end,0);
        assert(!*end && id<=UINT32_MAX);
        Module operation=NULL;
        if (!strcmp(argv[2],"load")) operation=load;
        if (!strcmp(argv[2],"query")) operation=loaded;
        if (!strcmp(argv[2],"unload")) operation=unload;
        assert(operation);
        stop_appcontent_loaded=!strcmp(argv[4],"loaded");
        assert(stop_appcontent_loaded || !strcmp(argv[4],"unloaded"));
        assert(load(0xb5)==0 && load(0xb5)==0 && load(UINT16_MAX)==0);
        if (stop_appcontent_loaded) {
            unsigned char initial[32]={0},boot[40];
            assert(load(0xb4)==0 && load(0xb4)==0 && init(initial,boot)==0);
        }
        stop_load=load; stop_loaded=loaded; stop_unload=unload; stop_init=init;
        assert(atexit(check_stopped_records)==0);
        operation((uint32_t)id);
        return 99; /* Unsupported IDs must stop; no result is invented. */
    }
    if (argc>1 && !strcmp(argv[1],"--unknown")) {
        /* Other system modules are host-provided: reference counted, independent. */
        assert((uint32_t)loaded(0xb5)==0x805a1001);
        assert(load(0xb5)==0 && loaded(0xb5)==0 && (uint32_t)loaded(0xb4)==0x805a1001);
        assert(unload(0xb5)==0 && (uint32_t)loaded(0xb5)==0x805a1001);
        puts("PASS: independent module references"); return 0;
    }
    assert((uint32_t)load(0)==0x805a1000);
    assert((uint32_t)load(UINT32_C(0x80000000))==0x805a1000);
    assert((uint32_t)loaded(0)==0x805a1000);
    assert((uint32_t)unload(0)==0x805a1000);
    assert((uint32_t)loaded(0xb4)==0x805a1001);
    assert((uint32_t)unload(0xb4)==0x805a1001);
    assert(load(0xb4)==0 && load(0xb4)==0 && loaded(0xb4)==0);
    /* Proven invalid-ID returns must preserve existing references, too. */
    assert((uint32_t)load(0)==0x805a1000);
    assert((uint32_t)load(UINT32_C(0x80000000))==0x805a1000);
    assert((uint32_t)loaded(0)==0x805a1000);
    assert((uint32_t)unload(0)==0x805a1000);
    unsigned char initial[32]={0};
    struct { unsigned char boot[40]; uint32_t canary; } b;
    memset(&b,0xa5,sizeof(b));
    assert((uint32_t)init(NULL,b.boot)==0x80d90002);
    initial[31]=1; assert((uint32_t)init(initial,b.boot)==0x80d90002); initial[31]=0;
    assert(b.boot[0]==0xa5 && init(initial,b.boot)==0);
    for (unsigned i=0;i<40;++i) assert(b.boot[i]==0);
    assert(b.canary==0xa5a5a5a5);
    b.boot[0]=0xcc; assert((uint32_t)init(initial,b.boot)==0x80d90003 && b.boot[0]==0xcc);
    struct { int32_t value; uint32_t canary; } result={-1,0x1234};
    for (unsigned i=0;i<5;++i) {
        assert(param(i,&result.value)==0 && (uint32_t)result.value==profile[i]);
        assert(result.canary==0x1234);
    }
    result.value=77;
    assert((uint32_t)param(5,&result.value)==0x80d90002 && result.value==77);
    assert((uint32_t)param(0,NULL)==0x80d90002);
    unsigned char entries[48]; memset(entries,0xaa,sizeof(entries));
    struct { uint32_t hits,canary; } h={99,0xabcdef};
    assert(list(0,NULL,0,&h.hits)==0 && h.hits==0 && h.canary==0xabcdef);
    assert(list(0,entries,2,&h.hits)==0 && h.hits==0);
    for (unsigned i=0;i<sizeof(entries);++i) assert(entries[i]==0xaa);
    assert((uint32_t)list(0,NULL,0,NULL)==0x80d90002);
    assert(unload(0xb4)==0 && loaded(0xb4)==0);
    assert((uint32_t)init(initial,b.boot)==0x80d90003);
    assert(unload(0xb4)==0 && (uint32_t)loaded(0xb4)==0x805a1001);
    assert(load(0xb4)==0 && init(initial,b.boot)==0 && unload(0xb4)==0);
    puts("PASS: AppContent lifecycle, profile and output boundaries");
    return 0;
}
