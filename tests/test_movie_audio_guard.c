// SPDX-License-Identifier: GPL-2.0-or-later
/* Opt-in execution of the player's private 1.09 movie-audio instructions.
 * No game instructions/assets are distributed. External audio and lock calls
 * are recorded; this is a continuation/ABI fixture, not a playback test. */
#define _GNU_SOURCE
#include "runtime.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <unistd.h>

enum { START=0x2822790, STOP=0x2822750, PARENT_START=0x2801c30,
       PARENT_STOP=0x2801cd0, CONTROL_START=0x2900360, IMAGE_SIZE=0x6000000,
       DEVICE_SIZE=0x100c8, OPEN_FLAG=0x100c4, CASES=14 };
typedef uint8_t (ABI *Start)(void *, uint32_t, uint32_t, uint32_t, void *, float);
typedef uint8_t (ABI *Stop)(void *);
typedef uint8_t (ABI *ParentStart)(void *, uint32_t, uint32_t, uint32_t, float);
typedef uint8_t (ABI *ControlStart)(void *, uint32_t, uint32_t, float);
typedef struct { uint64_t kind, args[6]; int32_t volume[8]; } Call;
typedef struct { unsigned char device[DEVICE_SIZE]; Call calls[16]; size_t count; uint8_t result; } Result;
static Call calls[16];
static size_t call_count;
static int32_t open_result, volume_result, close_result;
static unsigned char manager[0xc00], control[0x60];
static uintptr_t manager_vtable[128], lock_vtable[6];
static void *held[3];
static unsigned lock_depth, locks, unlocks, ready=1;

static Call *record(uint64_t kind) {
    assert(call_count < sizeof(calls)/sizeof(*calls));
    Call *c=&calls[call_count++];memset(c,0,sizeof(*c));c->kind=kind;return c;
}
static int32_t ABI audio_open(uint32_t user, uint32_t type, uint32_t index,
                               uint32_t frames, uint32_t rate, uint32_t format) {
    Call *c=record(1);c->args[0]=user;c->args[1]=type;c->args[2]=index;
    c->args[3]=frames;c->args[4]=rate;c->args[5]=format;return open_result;
}
static int32_t ABI audio_volume(int32_t handle, uint32_t mask, const int32_t *volume) {
    Call *c=record(2);c->args[0]=(uint32_t)handle;c->args[1]=mask;
    memcpy(c->volume,volume,sizeof(c->volume));return volume_result;
}
static int32_t ABI audio_output(int32_t handle, const void *buffer) {
    Call *c=record(3);c->args[0]=(uint32_t)handle;c->args[1]=(uintptr_t)buffer;return 0;
}
static int32_t ABI audio_close(int32_t handle) {
    record(4)->args[0]=(uint32_t)handle;return close_result;
}
static uint32_t ABI lock(void *object, uint32_t timeout) {
    assert(timeout==UINT32_MAX && lock_depth<3);
    void *expected=lock_depth==0 && object==control+0x40 ? control+0x40 :
                   lock_depth && held[0]==control+0x40 ?
                       (lock_depth==1 ? manager+0x308 : manager+0xbe8) :
                       (lock_depth==0 ? manager+0x308 : manager+0xbe8);
    assert(object==expected);held[lock_depth++]=object;++locks;return 0;
}
static uint32_t ABI unlock(void *object) {
    assert(lock_depth && held[lock_depth-1]==object);--lock_depth;++unlocks;return 0;
}
static void *ABI singleton(void) { return manager; }
static uint32_t ABI is_ready(void *state) { assert(state==control+0x10);return ready; }
static void put_pointer(unsigned char *p, uintptr_t value) { memcpy(p,&value,8); }
static void put_jump(unsigned char *p, uintptr_t target) {
    const unsigned char op[]={0xff,0x25,0,0,0,0};memcpy(p,op,sizeof(op));put_pointer(p+6,target);
}
static uint64_t u64(const unsigned char *p) { uint64_t n;memcpy(&n,p,8);return n; }
static uint32_t u32(const unsigned char *p) { uint32_t n;memcpy(&n,p,4);return n; }
static uint16_t u16(const unsigned char *p) { uint16_t n;memcpy(&n,p,2);return n; }
static void copy_original(FILE *f, unsigned char *image, uint64_t address, size_t size) {
    unsigned char header[64], ph[56];
    assert(fseek(f,0,SEEK_SET)==0 && fread(header,1,sizeof(header),f)==sizeof(header));
    assert(!memcmp(header,"\177ELF\2\1",6) && u16(header+18)==62 && u16(header+54)>=sizeof(ph));
    for (unsigned i=0;i<u16(header+56);++i) {
        assert(fseek(f,(long)(u64(header+32)+i*u16(header+54)),SEEK_SET)==0);
        assert(fread(ph,1,sizeof(ph),f)==sizeof(ph));
        uint64_t start=u64(ph+16), bytes=u64(ph+32);
        if (u32(ph)==1 && address>=start && address-start<=bytes && size<=bytes-(address-start)) {
            assert(address+size<=IMAGE_SIZE);
            assert(fseek(f,(long)(u64(ph+8)+address-start),SEEK_SET)==0);
            assert(fread(image+address,1,size,f)==size);return;
        }
    }
    assert(!"instruction window is not file backed");
}
static void reset(unsigned char *device) {
    assert(!lock_depth && locks==unlocks);locks=unlocks=0;
    memset(calls,0,sizeof(calls));call_count=0;
    memset(manager,0,sizeof(manager));memset(control,0,sizeof(control));
    put_pointer(manager,(uintptr_t)manager_vtable);
    put_pointer(manager+0x308,(uintptr_t)lock_vtable);
    put_pointer(manager+0xbe8,(uintptr_t)lock_vtable);
    put_pointer(manager+0xbe0,(uintptr_t)device);
    put_pointer(control+0x40,(uintptr_t)lock_vtable);
    float volume=0.375f;memcpy(control+0x38,&volume,sizeof(volume));
    open_result=23;volume_result=close_result=0;ready=1;
}
static void capture(unsigned char *image, unsigned char *device, unsigned which, Result *out) {
    reset(device);memset(device,0xa5,DEVICE_SIZE);device[OPEN_FLAG]=0;
    memset(out,0,sizeof(*out));
    if (which<10) {
        unsigned channels=which%3==0 ? 1 : which%3==1 ? 2 : 8;
        unsigned rate=which%2 ? 44100 : 48000;
        if(which==6)device[OPEN_FLAG]=1;
        if(which==7)open_result=(int32_t)0x80260005;
        if(which==8)volume_result=(int32_t)0x80260001;
        out->result=((Start)(image+START))(device,0x64000,channels,rate,manager,0.125f*(float)(which+1));
        assert(out->result==(which!=6 && which!=7));
        assert(call_count==(which==6 ? 0u : which==7 ? 1u : 2u));
        if(call_count) {
            assert(calls[0].kind==1 && calls[0].args[0]==255 && !calls[0].args[1] && !calls[0].args[2]);
            assert(calls[0].args[3]==1024 && calls[0].args[4]==48000);
            unsigned format=(rate==48000 ? 0 : 3)+(channels==1 ? 0 : channels==2 ? 1 : 2);
            assert(calls[0].args[5]==format);
        }
        if(call_count==2) {
            float scale;memcpy(&scale,image+0x4889e10,4);
            assert(calls[1].kind==2 && calls[1].args[0]==23 && calls[1].args[1]==255);
            for(unsigned i=0;i<8;++i) assert(calls[1].volume[i]==(int32_t)(scale*0.125f*(float)(which+1)));
            for(unsigned i=0x80c4;i<OPEN_FLAG;++i)assert(!device[i]);
        }
    } else {
        uint32_t handle=23;memcpy(device,&handle,4);device[OPEN_FLAG]=which!=10;
        close_result=which==12 ? (int32_t)0x80260001 : 0;
        out->result=((Stop)(image+STOP))(device);
        assert(out->result==(which==10 || which==12));
        assert(!device[OPEN_FLAG] && call_count==(which==10 ? 0u : 2u));
        if(call_count)assert(calls[0].kind==3 && calls[0].args[0]==23 && !calls[0].args[1] && calls[1].kind==4 && calls[1].args[0]==23);
    }
    memcpy(out->device,device,DEVICE_SIZE);memcpy(out->calls,calls,sizeof(calls));out->count=call_count;
}
static uintptr_t fault_pc, fault_device;
static void fatal(int sig, siginfo_t *info, void *opaque) {
    ucontext_t *context=opaque;
#if defined(__APPLE__)
    uintptr_t pc=context->uc_mcontext->__ss.__rip;
    uintptr_t rdi=context->uc_mcontext->__ss.__rdi, rbx=context->uc_mcontext->__ss.__rbx;
#else
    uintptr_t pc=context->uc_mcontext.gregs[REG_RIP];
    uintptr_t rdi=context->uc_mcontext.gregs[REG_RDI], rbx=context->uc_mcontext.gregs[REG_RBX];
#endif
    _exit(pc==fault_pc && rdi==fault_device && rbx==fault_device &&
          (uintptr_t)info->si_addr==fault_device+OPEN_FLAG ? 128+sig : 2);
}
static void expect_fault(unsigned char *image, unsigned entry, uintptr_t pc, uintptr_t device) {
    fflush(NULL);pid_t child=fork();assert(child>=0);
    if(!child) {
        fault_pc=pc;fault_device=device;
        struct sigaction action={0};action.sa_sigaction=fatal;action.sa_flags=SA_SIGINFO;sigemptyset(&action.sa_mask);
        assert(!sigaction(SIGSEGV,&action,NULL) && !sigaction(SIGBUS,&action,NULL));
        reset((void *)device);
        if(entry==CONTROL_START) ((ControlStart)(image+entry))(control,0x64000,2,48000.0f);
        else if(entry==STOP) ((Stop)(image+entry))((void *)device);
        else ((Start)(image+entry))((void *)device,0x64000,2,48000,manager,0.5f);
        _exit(1);
    }
    int status;assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status) && (WEXITSTATUS(status)==128+SIGSEGV || WEXITSTATUS(status)==128+SIGBUS));
}
int main(int argc, char **argv) {
    const char *path=argc>1 ? argv[1] : getenv("BB_MOVIE_AUDIO_GUARD_ELF");
    if(!path || !*path) {
        puts("SKIP: movie-audio original-code fixture (set BB_MOVIE_AUDIO_GUARD_ELF to a private prepared 1.09 ELF)");return 0;
    }
    FILE *f=fopen(path,"rb");assert(f);
    unsigned char *image=mmap(NULL,IMAGE_SIZE,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(image!=MAP_FAILED);
    copy_original(f,image,STOP,0x137);copy_original(f,image,0x2821860,0x25);
    copy_original(f,image,PARENT_START,0x9d);copy_original(f,image,PARENT_STOP,0x75);
    copy_original(f,image,CONTROL_START,0x8c);
    copy_original(f,image,0x27d9fe0,0x43);copy_original(f,image,0x27da030,0x16);
    copy_original(f,image,0x4889e10,4);fclose(f);
    put_jump(image+0x2bbff38,(uintptr_t)audio_open);put_jump(image+0x2bbff48,(uintptr_t)audio_volume);
    put_jump(image+0x2bbff58,(uintptr_t)audio_output);put_jump(image+0x2bbff68,(uintptr_t)audio_close);
    put_jump(image+0x2bbe688,(uintptr_t)memset);put_jump(image+0x27f0230,(uintptr_t)singleton);
    put_jump(image+0x263a1b0,(uintptr_t)is_ready);
    lock_vtable[3]=(uintptr_t)lock;lock_vtable[5]=(uintptr_t)unlock;
    manager_vtable[0x3c0/8]=(uintptr_t)(image+PARENT_START);
    manager_vtable[0x3c8/8]=(uintptr_t)(image+PARENT_STOP);
    expect_fault(image,START,(uintptr_t)image+0x282279d,0);
    expect_fault(image,CONTROL_START,(uintptr_t)image+0x282279d,0);
    expect_fault(image,STOP,(uintptr_t)image+0x2822759,0);
    puts("PASS: original start and movie caller reproduce #123's PC/address/registers; original null stop also faults");
    unsigned char *device=malloc(DEVICE_SIZE);Result *before=calloc(CASES,sizeof(*before)), after;
    assert(device && before);
    for(unsigned i=0;i<CASES;++i)capture(image,device,i,&before[i]);
    unsigned char start[36], stop[29];memcpy(start,image+START,sizeof(start));memcpy(stop,image+STOP,sizeof(stop));
    assert(!runtime_install_movie_audio_guard(NULL,IMAGE_SIZE));
    assert(!runtime_install_movie_audio_guard(image,START+sizeof(start)-1));
    assert(!setenv("BB_MOVIE_AUDIO_GUARD","0",1));assert(!runtime_install_movie_audio_guard(image,IMAGE_SIZE));
    assert(!unsetenv("BB_MOVIE_AUDIO_GUARD"));
    for(unsigned which=0;which<2;++which) {
        unsigned char *entry=image+(which ? STOP : START);
        for(size_t i=0;i<(which ? sizeof(stop) : sizeof(start));++i) {
            entry[i]^=1;assert(!runtime_install_movie_audio_guard(image,IMAGE_SIZE));entry[i]^=1;
            assert(!memcmp(start,image+START,sizeof(start)) && !memcmp(stop,image+STOP,sizeof(stop)));
        }
    }
    assert(runtime_install_movie_audio_guard(image,IMAGE_SIZE));
    for(unsigned i=0;i<CASES;++i) {
        capture(image,device,i,&after);assert(!memcmp(&before[i],&after,sizeof(after)));
    }
    puts("PASS: 14 original non-null start/stop cases preserve returns, device bytes, volume, format, and audio calls");
    reset(NULL);
    assert(!((ParentStart)(image+PARENT_START))(manager,0x64000,2,48000,0.5f));
    assert(locks==2 && unlocks==2 && !lock_depth && !call_count);
    assert(((Stop)(image+PARENT_STOP))(manager)==1);
    assert(locks==4 && unlocks==4 && !lock_depth && !call_count);
    for(unsigned started=0;started<2;++started) {
        reset(NULL);control[0x58]=(unsigned char)started;
        assert(!((ControlStart)(image+CONTROL_START))(control,0x64000,2,48000.0f));
        assert(!control[0x58] && !lock_depth && locks==3+2*started && locks==unlocks && !call_count);
    }
    reset(NULL);ready=0;
    assert(!((ControlStart)(image+CONTROL_START))(control,0x64000,2,48000.0f));
    assert(!locks && !call_count);
    reset(NULL);
    for(unsigned i=0;i<32;++i) {
        assert(!((ControlStart)(image+CONTROL_START))(control,0x64000,2,48000.0f));
        assert(!control[0x58] && !lock_depth && locks==unlocks && !call_count);
    }
    memset(device,0,DEVICE_SIZE);put_pointer(manager+0xbe0,(uintptr_t)device);
    assert(((ControlStart)(image+CONTROL_START))(control,0x64000,2,48000.0f)==1);
    assert(control[0x58] && device[OPEN_FLAG] && !lock_depth && locks==unlocks && call_count==2);
    assert(((Stop)(image+PARENT_STOP))(manager)==0 && !device[OPEN_FLAG] && !lock_depth && locks==unlocks);
    puts("PASS: original callers release every lock, keep failed starts unstarted, and accept a later valid device");
    expect_fault(image,START,(uintptr_t)u64(image+START+6)+19+13,0x10);
    puts("PASS: non-null invalid devices still fault; disable, bounds and both signature checks leave entries untouched");
    free(before);free(device);assert(!munmap(image,IMAGE_SIZE));return 0;
}
