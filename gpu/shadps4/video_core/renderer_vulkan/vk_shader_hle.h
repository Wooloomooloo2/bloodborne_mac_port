// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace AmdGpu {
struct ComputeProgram;
union Regs;
} // namespace AmdGpu

namespace Shader {
struct Info;
}

namespace VideoCore {
class Buffer;
}

namespace Vulkan {

class Rasterizer;

/// Attempts to execute a shader using HLE if possible.
bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer);

/// bbport: copy-shader copies deferred to the end of the render pass they were issued in
/// (vk_shader_hle.cpp). Records them, in order; called when a pass ends.
void FlushDeferredCopies(Rasterizer& rasterizer);
/// Whether an access by a command of the pass must wait for the deferred copies: it reads or
/// writes one of their destinations, or writes one of their sources.
bool DeferredCopiesConflict(const VideoCore::Buffer* buffer, u64 offset, u64 size, bool write);

} // namespace Vulkan
