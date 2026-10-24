// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: from Huax-1839/bloodborne-macos (ee3120a, GPL-2.0-or-later), renderer_metal/.

#pragma once

#include <span>
#include <string>
#include <vector>
#include "common/types.h"

namespace Metal {

/// Result of RewritePerVertexInputs.
struct PerVertexRewrite {
    bool ok{};               ///< false: `error` says why; `spirv` is empty
    std::vector<u32> spirv;  ///< the rewritten module (a copy of the input when it had none)
    std::string error;
    u32 flat_inputs{};          ///< inputs read only at vertex 0, turned into a Flat input
    u32 interpolated_inputs{};  ///< inputs reconstructed from barycentrics (see the .cpp)
};

/// True when the module has an Input variable decorated PerVertexKHR (cheap word scan).
bool HasPerVertexInputs(std::span<const u32> spirv);

/// MSL has no per-vertex fragment inputs, SPIRV-Cross refuses `PerVertexKHR`. Rewrites every such
/// fragment input `in T a_p[3]` into ordinary inputs plus a Private array that the entry point
/// fills at its start, so the shader body keeps indexing `a_p[i]`:
///   - only `a_p[0]` is read (the pull-model "P0" of V_INTERP_MOV; exact, bit patterns survive):
///     `in T a` decorated Flat (Metal's provoking vertex is the first vertex) and a_p[i] = a;
///   - otherwise: `in T a` (smooth) plus the BaryCoordKHR input; the three vertex values are
///     solved from A, dA/dx, dA/dy (OpDPdxFine/OpDPdyFine) and the barycentrics' b, db/dx, db/dy.
///     Derivatives of the quad make this exact at every pixel up to float rounding; a degenerate
///     system (collinear gradients, tiny triangle) falls back to a_p[i] = A (flat).
/// Fails for unexpected shapes (other decorations, pointers escaping into calls, no fragment
/// entry point, ...): the caller keeps reporting CompileStatus::PerVertexInput then.
PerVertexRewrite RewritePerVertexInputs(std::span<const u32> spirv);

} // namespace Metal
