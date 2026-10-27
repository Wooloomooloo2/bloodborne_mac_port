// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/types.h"
namespace Shader {
namespace IR {
struct Program;
}
struct RuntimeInfo;
constexpr bool NativeHdrToneShader(u64 hash) { return hash == 0x5c02eaf8 || hash == 0x5e67d866; }
/// Export exposed lighting and unclipped bloom alongside the original SDR output.
/// Only the recognised, straight-line tone shaders with the complete expected dataflow
/// are admitted. Failure leaves the original shader unchanged.
bool ExportNativeHdr(IR::Program& program, const RuntimeInfo& runtime);
} // namespace Shader
