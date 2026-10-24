// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: CompiledShader's out-of-line members (mtl_shader.h), in the Metal-native core so that
// core code (mtl_draw_core.mm) and its unit tests can hold CompiledShaders without the SPIR-V
// compiler.
#import <Metal/Metal.h>

#include "video_core/renderer_metal/mtl_shader.h"

namespace Metal {

CompiledShader::~CompiledShader() {
    // Balance the +1 of __bridge_retained; ARC releases at the end of the scope.
    if (function) {
        id<MTLFunction> released = (__bridge_transfer id<MTLFunction>)function;
        (void)released;
    }
    if (library) {
        id<MTLLibrary> released = (__bridge_transfer id<MTLLibrary>)library;
        (void)released;
    }
}

const ShaderBinding* CompiledShader::Find(u32 set, u32 binding, ShaderBinding::Kind kind) const {
    for (const ShaderBinding& b : bindings) {
        if (b.set == set && b.binding == binding && b.kind == kind) {
            return &b;
        }
    }
    return nullptr;
}

} // namespace Metal
