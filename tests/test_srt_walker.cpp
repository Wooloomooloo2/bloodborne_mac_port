// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// Portable SRT walker (gpu/shadps4/shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp):
// runs hand-written bytecode against tables in memory, on x86-64 (Rosetta) and native arm64.
// Covers pointer chasing with tagged pointers, offset arithmetic, and loads that fault (they
// must read zero, then and on every later walk).
#include <csetjmp>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>

using u32 = uint32_t;
using u64 = uint64_t;

extern "C" __thread sigjmp_buf* runtime_fault_recover;
__thread sigjmp_buf* runtime_fault_recover;
// Data the GPU library imports from the runtime (its functions bind lazily and are not called).
extern "C" uint64_t runtime_disabled_optimizations;
uint64_t runtime_disabled_optimizations;

namespace Shader {
using SrtWalkerCode = const u32*;
SrtWalkerCode RegisterWalkerCode(const unsigned char* ptr, unsigned long size);
void RunSrtWalker(SrtWalkerCode code, const u32* user_data, u32* flat_dst);
} // namespace Shader

namespace {

int faults;

void Fault(int sig, siginfo_t*, void*) {
    ++faults;
    if (sigjmp_buf* recover = runtime_fault_recover) {
        runtime_fault_recover = nullptr;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
        siglongjmp(*recover, 1);
    }
    std::signal(sig, SIG_DFL);
}

enum Op : u32 {
    End, Imm, LoadFlat, Add, Sub, Mul, Shl, Shr, And, Or, Xor, Not, UMin, UMax, Bfe,
    PushPtrImm, PushPtrExpr, CopyImm, CopyExpr, PopPtr,
};

int failures;

void Expect(const char* what, u32 got, u32 want) {
    if (got != want) {
        std::printf("FAIL %s: %#x, expected %#x\n", what, got, want);
        ++failures;
    }
}

} // namespace

int main() {
    struct sigaction action {};
    action.sa_sigaction = Fault;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);

    // Guest-like tables. T[6..7] points into an inaccessible page.
    alignas(8) static u32 inner[8] = {0, 0, 0, 0x33};
    alignas(8) static u32 table[16] = {0x11, 0x22, 0, 0x44, 0, 0, 0, 0, 0x55, 0, 0x66};
    void* hole = mmap(nullptr, 16384, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    const u64 inner_ptr = reinterpret_cast<u64>(inner);
    const u64 hole_ptr = reinterpret_cast<u64>(hole);
    std::memcpy(&table[4], &inner_ptr, 8);
    std::memcpy(&table[6], &hole_ptr, 8);

    u32 user_data[16] = {};
    const u64 tagged = reinterpret_cast<u64>(table) | (0xABCDull << 48); // high bits ignored
    std::memcpy(&user_data[2], &tagged, 8);
    user_data[0] = 7;

    const u32 program[] = {
        0x31545253,
        PushPtrImm, 2, 0,     // cur = table
        CopyImm, 0, 16,       // flat[16] = table[0]
        LoadFlat, 0, 0,       // 7
        Imm, 6, 0,            //
        Sub, 0, 0,            // 7 - 6 = 1
        CopyExpr, 0, 17,      // flat[17] = table[1]
        PushPtrImm, 4, 0,     // cur = inner
        CopyImm, 3, 18,       // flat[18] = inner[3]
        PopPtr, 0, 0,
        PushPtrImm, 6, 0,     // cur = hole: reading through it faults
        CopyImm, 0, 19,       // flat[19] = 0 (fault)
        PopPtr, 0, 0,
        Imm, 0x35, 0,
        Imm, 4, 0,
        Imm, 4, 0,
        Bfe, 0, 0,            // (0x35 >> 4) & 0xf = 3
        CopyExpr, 0, 20,      // flat[20] = table[3]
        Imm, 1, 0,
        Imm, 3, 0,
        Shl, 0, 0,            // 8
        LoadFlat, 16, 0,      // 0x11 (read back from flat)
        Imm, 0x10, 0,
        Xor, 0, 0,            // 1
        Imm, 9, 0,
        UMin, 0, 0,           // 1
        Add, 0, 0,            // 9
        Not, 0, 0,
        Not, 0, 0,            // 9
        Imm, 1, 0,
        Add, 0, 0,            // 10
        CopyExpr, 0, 21,      // flat[21] = table[10]
        PopPtr, 0, 0,
        End, 0, 0,
    };
    const Shader::SrtWalkerCode code = Shader::RegisterWalkerCode(
        reinterpret_cast<const unsigned char*>(program), sizeof(program));

    for (int pass = 0; pass < 3; ++pass) {
        u32 flat[24] = {};
        std::memcpy(flat, user_data, sizeof(user_data));
        Shader::RunSrtWalker(code, user_data, flat);
        Expect("copy imm", flat[16], 0x11);
        Expect("copy expr (sub)", flat[17], 0x22);
        Expect("nested pointer", flat[18], 0x33);
        Expect("faulting load", flat[19], 0);
        Expect("bfe offset", flat[20], 0x44);
        Expect("arithmetic offset", flat[21], 0x66);
        Expect("user data untouched", flat[0], 7);
        Expect("recover pointer restored", runtime_fault_recover != nullptr, 0);
    }
    Expect("faults (first walk only)", faults, 1);

    // A null table (#74) reads zero without a fault, and only for that walk: once the game
    // fills the pointer in, the same load reads the table again.
    const u32 null_program[] = {
        0x31545253,
        PushPtrImm, 2, 0,     // cur = user_data[2..3]
        CopyImm, 1, 16,       // flat[16] = table[1]
        PushPtrImm, 4, 0,     // cur = (table[4..5])
        CopyImm, 3, 17,       // flat[17] = inner[3]
        PopPtr, 0, 0,
        PopPtr, 0, 0,
        End, 0, 0,
    };
    const Shader::SrtWalkerCode null_code = Shader::RegisterWalkerCode(
        reinterpret_cast<const unsigned char*>(null_program), sizeof(null_program));
    const u64 pointers[] = {0, reinterpret_cast<u64>(table), 0, reinterpret_cast<u64>(table)};
    for (const u64 pointer : pointers) {
        u32 null_ud[16] = {};
        std::memcpy(&null_ud[2], &pointer, 8);
        u32 flat[24] = {};
        Shader::RunSrtWalker(null_code, null_ud, flat);
        Expect("null table, outer load", flat[16], pointer ? 0x22 : 0);
        Expect("null table, nested load", flat[17], pointer ? 0x33 : 0);
    }
    Expect("faults (null tables add none)", faults, 1);
    if (failures) {
        return 1;
    }
    std::printf("PASS: SRT walker bytecode (%s): pointers, offsets, faulting load zeroed once, null tables\n",
#if defined(__aarch64__)
                "arm64"
#else
                "x86-64"
#endif
    );
    return 0;
}
