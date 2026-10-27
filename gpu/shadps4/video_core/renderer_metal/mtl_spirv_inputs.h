// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: fragment inputs the vertex stage does not write (Metal-native M5, Task 11).

#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>
#include "common/types.h"

namespace Metal {

/// The Locations of the non-builtin Output variables of a vertex module, as a mask of locations
/// 0..63 (an array output covers one location per element). nullopt when an output's location
/// is unknown (no Location decoration, a Block, a location above 63): the caller then assumes
/// every input is written.
std::optional<u64> OutputLocations(std::span<const u32> spirv);

/// Result of ZeroUnwrittenInputs.
struct ZeroInputsRewrite {
    bool ok{};              ///< false: `error` says why; `spirv` is empty
    std::vector<u32> spirv; ///< the rewritten module
    std::string error;
    u32 zeroed{};           ///< Input variables turned into zero-initialised Private variables
};

/// Vulkan lets a fragment shader read an input the vertex shader never writes (the value is
/// undefined); Metal refuses such a pipeline ("Fragment input(s) ... not written by vertex
/// shader"). Turns every Input variable whose Location lies in `locations` (a mask of locations
/// 0..63; for arrays, any element's) into a Private variable initialised to zero, keeping the
/// shader body: OpVariable gets a Private pointer type and an OpConstantNull initialiser, access
/// chains into it get Private pointer types, its decorations go, and it leaves the entry point's
/// interface below SPIR-V 1.4 (from 1.4 on the interface lists every global it uses).
/// Fails when such a variable reaches anything but loads and access chains (calls, copies).
ZeroInputsRewrite ZeroUnwrittenInputs(std::span<const u32> spirv, u64 locations);

/// Duplicate a fragment shader's sole float4 colour output at locations 1 and 2.
/// The original shader and its bindings remain intact. Used to accumulate HDR UI
/// blend coefficients in the original draw, sharing rasterisation and stencil.
ZeroInputsRewrite DuplicateFragmentOutput(std::span<const u32> spirv);

} // namespace Metal
