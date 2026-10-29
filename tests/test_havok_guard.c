// SPDX-License-Identifier: GPL-2.0-or-later
/* Execute private 1.09 upload and pool instructions with resource operations
 * stubbed. No game bytes are distributed. BB_HAVOK_GUARD_ELF opts into this test. */
#define _GNU_SOURCE
#include "runtime.h"
#include <assert.h>
#include <signal.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
enum { IMAGE_SIZE=0x6000000, ACQUIRE=0x1067f60, TAKE=0x1056920, GIVE=0x1056990 };
static unsigned char pool[0x90], owner[0x130], parent[0xa0], context[0xb800];
static unsigned char *resource=parent+0x40;
static uint64_t nodes[2][2], *node_ptrs[2];
static uint32_t flags[2];
static uint64_t canary=0x123456789abcdef;
static unsigned check_error=8, map_error;
static uintptr_t *pool_slot;
static int replace_pool;
static void ABI noop(void) {}
static void ABI yield(void) { sched_yield(); }
static unsigned ABI context_kind(void *c) { assert(c==context); return 1; }
static unsigned ABI check(void *r, unsigned access, unsigned kind) {
    assert(r==resource && access==3 && kind==1); return check_error;
}
static unsigned ABI map(void *c, void *scratch, void *r, unsigned sub, unsigned access, void **out) {
    assert(c==context && scratch==context+0xb730 && r==resource && sub==0 && access==3);
    if(replace_pool) *pool_slot=0;
    *out=context; return map_error;
}
static void put_jump(unsigned char *p, uintptr_t target) {
    const unsigned char op[]={0xff,0x25,0,0,0,0}; memcpy(p,op,6);memcpy(p+6,&target,8);
}
static uint64_t u64(const unsigned char *p) { uint64_t n; memcpy(&n,p,8); return n; }
static uint32_t u32(const unsigned char *p) { uint32_t n; memcpy(&n,p,4); return n; }
static uint16_t u16(const unsigned char *p) { uint16_t n; memcpy(&n,p,2); return n; }
static void copy_original(FILE *f, unsigned char *image, uint64_t address, size_t size) {
    unsigned char header[64], ph[56];
    assert(fseek(f,0,SEEK_SET)==0 && fread(header,1,sizeof(header),f)==sizeof(header));
    assert(!memcmp(header,"\177ELF\2\1",6) && u16(header+18)==62);
    assert(u16(header+54)>=sizeof(ph));
    for (unsigned i=0;i<u16(header+56);++i) {
        assert(fseek(f,(long)(u64(header+32)+i*u16(header+54)),SEEK_SET)==0);
        assert(fread(ph,1,sizeof(ph),f)==sizeof(ph));
        const uint64_t start=u64(ph+16), bytes=u64(ph+32);
        if (u32(ph)==1 && address>=start && address-start<=bytes && size<=bytes-(address-start)) {
            assert(address+size<=IMAGE_SIZE);
            assert(fseek(f,(long)(u64(ph+8)+address-start),SEEK_SET)==0);
            assert(fread(image+address,1,size,f)==size);
            return;
        }
    }
    assert(!"instruction window is not file backed");
}

static void init(void) {
    memset(pool,0,sizeof(pool));memset(owner,0,sizeof(owner));
    static void *vtable[6];vtable[3]=noop;vtable[5]=noop;
    void *vt=vtable, *ptr=node_ptrs, *fp=flags, *r=resource-0x40, *buffer=resource;
    unsigned capacity=2;
    memcpy(pool+8,&capacity,4);memcpy(pool+0x28,&ptr,8);memcpy(pool+0x50,&fp,8);memcpy(pool+0x70,&vt,8);
    memcpy(owner+0x118,&r,8);memcpy(resource,&buffer,8);
    for(unsigned i=0;i<2;i++){nodes[i][1]=(uintptr_t)context;node_ptrs[i]=nodes[i];flags[i]=1;}
}
int main(int argc,char **argv) {
    const char *path=argc>1?argv[1]:getenv("BB_HAVOK_GUARD_ELF");
    if(!path || !*path){puts("SKIP: Havok pool fixture (set BB_HAVOK_GUARD_ELF to a private prepared 1.09 ELF)");return 0;}
    FILE *f=fopen(path,"rb");assert(f);
    unsigned char *image=mmap(NULL,IMAGE_SIZE,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(image!=MAP_FAILED);
    copy_original(f,image,ACQUIRE,0x153);copy_original(f,image,TAKE,0x6b);copy_original(f,image,GIVE,0x72);copy_original(f,image,0x1057350,8);fclose(f);
    uintptr_t canary_ptr=(uintptr_t)&canary;memcpy(image+0x53e3be0,&canary_ptr,8);
    put_jump(image+0x207f970,(uintptr_t)yield);
    uintptr_t pool_ptr=(uintptr_t)pool;memcpy(image+0x54b8788,&pool_ptr,8);put_jump(image+0x1049890,(uintptr_t)noop);
    pool_slot=(void *)(image+0x54b8788);
    put_jump(image+0x216a7c0,(uintptr_t)context_kind);put_jump(image+0x2165150,(uintptr_t)check);
    put_jump(image+0x2169c30,(uintptr_t)map);put_jump(image+0x1049860,(uintptr_t)noop);put_jump(image+0x2169eb0,(uintptr_t)noop);
    unsigned (ABI *acquire)(void *)=(void *)(image+ACQUIRE);
    unsigned char original[20];memcpy(original,image+ACQUIRE,sizeof(original));
    assert(!runtime_install_havok_guard(NULL,0));
    assert(!runtime_install_havok_guard(image,ACQUIRE+19));
    assert(!runtime_install_havok_guard(image,0x54b8788+7));
    assert(setenv("BB_HAVOK_POOL_GUARD","0",1)==0);
    assert(!runtime_install_havok_guard(image,IMAGE_SIZE));
    assert(unsetenv("BB_HAVOK_POOL_GUARD")==0);
    const unsigned sites[]={ACQUIRE+19,0x1057357,0x1067f82,0x1067fd1,0x1068001,0x106803f,0x106808a,GIVE,0x10569df};
    for(unsigned i=0;i<sizeof(sites)/sizeof(sites[0]);i++){
        image[sites[i]]^=1;assert(!runtime_install_havok_guard(image,IMAGE_SIZE));image[sites[i]]^=1;
        assert(!memcmp(original,image+ACQUIRE,sizeof(original)));
    }
    for(unsigned mode=0;mode<2;mode++) {
        init();check_error=mode==0?8:0;map_error=mode==1?8:0;
        assert(acquire(owner)==1);assert(flags[0]==0 && flags[1]==1);
        assert(acquire(owner)==1);assert(flags[0]==0 && flags[1]==0);
        fflush(NULL);pid_t child=fork();assert(child>=0);
        if(!child){alarm(1);acquire(owner);_exit(1);}
        int status;assert(waitpid(child,&status,0)==child);assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGALRM);
        printf("PASS: original %s failure exhausts the pool; next call spins until interrupted\n",mode==0?"validation":"mapping");
    }
    init();check_error=0;map_error=0;
    for(unsigned i=0;i<10;i++){assert(acquire(owner)==0);assert(flags[0]==1 && flags[1]==1);}
    puts("PASS: original success path returns the borrowed context");
    unsigned char expected_owner[sizeof(owner)];memcpy(expected_owner,owner,sizeof(owner));
    assert(runtime_install_havok_guard(image,IMAGE_SIZE));
    init();
    for(unsigned i=0;i<10;i++){assert(acquire(owner)==0);assert(flags[0]==1 && flags[1]==1);}
    assert(!memcmp(expected_owner,owner,sizeof(owner)));
    for(unsigned mode=0;mode<2;mode++){
        init();check_error=mode==0?8:0;map_error=mode==1?8:0;
        for(unsigned i=0;i<32;i++){
            assert(acquire(owner)==1);assert(flags[0]==1 && flags[1]==1);
            uintptr_t active;memcpy(&active,owner+0x120,8);assert(!active);
        }
        check_error=0;map_error=0;
        assert(acquire(owner)==0);assert(flags[0]==1 && flags[1]==1);
    }
    puts("PASS: both failures preserve the error result, return the borrow, and allow later successful uploads");
    /* Failures before borrowing must not invent a return-to-pool operation. */
    init();memset(image+0x54b8788,0,8);assert(acquire(owner)==1);
    assert(acquire(NULL)==1);
    memcpy(image+0x54b8788,&pool_ptr,8);assert(flags[0]==1 && flags[1]==1);
    memset(owner+0x118,0,8);assert(acquire(owner)==1);assert(flags[0]==1 && flags[1]==1);
    init();memset(resource,0,8);assert(acquire(owner)==1);assert(flags[0]==1 && flags[1]==1);
    /* Do not treat an already active owner as a new borrow. */
    init();check_error=8;void *active=nodes[1];memcpy(owner+0x120,&active,8);flags[1]=0;
    assert(acquire(owner)==1);assert(flags[0]==0 && flags[1]==0);
    init();check_error=0;map_error=8;replace_pool=1;
    assert(acquire(owner)==1);assert(flags[0]==0 && flags[1]==1);
    assert(!*pool_slot);replace_pool=0;*pool_slot=pool_ptr;
    puts("PASS: early exits, pre-existing active owners and a replaced pool are left alone; successful owner bytes match");
    assert(munmap(image,IMAGE_SIZE)==0);
}
