// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <boost/container/set.hpp>
#include <boost/container/small_vector.hpp>
#include "common/types.h"

namespace Serialization {
struct Archive;
}

namespace Shader {

// bbport: the SRT walker is a small portable bytecode program run by an interpreter
// (flatten_extended_userdata_pass.cpp), not JIT-compiled x86-64 code, so the native arm64 GPU
// host and the x86-64 runtime share one walker and one pipeline-cache format.
using SrtWalkerCode = const u32*;
/// Copies a serialized walker program into permanent storage.
SrtWalkerCode RegisterWalkerCode(const u8* ptr, size_t size);
/// Runs a walker: user_data -> flat_dst (whose first NumUserDataRegs words hold the user data).
void RunSrtWalker(SrtWalkerCode code, const u32* user_data, u32* flat_dst);
/// bbport: diagnostics (the frame capture's event trace): called with each guest address a
/// walker reads, when set.
using SrtReadHook = void (*)(u64 address, u32 size);
extern SrtReadHook srt_read_hook;

struct PersistentSrtInfo {
    // Special case when fetch shader uses step rates.
    struct SrtSharpReservation {
        u32 sgpr_base;
        u32 dword_offset;
        u32 num_dwords;
    };

    SrtWalkerCode walker_func{};
    size_t walker_func_size{}; // bytes
    u32 flattened_bufsize_dw = 16; // NumUserDataRegs

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

} // namespace Shader
