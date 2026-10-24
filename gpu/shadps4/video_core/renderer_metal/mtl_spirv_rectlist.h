// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: from Huax-1839/bloodborne-macos (ee3120a, GPL-2.0-or-later), renderer_metal/.

#pragma once

#include <span>
#include <string>
#include <vector>
#include "common/types.h"

namespace Metal {

/// Result of RewriteRectListVertex.
struct RectListRewrite {
    bool ok{};               ///< false: `error` says why; `spirv` is empty
    std::vector<u32> spirv;  ///< the rewritten vertex shader
    std::string error;
    u32 leaf_outputs{};      ///< scalar / vector output leaves that are interpolated or copied
};

/// GCN RectList = three vertices v0, v1, v2 of a screen-aligned rectangle; the hardware adds the
/// 4th corner v3 = va + vb - vc (vc the right-angle corner) and interpolates every output the same
/// way. Metal has no such topology and a vertex function cannot see another vertex's outputs, so
/// the vertex shader is rewritten to evaluate all three vertices itself (the Vulkan backend does
/// the same in a tessellation control shader, EmitRectListTCS):
///   - the original entry function stays as an ordinary function; its VertexIndex builtin becomes
///     a Private variable. A new entry point (same name and interface otherwise) reads the real
///     VertexIndex `vid`, sets the index to `(vid / 6) * 3 + j` and calls the original function
///     for j = 0, 1, 2, reading every Output leaf (gl_PerVertex members, arrays, params) after
///     each call;
///   - from the three Positions it finds the corner (equal x / y coordinates, as the TCS does),
///     computes out3 = sum(b_j out_j) with b = -1 for the corner and +1 else for every float
///     scalar / vector leaf; integer leaves keep the corner vertex's value;
///   - vertex `k = vid % 6` of rectangle r outputs: triangle 1 = (c, a, b), triangle 2 =
///     (a, v3, b), c the corner and a, b the vertices after it in the TCS order, so the
///     rectangle is drawn as 6 vertices per 3 guest vertices (non-indexed, vertexStart = 2 *
///     first_vertex, vertexCount = num_vertices / 3 * 6; first_vertex a multiple of 3).
/// InstanceIndex is untouched. Fails for shapes it cannot do: Input variables other than the
/// VertexIndex / InstanceIndex builtins (vertex attributes could not be fetched for the other
/// vertices), stores to memory the three calls would repeat (StorageBuffer, PhysicalStorageBuffer,
/// images, atomics), more than one entry point, terminators like OpKill, SPIR-V below 1.4.
RectListRewrite RewriteRectListVertex(std::span<const u32> spirv);

} // namespace Metal
