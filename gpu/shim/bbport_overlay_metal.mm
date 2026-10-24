// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the settings overlay's Metal side (bbport_overlay_metal.h). ARC.
#import <Metal/Metal.h>

#include "bbport_overlay_metal.h"
#include "imgui.h"
#include "imgui_impl_metal.h"
#include "video_core/renderer_metal/mtl_device.h"

#include <cstdio>

namespace BbOverlay::MetalBackend {

namespace {
MTLPixelFormat expected_format = MTLPixelFormatInvalid;
}

bool Init(Metal::Device& device, u32 pixel_format) {
    expected_format = MTLPixelFormat(pixel_format);
    return ImGui_ImplMetal_Init((__bridge id<MTLDevice>)device.NativeDevice());
}

void Frame(void* command_buffer, void* target, const std::function<void()>& build) {
    id<MTLCommandBuffer> cmdbuf = (__bridge id<MTLCommandBuffer>)command_buffer;
    id<MTLTexture> texture = (__bridge id<MTLTexture>)target;
    if (texture.pixelFormat != expected_format) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::printf("Overlay: target format %u, not %u (the backend builds a pipeline per format)\n",
                        unsigned(texture.pixelFormat), unsigned(expected_format));
        }
    }
    // The backend keys its pipeline on the pass's formats from NewFrame, so the descriptor is
    // made before the frame and the encoder from it after.
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    ImGui_ImplMetal_NewFrame(pass);
    build();
    ImDrawData* draw_data = ImGui::GetDrawData();
    if (!draw_data || draw_data->CmdLists.Size == 0) {
        return; // nothing to draw: no pass (the target keeps its contents)
    }
    id<MTLRenderCommandEncoder> encoder = [cmdbuf renderCommandEncoderWithDescriptor:pass];
    encoder.label = @"Overlay";
    ImGui_ImplMetal_RenderDrawData(draw_data, cmdbuf, encoder);
    [encoder endEncoding];
}

} // namespace BbOverlay::MetalBackend
