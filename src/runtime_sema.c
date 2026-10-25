// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
/* Kernel semaphores: positive int32 IDs in 64-bit guest handles, counted tokens and FIFO waiters.
 * Priority-ordered semaphores are approximated by FIFO order. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <time.h>

#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))

typedef struct Waiter {
    int32_t need, result;
    int done;
    HostCond event;
    struct Waiter *next;
} Waiter;

typedef struct Sema {
    int32_t id;
    uint32_t attr;
    int32_t count, initial, maximum;
    unsigned active;
    int deleted;
    Waiter *first;
    struct Sema *next;
} Sema;

static HostMutex lock = HOST_MUTEX_INIT;
static Sema *registry;
static uint32_t next_id=1;
static size_t created, deleted, acquired, signaled, timed_out;

static Sema *find(uint64_t id) {
    for (Sema *s=registry;s;s=s->next) if ((uint64_t)s->id==id) return s;
    return NULL;
}

static void host_check(int e) {
    if (e) { fprintf(stderr,"STOP: semaphore host error %d\n",e); exit(21); }
}

static uint64_t now_ns(void) { return host_monotonic_ns(); }

static ABI int32_t sem_create(void *out,const char *name,uint32_t attr,
                              int32_t initial,int32_t maximum,const void *options) {
    /* libkernel rejects options before accessing any other argument. */
    if (options) return ERR(22);
    if (!out || !name || attr>2 || initial<0 || maximum<=0 || initial>maximum) return ERR(22);
    Sema *s=calloc(1,sizeof(*s));
    if (!s) return ERR(12);
    host_lock(&lock);
    if (next_id>INT32_MAX) { host_unlock(&lock); free(s); return ERR(28); }
    s->id=(int32_t)next_id++; s->attr=attr; s->count=s->initial=initial; s->maximum=maximum;
    s->next=registry; registry=s;
    /* The native wrapper sign-extends its int32 result and stores eight bytes.
       memcpy also admits an unaligned guest output slot. */
    const int64_t handle=s->id;
    memcpy(out,&handle,sizeof(handle)); ++created;
    printf("Runtime: semaphore id=%d, attr=%u, initial=%d, max=%d\n",s->id,attr,initial,maximum);
    host_unlock(&lock);
    return 0;
}

static int32_t wait_count(uint64_t id,int32_t need,uint32_t *timeout,int block) {
    host_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (need<=0 || need>s->maximum) result=ERR(22);
    else if (s->count>=need) { s->count-=need; ++acquired; }
    else if (!block) result=ERR(16);
    else if (timeout && !*timeout) { result=ERR(60); ++timed_out; }
    else {
        /* Priority-ordered semaphores (attr 2) are served FIFO: there is no
           PS4 priority scheduler, host threads run at equal priority. */
        if (getenv("BB_TRACE_SEMA")) fprintf(stderr,"Runtime: blocking wait on semaphore %" PRIu64 " (need %d, count %d)\n",id,need,s->count);
        Waiter w={.need=need};
#ifdef __APPLE__
        host_cond_init(&w.event); /* macOS: realtime clock only; waits below are relative */
#else
        pthread_condattr_t attr;
        host_check(pthread_condattr_init(&attr));
        host_check(pthread_condattr_setclock(&attr,CLOCK_MONOTONIC));
        host_check(pthread_cond_init(&w.event,&attr));
        host_check(pthread_condattr_destroy(&attr));
#endif
        Waiter **tail=&s->first;
        while (*tail) tail=&(*tail)->next;
        *tail=&w; ++s->active;
        uint64_t deadline=timeout ? now_ns()+(uint64_t)*timeout*1000 : 0;
        runtime_thread_set_blocked("semaphore", id);
        while (!w.done) {
#ifdef __APPLE__
            int e=0;
            if (timeout) {
                uint64_t now=now_ns(),left=now<deadline ? deadline-now : 0;
                struct timespec rel={.tv_sec=(time_t)(left/1000000000),.tv_nsec=(long)(left%1000000000)};
                e=left ? pthread_cond_timedwait_relative_np(&w.event,&lock,&rel) : ETIMEDOUT;
            } else e=pthread_cond_wait(&w.event,&lock);
#else
            struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
            int e=timeout ? pthread_cond_timedwait(&w.event,&lock,&end) : pthread_cond_wait(&w.event,&lock);
#endif
            if (e==ETIMEDOUT && !w.done) {
                Waiter **p=&s->first;
                while (*p!=&w) p=&(*p)->next;
                *p=w.next; w.result=ERR(60); w.done=1; ++timed_out;
            } else if (e!=ETIMEDOUT) host_check(e);
        }
        runtime_thread_clear_blocked();
        result=w.result;
        if (!result) ++acquired;
        if (timeout) {
            uint64_t now=now_ns();
            *timeout=result ? 0 : (uint32_t)(now>=deadline ? 0 : (deadline-now)/1000);
        }
        host_check(pthread_cond_destroy(&w.event));
        if (!--s->active && s->deleted) free(s);
    }
    host_unlock(&lock);
    return result;
}

static ABI int32_t sem_wait(uint64_t id,int32_t need,uint32_t *timeout) {
    int32_t r=wait_count(id,need,timeout,1); RUNTIME_SYNC("sema_wait",id,r); return r;
}
static ABI int32_t sem_poll(uint64_t id,int32_t need) {
    int32_t r=wait_count(id,need,NULL,0); RUNTIME_SYNC("sema_poll",id,r); return r;
}
static ABI int32_t sem_signal(uint64_t id,int32_t count) {
    RUNTIME_SYNC("sema_signal",id,count);
    host_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (count<=0 || count>s->maximum-s->count) result=ERR(22);
    else {
        s->count+=count; ++signaled;
        Waiter **p=&s->first;
        while (*p) {
            Waiter *w=*p;
            if (w->need>s->count) { p=&w->next; continue; }
            s->count-=w->need; *p=w->next; w->done=1;
            host_cond_signal(&w->event);
        }
    }
    host_unlock(&lock);
    return result;
}

static int32_t wake_all(Sema *s,int32_t result) {
    int32_t count=0;
    while (s->first) {
        Waiter *w=s->first; s->first=w->next;
        w->result=result; w->done=1; ++count;
        host_cond_signal(&w->event);
    }
    return count;
}

static ABI int32_t sem_cancel(uint64_t id,int32_t count,int32_t *waiters) {
    host_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (count>s->maximum) result=ERR(22);
    else {
        int32_t n=wake_all(s,ERR(85));
        if (waiters) *waiters=n;
        s->count=count<0 ? s->initial : count;
    }
    host_unlock(&lock);
    return result;
}

static ABI int32_t sem_delete(uint64_t id) {
    host_lock(&lock);
    Sema **p=&registry;
    while (*p && (uint64_t)(*p)->id!=id) p=&(*p)->next;
    if (!*p) { host_unlock(&lock); return ERR(3); }
    Sema *s=*p; *p=s->next; s->deleted=1; ++deleted;
    wake_all(s,ERR(13));
    if (!s->active) free(s);
    host_unlock(&lock);
    return 0;
}

/* Read-only test/diagnostic snapshot, not exposed to guest code. */
unsigned runtime_sema_waiters(uint64_t id) {
    host_lock(&lock);
    Sema *s=find(id); unsigned n=0;
    if (s) for (Waiter *w=s->first;w;w=w->next) ++n;
    host_unlock(&lock);
    return n;
}

uintptr_t runtime_sema_resolve(const char *name) {
    if (!strcmp(name,"188x57JYp0g#p#J")) return (uintptr_t)sem_create;
    if (!strcmp(name,"Zxa0VhQVTsk#p#J")) return (uintptr_t)sem_wait;
    if (!strcmp(name,"4czppHBiriw#p#J")) return (uintptr_t)sem_signal;
    if (!strcmp(name,"12wOHk8ywb0#p#J")) return (uintptr_t)sem_poll;
    if (!strcmp(name,"4DM06U2BNEY#p#J")) return (uintptr_t)sem_cancel;
    if (!strcmp(name,"R1Jvn8bSCW8#p#J")) return (uintptr_t)sem_delete;
    return 0;
}

void runtime_sema_report(void) {
    host_lock(&lock);
    printf("Runtime: semaphores created=%zu, deleted=%zu, acquired=%zu, signals=%zu, timeouts=%zu\n",
           created,deleted,acquired,signaled,timed_out);
    host_unlock(&lock);
}
