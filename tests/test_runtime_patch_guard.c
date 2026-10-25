/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include "runtime_patch_guard.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* This verifier never installs a hook or runs the guest allocation callback. */
void *runtime_low_alloc_aligned(size_t size,size_t align) { (void)size; (void)align; abort(); }

static void entry(FILE *f,uint64_t offset,const unsigned char *data,uint64_t size) {
    assert(fwrite(&offset,8,1,f)==1 && fwrite(&size,8,1,f)==1);
    assert(fwrite(data,1,(size_t)size,f)==size);
}
int main(void) {
    unsetenv("BB_CONTEXT_LOCK"); unsetenv("BB_FRAME_POOL");
    const unsigned char original=0x55, changed=0x90;
    const uint64_t sites[]={0xebb960,0x2199af0,0x219d220,0x2198fd0,0xe453b0};
    for (size_t i=0;i<sizeof(sites)/sizeof(sites[0]);++i) {
        assert(runtime_patch_conflict(sites[i],&changed,1));
        assert(!runtime_patch_conflict(sites[i],&original,1));
    }
    const unsigned char spanning[]={0x90,0x55};
    assert(!runtime_patch_conflict(sites[0]-1,spanning,sizeof(spanning)));
    assert(runtime_patch_conflict(UINT64_MAX,&changed,2));
    const unsigned char call[]={0xe8,0xa2,0xce,0xb3,0x00},ret[]={0xc3,0x90,0x90,0x90,0x90};
    assert(!runtime_patch_conflict(0x2483ec1-0x400000,call,sizeof(call)));
    assert(runtime_patch_conflict(0x2483ec1-0x400000,ret,sizeof(ret)));
    setenv("BB_CONTEXT_LOCK","0",1);
    assert(!runtime_patch_conflict(sites[0],&changed,1));
    assert(runtime_patch_conflict(sites[3],&changed,1));
    unsetenv("BB_CONTEXT_LOCK"); setenv("BB_FRAME_POOL","0",1);
    assert(runtime_patch_conflict(sites[0],&changed,1));
    assert(!runtime_patch_conflict(sites[3],&changed,1));
    setenv("BB_CONTEXT_LOCK","0",1);
    for (size_t i=0;i<sizeof(sites)/sizeof(sites[0]);++i)
        assert(!runtime_patch_conflict(sites[i],&changed,1));
    assert(runtime_patch_conflict(0x2483ec1-0x400000,ret,sizeof(ret)));
    unsetenv("BB_CONTEXT_LOCK"); unsetenv("BB_FRAME_POOL");
    FILE *f=tmpfile(); assert(f);
    entry(f,0x1000,&changed,1); entry(f,sites[0],&changed,1);
    rewind(f); const char *reason=NULL;
    assert(runtime_validate_patch_set(f,2,&reason)==2 && reason && ftell(f)==0);
    fclose(f);
    f=tmpfile(); assert(f); entry(f,0x1000,&changed,1); rewind(f);
    assert(runtime_validate_patch_set(f,1,&reason)==0 && !reason && ftell(f)==0);
    assert(runtime_validate_patch_set(f,2,&reason)==1 && ftell(f)==0);
    assert(fseek(f,0,SEEK_END)==0 && fputc(0,f)!=EOF); rewind(f);
    assert(runtime_validate_patch_set(f,1,&reason)==1 && ftell(f)==0);
    fclose(f);
    puts("PASS: runtime hook/wait protection, disabled hooks, whole-set scan and stream restoration");
    return 0;
}
