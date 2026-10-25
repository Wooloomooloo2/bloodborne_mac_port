// SPDX-License-Identifier: GPL-2.0-or-later
/* Execute the player's original 1.09 dispatchers and callbacks with recorded
 * external writers; also check the real finalizer's empty-context path.
 * No game instructions or assets are distributed here.
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

enum { QUERY = 0x216a2f0, QUERY_TAIL = 0x216a717, BIND = 0x2169ec0, UNBIND = 0x216a070, TAIL = 0x216a014, UNBIND_TAIL = 0x216a298, IDENTITY = 0x26a94c0,
       FINISH = 0x26af700, IMAGE_SIZE = 0x6000000, CONTEXT_SIZE = 0xb800, MAX_CASES = 60 };
typedef uint64_t (ABI *Dispatch)(void *, void *, void *);
typedef uint32_t (ABI *Query)(void *, void *, const void *, void *, uint32_t, uint8_t);
typedef struct { uint64_t kind, context, stage, data, extra, flags; } Call;
typedef struct { uint32_t family, type; uint64_t reserved, data; } Resource;
typedef struct { unsigned char context[CONTEXT_SIZE]; Call calls[16]; size_t count; uint64_t result, payload[16], output[4]; } Result;
static Call calls[16];
static size_t call_count;

static void record(uint64_t kind, void *context, uint64_t stage, uint64_t data) {
    assert(call_count < sizeof(calls) / sizeof(calls[0]));
    calls[call_count++] = (Call){kind, (uintptr_t)context, stage, data, 0, 0};
}
static uint64_t ABI write_1(void *c, uint64_t s, uint64_t d) { record(1,c,s,d); return 0; }
static uint64_t ABI write_2(void *c, uint64_t s, uint64_t d) { record(2,c,s,d); return 0; }
static uint64_t ABI write_3(void *c, uint64_t s, uint64_t d) { record(3,c,s,d); return 0; }
static uint64_t ABI event(void *c, uint64_t s) { record(4,c,s,0); return 0; }
static uint64_t ABI release_record(void *c, uint64_t s, uint64_t d, uint64_t extra, uint64_t flags) {
    record(6,c,s,d); calls[call_count-1].extra=extra; calls[call_count-1].flags=flags; return 0;
}
static uint64_t ABI flush_context(void *c) {
    record(7,c,0,0);((unsigned char *)c)[0x80]^=0x3c;return 0;
}
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
    put_jump(image+0x26bb6d0,(uintptr_t)release_record);
    put_jump(image+FINISH,(uintptr_t)finish);
    put_jump(image+0x26afa00,(uintptr_t)flush_context);
}
static void begin_capture(unsigned char *context, Resource *resource, unsigned counter, Result *out) {
    memset(out,0,sizeof(*out)); memset(context,0,CONTEXT_SIZE); memset(calls,0,sizeof(calls));
    context[0xb657]=0xfd;
    memcpy(context+0xb720,&counter,sizeof(counter));
    if(resource) memset((void *)(uintptr_t)resource->data,0xa5,sizeof(out->payload));
    call_count=0;
}
static void end_capture(unsigned char *context, Resource *resource, Result *out) {
    memcpy(out->context,context,CONTEXT_SIZE); memcpy(out->calls,calls,sizeof(calls)); out->count=call_count;
    if(resource) memcpy(out->payload,(void *)(uintptr_t)resource->data,sizeof(out->payload));
    assert(out->count && calls[out->count-1].kind==5);
    assert(calls[out->count-1].context==(uintptr_t)context+0x10);
}
static void capture(Dispatch dispatch, unsigned char *context, Resource *resource, unsigned counter, Result *out) {
    begin_capture(context,resource,counter,out);
    out->result=dispatch(context,context+0xb730,resource);
    end_capture(context,resource,out);
}
static uint64_t call_pair(unsigned char *image, unsigned char *context, Resource *resource) {
    typedef uint64_t (ABI *Callback)(void *, void *);
    struct { Resource *record; uint32_t state, pad; uint64_t value; } record={resource,0,0,0};
    uint64_t job[2]={0,(uintptr_t)context};
    uint64_t first=((Callback)(image+0xe2f130))(&record,job);
    assert((first&255)==1 && record.state==2);
    uint64_t result=((Callback)(image+0xe2f160))(&record,job);
    assert((result&255)==1 && record.state==1);
    if(!resource) {
        typedef uint64_t (ABI *GetValue)(void *,void *,void *);
        uint64_t output=UINT64_C(0xfedcba9876543210);
        uint64_t queried=((GetValue)(image+0xe2f190))(&record,job,&output);
        assert(!(queried&255) && record.state==1 && !record.value && output==UINT64_C(0xfedcba9876543210));
    }
    return result;
}
static void capture_pair(unsigned char *image, unsigned char *context, Resource *resource, unsigned counter, Result *out) {
    begin_capture(context,resource,counter,out);
    out->result=call_pair(image,context,resource);
    end_capture(context,resource,out);
}
static void capture_real_finish(Dispatch dispatch, unsigned char *context, Resource *resource,
                                unsigned counter, Result *out) {
    begin_capture(context,resource,counter,out);
    const uint64_t cursor=UINT64_C(0x123456789abcdef0);
    memcpy(context+0x20,&cursor,sizeof(cursor));
    out->result=dispatch(context,context+0xb730,resource);
    memcpy(out->context,context,CONTEXT_SIZE);
    if(resource) memcpy(out->payload,(void *)(uintptr_t)resource->data,sizeof(out->payload));
    assert(!call_count && u64(context+0xb4c8)==cursor);
}
static void capture_query(Query query, unsigned char *context, Resource *resource, unsigned flush, int with_output, Result *out) {
    begin_capture(context,resource,0,out);
    if(resource) memset((void *)(uintptr_t)resource->data,0,sizeof(out->payload));
    uint64_t output[4];memset(output,0xcc,sizeof(output));
    out->result=query(context,context+0xb730,resource,with_output?output:NULL,8,(uint8_t)flush);
    memcpy(out->context,context,CONTEXT_SIZE);memcpy(out->calls,calls,sizeof(calls));out->count=call_count;
    if(resource)memcpy(out->payload,(void *)(uintptr_t)resource->data,sizeof(out->payload));
    memcpy(out->output,output,sizeof(output));
}
static uintptr_t expected_fault_pc, expected_fault_context;
static int verify_unbind_registers;
static uintptr_t expected_fault_address;
static void fatal(int sig, siginfo_t *info, void *opaque) {
    ucontext_t *context = opaque;
#if defined(__APPLE__)
    uintptr_t pc = context->uc_mcontext->__ss.__rip;
    uintptr_t rax=context->uc_mcontext->__ss.__rax, rdi=context->uc_mcontext->__ss.__rdi;
    uintptr_t rbx=context->uc_mcontext->__ss.__rbx, rsi=context->uc_mcontext->__ss.__rsi;
#else
    uintptr_t pc = context->uc_mcontext.gregs[REG_RIP];
    uintptr_t rax=context->uc_mcontext.gregs[REG_RAX], rdi=context->uc_mcontext.gregs[REG_RDI];
    uintptr_t rbx=context->uc_mcontext.gregs[REG_RBX], rsi=context->uc_mcontext.gregs[REG_RSI];
#endif
    int registers_match=!verify_unbind_registers ||
        (!rax && !rdi && rbx==expected_fault_context && rsi==expected_fault_context+0xb730);
    _exit(pc == expected_fault_pc && (uintptr_t)info->si_addr == expected_fault_address && registers_match ? 128+sig : 2);
}

static void expect_null_fault(unsigned char *image, unsigned char *context, unsigned offset, int pair) {
    fflush(NULL);
    pid_t child=fork();assert(child>=0);
    if(!child) {
        expected_fault_pc=(uintptr_t)image+offset;
        expected_fault_address=offset==0x216a31b?4:0;
        expected_fault_context=(uintptr_t)context;verify_unbind_registers=offset==0x216a08c;
        struct sigaction action={0};action.sa_sigaction=fatal;action.sa_flags=SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        assert(sigaction(SIGSEGV,&action,NULL)==0 && sigaction(SIGBUS,&action,NULL)==0);
        Result result;
        if(pair) capture_pair(image,context,NULL,0,&result);
        else capture((Dispatch)(image+(offset==0x2169edc?BIND:UNBIND)),context,NULL,0,&result);
        _exit(1);
    }
    int status;assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status) && (WEXITSTATUS(status)==128+SIGSEGV || WEXITSTATUS(status)==128+SIGBUS));
}
int main(int argc, char **argv) {
    const char *path=argc>1?argv[1]:getenv("BB_RENDER_GUARD_ELF");
    if(!path || !*path) {
        puts("SKIP: render guard original-code fixture (set BB_RENDER_GUARD_ELF to a private prepared 1.09 ELF)");
        return 0;
    }
    FILE *f=fopen(path,"rb");assert(f);
    unsigned char *image=mmap(NULL,IMAGE_SIZE,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(image!=MAP_FAILED);
    copy_original(f,image,BIND,0x1b0);copy_original(f,image,UNBIND,0x27c);
    copy_original(f,image,QUERY,0x480);copy_original(f,image,0xe2f130,0xcd);copy_original(f,image,IDENTITY,4);
    copy_original(f,image,FINISH,0x300);fclose(f);
    static uint64_t canary=UINT64_C(0x9988776655443321);
    uintptr_t cookie=(uintptr_t)&canary;memcpy(image+0x53e3be0,&cookie,8);
    unsigned char original[2][33], query_entry[56], original_finish[0x300];
    memcpy(original[0],image+BIND,33);memcpy(original[1],image+UNBIND,33);
    memcpy(original_finish,image+FINISH,sizeof(original_finish));memcpy(query_entry,image+QUERY,sizeof(query_entry));
    unsigned char *context=malloc(CONTEXT_SIZE);assert(context);
    /* Compile this fixture with origin/master's guard to reproduce the report's
     * exact bind-then-unbind failure before testing the extended guard. */
    if(argc>2 && !strcmp(argv[2],"--baseline-pair")) {
        assert(runtime_install_render_guard(image,IMAGE_SIZE));writers(image);
        expect_null_fault(image,context,0x216a08c,1);
        puts("PASS: bind-only guard skips the null bind, then the original callback faults at 0x216a08c");
        return 0;
    }
    if(argc>2 && !strcmp(argv[2],"--baseline-query")) {
        assert(runtime_install_render_guard(image,IMAGE_SIZE));writers(image);
        expect_null_fault(image,context,0x216a31b,1);
        puts("PASS: bind/unbind-only guards reach the next original query fault at 0x216a31b, address 4");
        return 0;
    }
    assert(!runtime_install_render_guard(NULL,0));
    assert(!runtime_install_render_guard(image,FINISH+16));
    assert(setenv("BB_NULL_BIND_GUARD","0",1)==0);
    assert(!runtime_install_render_guard(image,IMAGE_SIZE));
    assert(unsetenv("BB_NULL_BIND_GUARD")==0);
    const size_t offsets[]={BIND+32,TAIL+17,IDENTITY+3,FINISH};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
        image[offsets[i]]^=1;assert(!runtime_install_render_guard(image,IMAGE_SIZE));image[offsets[i]]^=1;
        assert(!memcmp(original[0],image+BIND,33));assert(!memcmp(original[1],image+UNBIND,33));
    }
    const size_t unbind_offsets[]={UNBIND+33,UNBIND_TAIL+17};
    for(unsigned i=0;i<2;i++) {
        image[unbind_offsets[i]]^=1;
        unsigned char changed[33];memcpy(changed,image+UNBIND,sizeof(changed));
        assert(runtime_install_render_guard(image,IMAGE_SIZE));
        assert(memcmp(original[0],image+BIND,33) && !memcmp(changed,image+UNBIND,sizeof(changed)));
        memcpy(image+BIND,original[0],33);memcpy(image+QUERY,query_entry,sizeof(query_entry));image[unbind_offsets[i]]^=1;
    }
    const size_t query_offsets[]={QUERY+55,QUERY_TAIL+4};
    for(unsigned i=0;i<2;i++) {
        image[query_offsets[i]]^=1;unsigned char changed[56];memcpy(changed,image+QUERY,sizeof(changed));
        assert(runtime_install_render_guard(image,IMAGE_SIZE));
        assert(!memcmp(changed,image+QUERY,sizeof(changed)));
        assert(memcmp(original[0],image+BIND,33) && memcmp(original[1],image+UNBIND,33));
        memcpy(image+BIND,original[0],33);memcpy(image+UNBIND,original[1],33);image[query_offsets[i]]^=1;
    }
    writers(image);
    Dispatch dispatch[2]={(Dispatch)(image+BIND),(Dispatch)(image+UNBIND)};
    uint64_t payload[16]={0};Resource resource={0,0,0,(uintptr_t)payload};
    Result *expected[2]={calloc(MAX_CASES,sizeof(Result)),calloc(MAX_CASES,sizeof(Result))};
    assert(expected[0] && expected[1]);
    unsigned families[MAX_CASES],types[MAX_CASES],counters[MAX_CASES],count=0;
    for(unsigned counter=0;counter<3;counter++) {
        for(unsigned family=0;family<3;family++) {
            unsigned type_count=family==0?17:family==1?2:1;
            for(unsigned type=0;type<type_count;type++) {
                assert(count<MAX_CASES);resource.family=family;resource.type=type;
                families[count]=family;types[count]=type;counters[count]=counter;
                for(unsigned stage=0;stage<2;stage++)capture(dispatch[stage],context,&resource,counter,&expected[stage][count]);
                count++;
            }
        }
    }
    assert(count==MAX_CASES);
    const unsigned query_types[]={0,2,16};Result expected_queries[3][2][2];
    for(unsigned type=0;type<3;type++) {
        resource.type=query_types[type];
        for(unsigned flush=0;flush<2;flush++)
            for(unsigned output=0;output<2;output++)
                capture_query((Query)(image+QUERY),context,&resource,flush,(int)output,&expected_queries[type][flush][output]);
    }
    Result expected_pair[3];resource.family=2;
    for(unsigned counter=0;counter<3;counter++)capture_pair(image,context,&resource,counter,&expected_pair[counter]);
    Result expected_followup[3];
    for(unsigned counter=0;counter<3;counter++) {
        resource.family=2;capture_pair(image,context,&resource,counter,&expected_followup[counter]);
        resource.family=0;resource.type=1;
        expected_followup[counter].result=call_pair(image,context,&resource);
        end_capture(context,&resource,&expected_followup[counter]);
    }
    resource.family=2;
    Result expected_real[2][3];memcpy(image+FINISH,original_finish,sizeof(original_finish));
    for(unsigned stage=0;stage<2;stage++)
        for(unsigned counter=0;counter<3;counter++)
            capture_real_finish(dispatch[stage],context,&resource,counter,&expected_real[stage][counter]);
    expect_null_fault(image,context,0x2169edc,0);expect_null_fault(image,context,0x216a08c,0);
    puts("PASS: original null bind/unbind fault at the exact reported instructions");
    assert(runtime_install_render_guard(image,IMAGE_SIZE));writers(image);
    Result result;
    for(unsigned i=0;i<count;i++) {
        resource.family=families[i];resource.type=types[i];
        for(unsigned stage=0;stage<2;stage++) {
            capture(dispatch[stage],context,&resource,counters[i],&result);
            assert(!memcmp(&result,&expected[stage][i],sizeof(result)));
        }
    }
    for(unsigned counter=0;counter<3;counter++) {
        for(unsigned stage=0;stage<2;stage++) {
            capture(dispatch[stage],context,NULL,counter,&result);
            Result unsupported=expected[stage][19+20*counter];memset(unsupported.payload,0,sizeof(unsupported.payload));
            assert(!memcmp(&result,&unsupported,sizeof(result)));
        }
        capture_pair(image,context,NULL,counter,&result);
        memset(expected_pair[counter].payload,0,sizeof(expected_pair[counter].payload));
        assert(!memcmp(&result,&expected_pair[counter],sizeof(result)));
        assert(result.count==2 && calls[0].kind==5 && calls[1].kind==5);
        memset(payload,0xa5,sizeof(payload));resource.family=0;resource.type=1;
        result.result=call_pair(image,context,&resource);
        end_capture(context,&resource,&result);
        assert(!memcmp(&result,&expected_followup[counter],sizeof(result)));
    }
    resource.family=0;resource.type=1;
    for(unsigned stage=0;stage<2;stage++) {
        capture(dispatch[stage],context,&resource,1,&result);
        assert(!memcmp(&result,&expected[stage][21],sizeof(result)));
    }
    puts("PASS: 120 non-null bind/unbind cases preserve payload, context, writers and return values");
    puts("PASS: missing-record callbacks preserve phase transitions, finalize twice and leave counters unchanged; later valid calls work");
    for(unsigned type=0;type<3;type++) {
        resource.type=query_types[type];
        for(unsigned flush=0;flush<2;flush++) {
            for(unsigned output=0;output<2;output++) {
                capture_query((Query)(image+QUERY),context,&resource,flush,(int)output,&result);
                assert(!memcmp(&result,&expected_queries[type][flush][output],sizeof(result)));
            }
        }
    }
    for(unsigned flush=0;flush<2;flush++) {
        for(unsigned output=0;output<2;output++) {
            capture_query((Query)(image+QUERY),context,NULL,flush,(int)output,&result);
            assert(result.result==7 && !memcmp(&result,&expected_queries[2][flush][output],sizeof(result)));
        }
    }
    puts("PASS: 12 non-null queries match original behavior; null queries return unsupported, preserve flush and leave outputs untouched");
    memcpy(image+FINISH,original_finish,sizeof(original_finish));
    for(unsigned stage=0;stage<2;stage++) {
        for(unsigned counter=0;counter<3;counter++) {
            capture_real_finish(dispatch[stage],context,NULL,counter,&result);
            memset(expected_real[stage][counter].payload,0,sizeof(result.payload));
            assert(!memcmp(&result,&expected_real[stage][counter],sizeof(result)));
        }
    }
    puts("PASS: null bind/unbind match unsupported records through the real finalizer's empty-context path");
    free(expected[0]);free(expected[1]);free(context);assert(munmap(image,IMAGE_SIZE)==0);
}
