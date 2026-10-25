#define _GNU_SOURCE
#include "runtime.h"
#include <stdlib.h>

/* The settings menu of the GPU library restarts through probe.c, which tests do not link. */
void runtime_restart(void) { abort(); }
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <sys/mman.h>
#include <unistd.h>
typedef int32_t (ABI *Create)(void *,const char *,uint32_t,int32_t,int32_t,const void *);
typedef int32_t (ABI *CountOp)(uint64_t,int32_t);
typedef int32_t (ABI *Wait)(uint64_t,int32_t,uint32_t *);
typedef int32_t (ABI *Delete)(uint64_t);
typedef int32_t (ABI *Cancel)(uint64_t,int32_t,int32_t *);
#ifdef BB_SEMA_STANDALONE
/* The fixture links the real semaphore registry without the GPU/runtime graph. */
#define GET(t,n) ((t)runtime_sema_resolve(n))
void semaphore_id_limit_test(void);
#else
#define GET(t,n) ((t)runtime_resolve(n,0))
#endif
static Create create;
static CountOp poll_sem,signal_sem;
static Wait wait_sem;
static Delete delete_sem;
static Cancel cancel_sem;
static void setup(void) {
#ifndef BB_SEMA_STANDALONE
    runtime_start(1);
#endif
    create=GET(Create,"188x57JYp0g#p#J");
    poll_sem=GET(CountOp,"12wOHk8ywb0#p#J");
    signal_sem=GET(CountOp,"4czppHBiriw#p#J");
    wait_sem=GET(Wait,"Zxa0VhQVTsk#p#J");
    delete_sem=GET(Delete,"R1Jvn8bSCW8#p#J");
    cancel_sem=GET(Cancel,"4DM06U2BNEY#p#J");
    assert(create && poll_sem && signal_sem && wait_sem && delete_sem && cancel_sem);
    assert(!runtime_sema_resolve("188x57JYp0g#I#J"));
#ifndef BB_SEMA_STANDALONE
    assert(!runtime_resolve("188x57JYp0g#I#J",0));
    assert(!runtime_resolve("188x57JYp0g#p#J",1));
#endif
}
static void lifecycle(void) {
    struct { uint64_t before,handle,after; } s={
        UINT64_C(0x1122334455667788),UINT64_C(0xa5a5a5a5a5a5a5a5),UINT64_C(0x8877665544332211)
    };
    assert((uint32_t)create(NULL,"test",1,0,1,NULL)==0x80020016);
    assert((uint32_t)create(&s.handle,"test",1,2,1,NULL)==0x80020016);
    assert(s.handle==UINT64_C(0xa5a5a5a5a5a5a5a5));
    assert(create(&s.handle,"test",2,2,3,NULL)==0);
    assert(s.handle>0 && s.handle<=INT32_MAX);
    assert(s.before==UINT64_C(0x1122334455667788) && s.after==UINT64_C(0x8877665544332211));
    uint64_t old=s.handle;
    uint32_t timeout=1000000;
    assert(wait_sem(s.handle,2,&timeout)==0 && timeout==1000000);
    assert((uint32_t)poll_sem(s.handle,1)==0x80020010);
    timeout=0;
    assert((uint32_t)wait_sem(s.handle,1,&timeout)==0x8002003c);
    assert((uint32_t)signal_sem(s.handle,4)==0x80020016);
    assert((uint32_t)poll_sem(s.handle,0)==0x80020016);
    assert((uint32_t)signal_sem(s.handle,-1)==0x80020016);
    assert(signal_sem(s.handle,3)==0);
    assert((uint32_t)signal_sem(s.handle,INT_MAX)==0x80020016);
    assert(poll_sem(s.handle,3)==0);
    int32_t n=-1;
    assert(cancel_sem(s.handle,-1,&n)==0 && n==0);
    assert(poll_sem(s.handle,2)==0);
    assert(delete_sem(s.handle)==0);
    assert((uint32_t)delete_sem(s.handle)==0x80020003);
    assert((uint32_t)signal_sem(s.handle,1)==0x80020003);
    assert(create(&s.handle,"test",1,0,1,NULL)==0 && s.handle!=old);
    assert((uint32_t)poll_sem(old,1)==0x80020003);
    timeout=1000;
    assert((uint32_t)wait_sem(s.handle,1,&timeout)==0x8002003c && timeout==0);
    assert(runtime_sema_waiters(s.handle)==0);
    assert(signal_sem(s.handle,1)==0 && poll_sem(s.handle,1)==0);
    assert(delete_sem(s.handle)==0);
}
static void wire(void) {
    /* A deliberately odd output address must be written in full, with no
       access to either surrounding byte. Read it back without aligned casts. */
    _Alignas(uint64_t) unsigned char slot[10];
    memset(slot,0xa5,sizeof(slot));
    assert(create(slot+1,"unaligned",1,1,2,NULL)==0);
    uint64_t id;
    memcpy(&id,slot+1,sizeof(id));
    assert(id>0 && id<=INT32_MAX && slot[0]==0xa5 && slot[9]==0xa5);
    const uint64_t mismatched[]={
        id|UINT64_C(0x100000000),id|UINT64_C(0x8000000000000000),
        id|UINT64_C(0xffffffff00000000),UINT64_MAX,0
    };
    for (size_t i=0;i<sizeof(mismatched)/sizeof(*mismatched);++i) {
        uint64_t bad=mismatched[i];
        uint32_t timeout=12345;
        int32_t n=12345;
        assert((uint32_t)wait_sem(bad,1,&timeout)==0x80020003 && timeout==12345);
        assert((uint32_t)poll_sem(bad,1)==0x80020003);
        assert((uint32_t)signal_sem(bad,1)==0x80020003);
        assert((uint32_t)cancel_sem(bad,0,&n)==0x80020003 && n==12345);
        assert((uint32_t)delete_sem(bad)==0x80020003);
        assert(runtime_sema_waiters(bad)==0);
        /* Invalid operations must not acquire tokens, signal, cancel or delete
           the object selected by the same low 32 bits. */
        assert(poll_sem(id,1)==0);
        assert((uint32_t)poll_sem(id,1)==0x80020010);
        assert(signal_sem(id,1)==0);
    }
    assert(delete_sem(id)==0);
}
static void options(void) {
    size_t size=(size_t)sysconf(_SC_PAGESIZE);
    void *guard=mmap(NULL,size,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(guard!=MAP_FAILED);
    /* No name, output or options contents may be accessed on this path. */
    assert((uint32_t)create(guard,guard,2,0,1,guard)==0x80020016);
    assert((uint32_t)create(NULL,NULL,UINT32_MAX,INT_MIN,INT_MIN,guard)==0x80020016);
    uint64_t untouched=UINT64_C(0xa5a5a5a5a5a5a5a5);
    assert((uint32_t)create(&untouched,"options",1,0,1,guard)==0x80020016);
    assert(untouched==UINT64_C(0xa5a5a5a5a5a5a5a5));
    assert(munmap(guard,size)==0);
    /* A rejected create leaves no hidden allocation in the registry. */
    uint64_t id;
    assert(create(&id,"after options",1,0,1,NULL)==0 && id==1);
    assert(delete_sem(id)==0);
}
typedef struct { uint64_t id; int32_t need,result; uint32_t timeout; } Task;
static void *waiter(void *p) { Task *t=p; t->result=wait_sem(t->id,t->need,&t->timeout); return NULL; }
static void enrolled(uint64_t id,unsigned count) {
    const struct timespec nap={0,1000000};
    for (int i=0;i<2000;++i) {
        if (runtime_sema_waiters(id)==count) return;
        nanosleep(&nap,NULL);
    }
    assert(!"waiter enrollment timed out");
}
static void concurrency(void) {
    uint64_t id;
    assert(create(&id,"fifo",1,0,3,NULL)==0);
    Task a={id,1,99,5000000},b={id,1,99,5000000};
    pthread_t first,second;
    assert(pthread_create(&first,NULL,waiter,&a)==0); enrolled(id,1);
    assert(pthread_create(&second,NULL,waiter,&b)==0); enrolled(id,2);
    assert(signal_sem(id,1)==0);
    assert(pthread_join(first,NULL)==0 && a.result==0 && a.timeout<=5000000);
    assert(runtime_sema_waiters(id)==1 && (uint32_t)poll_sem(id,1)==0x80020010);
    assert(signal_sem(id,1)==0);
    assert(pthread_join(second,NULL)==0 && b.result==0);
    /* A request that fits can pass an earlier larger request, matching the
       researched kernel semaphore model. No tokens are partially consumed. */
    a=(Task){id,2,99,5000000}; b=(Task){id,1,99,5000000};
    assert(pthread_create(&first,NULL,waiter,&a)==0); enrolled(id,1);
    assert(pthread_create(&second,NULL,waiter,&b)==0); enrolled(id,2);
    assert(signal_sem(id,1)==0);
    assert(pthread_join(second,NULL)==0 && b.result==0);
    assert(runtime_sema_waiters(id)==1);
    assert(signal_sem(id,2)==0);
    assert(pthread_join(first,NULL)==0 && a.result==0);
    assert(delete_sem(id)==0);
}
static void cancellation(void) {
    uint64_t id;
    assert(create(&id,"cancel",1,0,2,NULL)==0);
    Task a={id,1,99,5000000},b={id,1,99,5000000};
    pthread_t first,second;
    assert(pthread_create(&first,NULL,waiter,&a)==0); enrolled(id,1);
    assert(pthread_create(&second,NULL,waiter,&b)==0); enrolled(id,2);
    int32_t n=-1;
    assert(cancel_sem(id,1,&n)==0 && n==2);
    assert(pthread_join(first,NULL)==0 && (uint32_t)a.result==0x80020055);
    assert(pthread_join(second,NULL)==0 && (uint32_t)b.result==0x80020055);
    assert(a.timeout==0 && b.timeout==0 && poll_sem(id,1)==0);
    a=(Task){id,1,99,5000000}; b=(Task){id,1,99,5000000};
    assert(pthread_create(&first,NULL,waiter,&a)==0); enrolled(id,1);
    assert(pthread_create(&second,NULL,waiter,&b)==0); enrolled(id,2);
    assert(delete_sem(id)==0);
    assert(pthread_join(first,NULL)==0 && (uint32_t)a.result==0x8002000d);
    assert(pthread_join(second,NULL)==0 && (uint32_t)b.result==0x8002000d);
    assert((uint32_t)poll_sem(id,1)==0x80020003);
}
int main(int argc,char **argv) {
    setup();
    if (argc>1 && !strcmp(argv[1],"--priority")) {
        /* Priority semaphores block and are woken in FIFO order. */
        uint64_t id; assert(create(&id,"priority",2,0,1,NULL)==0);
        Task a={id,1,99,5000000}; pthread_t t;
        assert(pthread_create(&t,NULL,waiter,&a)==0); enrolled(id,1);
        assert(signal_sem(id,1)==0);
        assert(pthread_join(t,NULL)==0 && a.result==0);
        puts("PASS: priority semaphore wait"); return 0;
    }
    if (argc>1 && !strcmp(argv[1],"--concurrency")) concurrency();
    else if (argc>1 && !strcmp(argv[1],"--cancel-delete")) cancellation();
    else if (argc>1 && !strcmp(argv[1],"--wire")) wire();
    else if (argc>1 && !strcmp(argv[1],"--options")) options();
#ifdef BB_SEMA_STANDALONE
    else if (argc>1 && !strcmp(argv[1],"--id-limit")) semaphore_id_limit_test();
#endif
    else lifecycle();
    puts("PASS: semaphore contracts");
    return 0;
}
