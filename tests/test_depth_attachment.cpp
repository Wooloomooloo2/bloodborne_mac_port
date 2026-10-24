// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// The pipeline key's depth/stencil formats describe the depth buffer exactly as the render pass
// binds it (Metal rejects a pipeline whose depth/stencil pixel formats differ from the pass's
// attachments): attached only while a depth or stencil test uses it, then with each valid aspect.
#include <cassert>
#include <cstdio>
#include <memory>
#include "video_core/renderer_vulkan/vk_depth_attachment.h"

using ZFormat = AmdGpu::DepthBuffer::ZFormat;
using StencilFormat = AmdGpu::DepthBuffer::StencilFormat;

namespace {

struct Case {
    const char* name;
    bool depth_valid, stencil_valid;   // the depth buffer's aspects
    bool depth_enable, stencil_enable; // the tests (DB_DEPTH_CONTROL)
    bool expect_depth, expect_stencil; // the attachment's aspects
};

constexpr Case kCases[] = {
    {"depth+stencil buffer, tests off", true, true, false, false, false, false},
    {"depth+stencil buffer, depth test", true, true, true, false, true, true},
    {"depth+stencil buffer, stencil test", true, true, false, true, true, true},
    {"depth+stencil buffer, both tests", true, true, true, true, true, true},
    {"depth buffer, tests off", true, false, false, false, false, false},
    {"depth buffer, depth test", true, false, true, false, true, false},
    {"depth buffer, stencil test only", true, false, false, true, false, false},
    {"stencil buffer, tests off", false, true, false, false, false, false},
    {"stencil buffer, stencil test", false, true, false, true, false, true},
    {"stencil buffer, depth test only", false, true, true, false, false, false},
    {"no buffer, both tests", false, false, true, true, false, false},
};

} // namespace

int main() {
    auto regs = std::make_unique<AmdGpu::Regs>();
    unsigned failures = 0;
    for (const Case& c : kCases) {
        *regs = {};
        auto& db = regs->depth_buffer;
        db.z_info.format = ZFormat::Z32Float;
        db.stencil_info.format = StencilFormat::Stencil8;
        db.z_read_base = c.depth_valid ? 0x1000u : 0u;
        db.stencil_read_base = c.stencil_valid ? 0x2000u : 0u;
        regs->depth_control.depth_enable = c.depth_enable;
        regs->depth_control.stencil_enable = c.stencil_enable;

        const Vulkan::DepthAttachment a = Vulkan::BoundDepthAttachment(*regs);
        const bool ok = a.HasDepth() == c.expect_depth && a.HasStencil() == c.expect_stencil &&
                        a.Bound() == (c.expect_depth || c.expect_stencil) &&
                        (!a.HasDepth() || a.z_format == ZFormat::Z32Float) &&
                        (!a.HasStencil() || a.stencil_format == StencilFormat::Stencil8);
        if (!ok) {
            std::printf("FAIL: %s: depth %d stencil %d (expected %d %d)\n", c.name,
                        a.HasDepth(), a.HasStencil(), c.expect_depth, c.expect_stencil);
            ++failures;
        }
    }
    if (failures) {
        std::printf("FAIL: depth-attachment-test: %u of %zu cases\n", failures, std::size(kCases));
        return 1;
    }
    std::printf("PASS: depth-attachment-test (%zu cases)\n", std::size(kCases));
    return 0;
}
