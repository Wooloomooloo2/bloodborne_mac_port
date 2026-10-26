// SPDX-License-Identifier: GPL-2.0-or-later
// Non-null stale vertex sharps recover locally only without an enclosing worker recovery.
#include <array>
#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sys/mman.h>
#include <unistd.h>
#include "bbport_threads.h"
#include "shader_recompiler/specialization.h"

extern "C" {
__thread BbRecoverBuf* runtime_fault_recover = nullptr;
uint64_t runtime_disabled_optimizations = 0;
}

namespace {
volatile sig_atomic_t faults = 0;
void Fault(int sig, siginfo_t* detail, void*) {
    if (auto* recover = runtime_fault_recover) {
        faults = faults + 1;
        runtime_fault_recover = nullptr;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
        siglongjmp(*recover, 1);
    }
    std::fprintf(stderr, "unhandled descriptor fault at %p\n", detail->si_addr);
    _exit(128 + sig);
}

void SetPointer(std::span<u32> words, u32 reg, const void* pointer) {
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    std::memcpy(words.data() + reg, &address, sizeof(address));
}

void ExpectZero(const AmdGpu::Buffer& buffer) {
    const AmdGpu::Buffer zero{};
    assert(std::memcmp(&buffer, &zero, sizeof(buffer)) == 0);
}

// Match GetProgram's draw-preparation worker: a guest fault abandons preparation,
// and the GPU thread retries the draw. Keep the callable and locals trivial so
// recovery does not skip destructors in this fixture.
template <typename Read>
bool PreparedRead(Read&& read) {
    assert(runtime_fault_recover == nullptr);
    BbRecoverBuf recover;
    if (BB_RECOVER_SET(recover)) {
        assert(runtime_fault_recover == nullptr);
        return false;
    }
    runtime_fault_recover = &recover;
    read();
    assert(runtime_fault_recover == &recover);
    runtime_fault_recover = nullptr;
    return true;
}
} // namespace

int main() {
    struct sigaction action{};
    action.sa_sigaction = Fault;
    action.sa_flags = SA_SIGINFO;
    assert(sigaction(SIGSEGV, &action, nullptr) == 0);
    assert(sigaction(SIGBUS, &action, nullptr) == 0);

    // s_load_dwordx4 s[8:11], s[2:3], 0
    // buffer_load_format_xyzw v[4:7], v0, s[8:11], 0 idxen
    // s_setpc_b64 s[0:1]
    const u32 fetch[] = {0xc0840300, 0xe00c2000, 0x80020400, 0xbe802000};
    std::array<u32, 16> ud{};
    SetPointer(ud, 0, fetch);
    Shader::Info info{};
    info.hw_stage = Shader::HwStage::Vertex;
    info.sw_stage = Shader::SwStage::Vertex;
    info.has_fetch_shader = true;
    info.user_data = ud;
    const Shader::Profile profile{};
    const auto specialize = [&] {
        return Shader::StageSpecialization(info, Shader::RuntimeInfo{}, profile, {});
    };

    // Preserve master's null-table behavior from 0.5.10 (#74/#97).
    std::puts("specialize null and non-null stale vertex descriptor tables");
    std::fflush(stdout);
    auto null_spec = specialize();
    assert(null_spec.fetch_shader_data && null_spec.fetch_shader_data->attributes.size() == 1);
    assert(null_spec.vs_attribs.size() == 1);
    ExpectZero(null_spec.fetch_shader_data->attributes[0].GetSharp(info));

    AmdGpu::Buffer descriptor{};
    descriptor.base_address = 0x123400;
    descriptor.stride = 16;
    descriptor.num_records = 8;
    descriptor.data_format = static_cast<u32>(AmdGpu::DataFormat::Format32_32_32_32);
    descriptor.num_format = static_cast<u32>(AmdGpu::NumberFormat::Float);
    descriptor.dst_sel_x = 4;
    descriptor.dst_sel_y = 5;
    descriptor.dst_sel_z = 6;
    descriptor.dst_sel_w = 7;
    alignas(8) std::array<u32, 8> table{};
    std::memcpy(table.data(), &descriptor, sizeof(descriptor));
    SetPointer(ud, 2, table.data());
    auto valid_spec = specialize();
    assert(valid_spec.fetch_shader_data->attributes[0].GetSharp(info) == descriptor);
    assert(valid_spec.vs_attribs[0].dst_select == descriptor.DstSelect());

    // Tagged pointers and direct SGPR descriptors retain their original values.
    ud[3] |= 0xabcd0000;
    assert(info.ReadUdReg<AmdGpu::Buffer>(2, 0) == descriptor);
    std::memcpy(ud.data() + 8, &descriptor, sizeof(descriptor));
    assert(info.ReadUdReg<AmdGpu::Buffer>(Shader::IR::NumScalarRegs, 8) == descriptor);
    ExpectZero(info.ReadUdReg<AmdGpu::Buffer>(Shader::IR::NumScalarRegs, 15));
    ExpectZero(info.ReadUdReg<AmdGpu::Buffer>(15, 0));
    ExpectZero(info.ReadUdReg<AmdGpu::Buffer>(std::numeric_limits<u32>::max(), 0));

    // Check the base before adding the offset, including tagged low pointers. No
    // signal/recovery is needed for these even on speculative workers.
    for (uintptr_t base : {uintptr_t{0}, uintptr_t{8},
                           uintptr_t{Shader::Info::MinTableAddress - 1}}) {
        SetPointer(ud, 2, reinterpret_cast<const void*>(base | (0xabcdULL << 48)));
        const auto before = faults;
        ExpectZero(info.ReadUdReg<AmdGpu::Buffer>(2, std::numeric_limits<u32>::max()));
        assert(PreparedRead([&] { ExpectZero(info.ReadUdReg<AmdGpu::Buffer>(2, 0)); }));
        assert(faults == before);
    }

    const auto page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto* pages = static_cast<u8*>(mmap(nullptr, page_size * 2, PROT_READ | PROT_WRITE,
                                      MAP_ANON | MAP_PRIVATE, -1, 0));
    assert(pages != MAP_FAILED);
    std::memset(pages, 0xa5, page_size);
    assert(mprotect(pages + page_size, page_size, PROT_NONE) == 0);
    // A stale non-null pointer, and a descriptor straddling a readable page and a hole.
    for (const void* address : {pages + page_size, pages + page_size - 8}) {
        SetPointer(ud, 2, address);
        ExpectZero(info.ReadUdReg<AmdGpu::Buffer>(2, 0));
        assert(runtime_fault_recover == nullptr);
    }
    assert(faults == 2);

    // Stale descriptor reads must reach the worker's recovery, rather than
    // returning a zero V# and continuing with the wrong permutation/format.
    for (const void* address : {pages + page_size, pages + page_size - 8}) {
        SetPointer(ud, 2, address);
        auto observed = descriptor;
        assert(!PreparedRead([&] { observed = info.ReadUdReg<AmdGpu::Buffer>(2, 0); }));
        assert(observed == descriptor);
    }
    assert(faults == 4);
    SetPointer(ud, 2, pages + page_size);
    auto stale_spec = specialize();
    assert(stale_spec.vs_attribs.size() == 1);

    // Recovery is per read: replacing the stale pointer with a valid table works next draw.
    SetPointer(ud, 2, table.data());
    assert(specialize().vs_attribs == valid_spec.vs_attribs);
    assert(PreparedRead([&] { assert(info.ReadUdReg<AmdGpu::Buffer>(2, 0) == descriptor); }));
    // The same stale address works again once its memory is readable.
    assert(mprotect(pages + page_size, page_size, PROT_READ | PROT_WRITE) == 0);
    std::memcpy(pages + page_size, &descriptor, sizeof(descriptor));
    SetPointer(ud, 2, pages + page_size);
    assert(PreparedRead([&] { assert(info.ReadUdReg<AmdGpu::Buffer>(2, 0) == descriptor); }));
    assert(mprotect(pages + page_size, page_size, PROT_NONE) == 0);
    SetPointer(ud, 2, table.data());

    // The second guest read used by tessellation specialization is guarded too.
    Shader::TessellationDataConstantBuffer constants{};
    constants.ls_stride = 64;
    descriptor.base_address = reinterpret_cast<uintptr_t>(&constants);
    std::memcpy(table.data(), &descriptor, sizeof(descriptor));
    info.tess_consts_ptr_base = static_cast<Shader::IR::ScalarReg>(2);
    info.tess_consts_dword_offset = 0;
    Shader::TessellationDataConstantBuffer readback{};
    info.ReadTessConstantBuffer(readback);
    assert(std::memcmp(&readback, &constants, sizeof(constants)) == 0);
    descriptor.base_address = reinterpret_cast<uintptr_t>(pages + page_size);
    std::memcpy(table.data(), &descriptor, sizeof(descriptor));
    info.ReadTessConstantBuffer(readback);
    const Shader::TessellationDataConstantBuffer zero_constants{};
    assert(std::memcmp(&readback, &zero_constants, sizeof(readback)) == 0);
    assert(!PreparedRead([&] { info.ReadTessConstantBuffer(readback); }));
    // Check the pointer to the descriptor itself, not just its constant data.
    SetPointer(ud, 2, pages + page_size);
    assert(!PreparedRead([&] { info.ReadTessConstantBuffer(readback); }));
    SetPointer(ud, 2, table.data());
    for (uintptr_t base : {uintptr_t{0}, uintptr_t{8},
                           uintptr_t{Shader::Info::MinTableAddress - 1}}) {
        descriptor.base_address = base;
        std::memcpy(table.data(), &descriptor, sizeof(descriptor));
        const auto before = faults;
        assert(PreparedRead([&] { info.ReadTessConstantBuffer(readback); }));
        assert(std::memcmp(&readback, &zero_constants, sizeof(readback)) == 0);
        assert(faults == before);
    }
    SetPointer(ud, 2, nullptr);
    info.ReadTessConstantBuffer(readback);
    assert(std::memcmp(&readback, &zero_constants, sizeof(readback)) == 0);
    descriptor.base_address = 0x123400;
    std::memcpy(table.data(), &descriptor, sizeof(descriptor));
    SetPointer(ud, 2, table.data());

    // Draw recording reads the installed snapshot, even if live registers are stale.
    std::array<u32, 16> live{};
    info.user_data = live;
    Shader::Info::ud_snapshots[0] = {&info, ud.data(), static_cast<u32>(ud.size()), nullptr, 0, 0};
    Shader::Info::num_ud_snapshots = 1;
    assert(info.ReadUdReg<AmdGpu::Buffer>(2, 0) == descriptor);
    Shader::Info::num_ud_snapshots = 0;
    assert(munmap(pages, page_size * 2) == 0);
    std::puts("PASS: shader userdata low/null fast paths, GPU recovery, worker fallback, bounds, tagged reads, snapshots and tessellation");
}
