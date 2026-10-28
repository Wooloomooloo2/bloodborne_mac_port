// SPDX-License-Identifier: GPL-2.0-or-later
/* Execute the player's original 1.09 dispatcher, with only its external writers
 * replaced by recorders. No game instructions or assets are distributed here.
 * BB_RENDER_GUARD_ELF supplies the private prepared ELF for this opt-in fixture. */
#define _GNU_SOURCE
#include "runtime.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/ucontext.h>

enum { BIND = 0x2169ec0, TAIL = 0x216a014, IDENTITY = 0x26a94c0,
       FINISH = 0x26af700, IMAGE_SIZE = 0x3000000, CONTEXT_SIZE = 0xb800, MAX_CASES = 40 };
typedef uint64_t (ABI *Dispatch)(void *, void *, void *);
typedef struct { uint64_t kind, context, stage, data; } Call;
typedef struct { uint32_t family, type; uint64_t reserved, data; } Resource;
typedef struct { unsigned char context[CONTEXT_SIZE]; Call calls[8]; size_t count; uint64_t result; } Result;
static Call calls[8];
static size_t call_count;

static void record(uint64_t kind, void *context, uint64_t stage, uint64_t data) {
    assert(call_count < sizeof(calls) / sizeof(calls[0]));
    calls[call_count++] = (Call){kind, (uintptr_t)context, stage, data};
}
static uint64_t ABI write_1(void *c, uint64_t s, uint64_t d) { record(1,c,s,d); return 0; }
static uint64_t ABI write_2(void *c, uint64_t s, uint64_t d) { record(2,c,s,d); return 0; }
static uint64_t ABI write_3(void *c, uint64_t s, uint64_t d) { record(3,c,s,d); return 0; }
static uint64_t ABI event(void *c, uint64_t s) { record(4,c,s,0); return 0; }
static uint64_t ABI finish(void *c) {
    record(5,c,0,0);
    ((unsigned char *)c)[0x80] ^= 0x3c;
    return UINT64_C(0x9988776655443321);
}
static void put_jump(unsigned char *p, uintptr_t target) {
    const unsigned char op[] = {0xff,0x25,0,0,0,0};
    memcpy(p, op, sizeof(op)); memcpy(p + sizeof(op), &target, 8);
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
static void writers(unsigned char *image) {
    put_jump(image+0x1077660,(uintptr_t)write_1);
    put_jump(image+0x1077850,(uintptr_t)write_2);
    put_jump(image+0x26bb700,(uintptr_t)write_3);
    put_jump(image+0x1075da0,(uintptr_t)event);
    put_jump(image+FINISH,(uintptr_t)finish);
}
static void capture(Dispatch dispatch, unsigned char *context, Resource *resource, unsigned counter, Result *out) {
    memset(out,0,sizeof(*out)); memset(context,0,CONTEXT_SIZE); memset(calls,0,sizeof(calls));
    context[0xb657]=0xfd;
    memcpy(context+0xb720,&counter,sizeof(counter));
    call_count=0;
    out->result=dispatch(context,context+0xb730,resource);
    memcpy(out->context,context,CONTEXT_SIZE); memcpy(out->calls,calls,sizeof(calls)); out->count=call_count;
    assert(out->count && calls[out->count-1].kind==5);
    assert(calls[out->count-1].context==(uintptr_t)context+0x10);
}
static uintptr_t expected_fault_pc;
static void fatal(int sig, siginfo_t *info, void *opaque) {
    ucontext_t *context = opaque;
#if defined(__APPLE__)
    uintptr_t pc = context->uc_mcontext->__ss.__rip;
#else
    uintptr_t pc = context->uc_mcontext.gregs[REG_RIP];
#endif
    _exit(pc == expected_fault_pc && info->si_addr == NULL ? 128+sig : 2);
}

int main(int argc, char **argv) {
    const char *path=argc>1?argv[1]:getenv("BB_RENDER_GUARD_ELF");
    if (!path || !*path) {
        puts("SKIP: render guard original-code fixture (set BB_RENDER_GUARD_ELF to a private prepared 1.09 ELF)");
        return 0;
    }
    FILE *f=fopen(path,"rb"); assert(f);
    unsigned char *image=mmap(NULL,IMAGE_SIZE,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(image!=MAP_FAILED);
    copy_original(f,image,BIND,0x1b0);
    copy_original(f,image,IDENTITY,4);
    copy_original(f,image,FINISH,17);
    fclose(f);
    unsigned char original[33], original_finish[17];
    memcpy(original,image+BIND,sizeof(original)); memcpy(original_finish,image+FINISH,sizeof(original_finish));
    assert(!runtime_install_render_guard(NULL,0));
    assert(!runtime_install_render_guard(image,FINISH+16));
    assert(setenv("BB_NULL_BIND_GUARD","0",1)==0);
    assert(!runtime_install_render_guard(image,IMAGE_SIZE));
    assert(unsetenv("BB_NULL_BIND_GUARD")==0);
    for (unsigned i=0;i<4;++i) {
        const size_t offsets[]={BIND+30,TAIL+17,IDENTITY+3,FINISH};
        image[offsets[i]]^=1;
        assert(!runtime_install_render_guard(image,IMAGE_SIZE));
        image[offsets[i]]^=1;
        assert(!memcmp(original,image+BIND,sizeof(original)));
    }
    writers(image);
    Dispatch dispatch=(Dispatch)(image+BIND);
    unsigned char *context=malloc(CONTEXT_SIZE); assert(context);
    uint64_t payload[16]={0};
    Resource resource={0,0,0,(uintptr_t)payload};
    Result *expected=calloc(MAX_CASES,sizeof(Result)); assert(expected);
    unsigned families[MAX_CASES], types[MAX_CASES], counters[MAX_CASES], count=0;
    for (unsigned counter=0;counter<2;++counter) {
        for (unsigned family=0;family<3;++family) {
            unsigned type_count=family==0?17:family==1?2:1;
            for (unsigned type=0;type<type_count;++type) {
                assert(count<MAX_CASES);
                resource.family=family; resource.type=type;
                families[count]=family;types[count]=type;counters[count]=counter;
                capture(dispatch,context,&resource,counter,&expected[count++]);
            }
        }
    }
    assert(count==MAX_CASES);
    fflush(NULL);
    pid_t child=fork();assert(child>=0);
    if (!child) {
        expected_fault_pc=(uintptr_t)image+0x2169edc;
        struct sigaction action={0};
        action.sa_sigaction=fatal;action.sa_flags=SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        assert(sigaction(SIGSEGV,&action,NULL)==0 && sigaction(SIGBUS,&action,NULL)==0);
        Result result;
        capture(dispatch,context,NULL,0,&result);
        _exit(1);
    }
    int status;assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status) && (WEXITSTATUS(status)==128+SIGSEGV || WEXITSTATUS(status)==128+SIGBUS));
    puts("PASS: original 1.09 null bind reproduces the reported guest fault");
    memcpy(image+FINISH,original_finish,sizeof(original_finish));
    assert(runtime_install_render_guard(image,IMAGE_SIZE));
    writers(image);
    Result result;
    for (unsigned i=0;i<count;++i) {
        resource.family=families[i];resource.type=types[i];
        capture(dispatch,context,&resource,counters[i],&result);
        assert(!memcmp(&result,&expected[i],sizeof(result)));
    }
    /* family 2 is the original unsupported-type path for the same context. */
    for (unsigned counter=0;counter<2;++counter) {
        capture(dispatch,context,NULL,counter,&result);
        assert(!memcmp(&result,&expected[19+20*counter],sizeof(result)));
    }
    resource.family=0;resource.type=1;
    capture(dispatch,context,&resource,0,&result);
    assert(!memcmp(&result,&expected[1],sizeof(result)));
    puts("PASS: null bind follows existing finish; 40 non-null cases preserve writes, context and return");
    free(expected);free(context);assert(munmap(image,IMAGE_SIZE)==0);
}
