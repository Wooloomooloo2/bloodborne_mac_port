// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the settings overlay's Metal side (bbport_overlay_metal.h). ARC.
#import <Metal/Metal.h>
#import <ImageIO/ImageIO.h>

#include "bbport_overlay_metal.h"
#include "bbport_achievements.h"
#include "imgui.h"
#include "imgui_impl_metal.h"
#include "video_core/renderer_metal/mtl_device.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace BbOverlay::MetalBackend {

namespace {
MTLPixelFormat expected_format = MTLPixelFormatInvalid;
id<MTLTexture> trophy_images[BB_TROPHY_COUNT];

uint64_t BigEndian(const unsigned char* bytes, int count) {
    uint64_t value = 0;
    while (count--) value = (value << 8) | *bytes++;
    return value;
}

// Adapted from game_achievements.mm, evill33t/bloodborne_mac (docs/ACHIEVEMENTS.md).
// Only bounded, unencrypted PNG entries. Missing/corrupt art always keeps the drawn cup.
void LoadTrophyImages(id<MTLDevice> device) {
    std::ifstream file(BbAchievements::ArtworkPath(), std::ios::binary | std::ios::ate);
    if (!file) return;
    const auto length = file.tellg();
    if (length < 96 || length > 64*1024*1024) return;
    std::vector<unsigned char> bytes(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), length)) return;
    const auto entries = BigEndian(bytes.data()+16, 4), stride = BigEndian(bytes.data()+20, 4);
    if (BigEndian(bytes.data(), 4) != 0xdca24d00 || BigEndian(bytes.data()+8, 8) != bytes.size() ||
        stride < 64 || stride > 4096 || entries > (bytes.size()-96)/stride) return;
    for (uint64_t entry = 0; entry < entries; ++entry) {
        const auto* row = bytes.data()+96+entry*stride;
        const auto offset = BigEndian(row+32, 8), size = BigEndian(row+40, 8);
        if (BigEndian(row+48,4) || size < 8 || size > 2*1024*1024 || offset > bytes.size() ||
            size > bytes.size()-offset || std::memcmp(bytes.data()+offset, "\211PNG\r\n\032\n", 8)) continue;
        for (int id = 0; id < BB_TROPHY_COUNT; ++id) {
            char name[32];
            std::snprintf(name, sizeof name, "TROP%03d.PNG", id);
            if (strnlen(reinterpret_cast<const char*>(row), 32) != std::strlen(name) ||
                std::memcmp(row, name, std::strlen(name))) continue;
            NSData* data = [NSData dataWithBytes:bytes.data()+offset length:size];
            CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)data, nullptr);
            if (!source) continue;
            NSDictionary* properties = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr));
            const NSUInteger width = [properties[(__bridge NSString*)kCGImagePropertyPixelWidth] unsignedIntegerValue];
            const NSUInteger height = [properties[(__bridge NSString*)kCGImagePropertyPixelHeight] unsignedIntegerValue];
            CGImageRef image = width && height && width <= 512 && height <= 512
                ? CGImageSourceCreateImageAtIndex(source, 0, nullptr) : nullptr;
            CFRelease(source);
            if (!image) continue;
            std::vector<unsigned char> rgba(width*height*4);
            CGColorSpaceRef colors = CGColorSpaceCreateDeviceRGB();
            CGContextRef context = CGBitmapContextCreate(rgba.data(), width, height, 8, width*4,
                colors, CGBitmapInfo(kCGImageAlphaPremultipliedLast) | CGBitmapInfo(kCGBitmapByteOrder32Big));
            CGColorSpaceRelease(colors);
            if (context) {
                CGContextDrawImage(context, CGRectMake(0,0,width,height), image);
                CGContextRelease(context);
                MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                    width:width height:height mipmapped:NO];
                desc.usage = MTLTextureUsageShaderRead;
                desc.storageMode = MTLStorageModeShared;
                trophy_images[id] = [device newTextureWithDescriptor:desc];
                [trophy_images[id] replaceRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0
                    withBytes:rgba.data() bytesPerRow:width*4];
            }
            CGImageRelease(image);
        }
    }
}
}

bool Init(Metal::Device& device, u32 pixel_format) {
    expected_format = MTLPixelFormat(pixel_format);
    LoadTrophyImages((__bridge id<MTLDevice>)device.NativeDevice());
    return ImGui_ImplMetal_Init((__bridge id<MTLDevice>)device.NativeDevice());
}

bool TrophyImage(int id, float size) {
    if (id < 0 || id >= BB_TROPHY_COUNT || !trophy_images[id]) return false;
    ImGui::Image(reinterpret_cast<ImTextureID>((__bridge void*)trophy_images[id]), ImVec2(size,size));
    return true;
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
