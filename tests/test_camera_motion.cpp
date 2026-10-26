// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// Execute production camera motion SPIR-V: distant geometry, sky and stale object vectors.
// Metal-native (M5): the kernel is built by Metal::BuildHostKernel and recorded by
// CameraMotion::Record, as in the renderer. argv[1] (optional): a SPIR-V file to run instead.
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>
#include "video_core/buffer_cache/buffer.h"
#include "video_core/host_shaders/camera_motion_comp.h"
#include "video_core/host_shaders/camera_motion_debug_comp.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_host_kernel.h"
#include "video_core/renderer_metal/mtl_texture.h"
#include "video_core/renderer_metal/vk_to_mtl.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

int main(int argc, char** argv) {
    // No game: the host kernel's MSL disk cache goes to a temporary directory.
    std::string dir_template =
        (std::filesystem::temp_directory_path() / "bb-camera-motion-test.XXXXXX").string();
    const char* cache_dir = mkdtemp(dir_template.data());
    assert(cache_dir);
    Metal::SetHostKernelCacheDir(cache_dir);
    std::vector<u32> spirv(std::begin(CAMERA_MOTION_COMP), std::end(CAMERA_MOTION_COMP));
    if (argc > 1) {
        std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
        assert(file && file.tellg() % 4 == 0);
        spirv.resize(size_t(file.tellg()) / 4);
        file.seekg(0);
        file.read(reinterpret_cast<char*>(spirv.data()), std::streamsize(spirv.size() * 4));
    }
    Metal::Device device;
    Vulkan::Scheduler scheduler(device, false);
    const Metal::HostKernel kernel = Metal::BuildHostKernel(device, spirv, {}, "camera_motion");
    constexpr u32 W=9,H=7,N=W*H;
    std::array<Metal::Object,3> images;
    for (u32 i=0;i<3;++i) {
        const auto format=i==1 ? vk::Format::eR16G16Sfloat : vk::Format::eR32G32B32A32Sfloat;
        images[i]=Metal::CreateTexture(device,{.pixel_format=Metal::VkToMtl::PixelFormat(format),
            .width=W,.height=H,.shader_write=i==1},"camera-motion-test",nullptr);
        assert(images[i]);
    }
    // The renderer's scene depth is D32S8, bound where the kernel declares a texture2D.
    const Metal::Object scene_depth=Metal::CreateTexture(device,{.pixel_format=
        Metal::VkToMtl::PixelFormat(vk::Format::eD32SfloatS8Uint),.width=W,.height=H},
        "camera-motion-test depth",nullptr);
    assert(scene_depth);
    // Depth (RGBA32F), object vectors (RGBA32F), and D32 depth values for scene_depth.
    VideoCore::Buffer staging(device,VideoCore::MemoryType::HostCached,0,N*16*2+N*4,"staging");
    assert(!staging.mapped_data.empty());
    const auto run=[&](const char* label,float depth,float translation,float rotation,
                       float object_depth,u32 mode,float expected,bool jitter=false,
                       bool d32s8=false,float lift=0.f,float expected_y=0.f) {
        auto* pixels=reinterpret_cast<float*>(staging.mapped_data.data());
        for (u32 p=0;p<N;++p) {
            pixels[p*4]=depth;
            pixels[N*4+p*4]=2.f; pixels[N*4+p*4+1]=0.f;
            pixels[N*4+p*4+2]=1.f; pixels[N*4+p*4+3]=object_depth;
            pixels[N*8+p]=depth;
        }
        // `lift`: the previous view sits higher by this much (view +y), for the vertical case.
        const Vulkan::CameraMotion::Params params{{1,0,rotation,translation,0,1,0,lift,0,0,1,0},
            {1,1,1,-0.05f},{1,1,1,-0.05f},{W,H},
            {jitter ? 0.37f : 0.f,jitter ? -0.29f : 0.f},{0,0},mode};
        void* const depth_texture=d32s8 ? scene_depth.Get() : images[0].Get();
        scheduler.Record([&,buffer=staging.Handle(),depth_texture](Metal::Encoder& e) {
            for (u32 i : {0u,2u}) {
                e.CopyBufferToTexture({buffer,i/2*N*16},W*16,N*16,images[i].Get(),0,0,{},
                    {W,H,1});
            }
            if (d32s8) {
                e.CopyBufferToTexture({buffer,N*16*2},W*4,N*4,depth_texture,0,0,{},{W,H,1},
                    Metal::BlitAspect::Depth);
            }
            Vulkan::CameraMotion::Record(e,kernel,depth_texture,images[1].Get(),images[2].Get(),
                params);
            e.CopyTextureToBuffer(images[1].Get(),0,0,{4,3,0},{1,1,1},{buffer,0},4,4);
        });
        scheduler.Finish();
        const auto* result=reinterpret_cast<const u16*>(staging.mapped_data.data());
        const auto half=[](u16 v) {
            const int e=(v>>10)&31;
            return (v&0x8000 ? -1.f : 1.f)*(e==0 ? std::ldexp(float(v&1023),-24) :
                std::ldexp(1.f+float(v&1023)/1024.f,e-15));
        };
        assert(std::abs(half(result[0])-expected)<0.002f);
        assert(std::abs(half(result[1])-expected_y)<0.002f);
        std::printf("Camera motion: %s PASS (%.5f, %.5f)\n",label,half(result[0]),half(result[1]));
    };
    constexpr float distant=0.999995f;
    run("distant geometry moves",distant,1000,0,0,0,4.5f*1000*(1-distant)/0.05f);
    run("sky rotates",1,0,0.1f,0,0,0.45f);
    run("sky ignores translation",1,1000,0,0,0,0);
    run("static camera cancels jitter",distant,0,0,0,0,0,true);
    run("matching object vector",distant,0,0,distant,1,2);
    run("stale distant object vector",distant,0,0,0.9999f,1,0);
    // View +y is up the screen: a point seen higher in the previous frame was at a smaller row
    // (motion is previous minus current, in pixel rows). Measured on the game (2026-10-22): the
    // y-down convention moved the vectors opposite to the picture the upscalers read.
    run("vertical movement maps up the screen",distant,0,0,0,0,0,false,false,1000,
        -3.5f*1000*(1-distant)/0.05f);
    // Metal-native addition: the first case again with the renderer's D32S8 scene depth.
    run("distant geometry moves, D32S8 depth",distant,1000,0,0,0,4.5f*1000*(1-distant)/0.05f,
        false,true);
    // The debug overlay kernel (BB_DEBUG_MOTION=1) builds, with its four bindings.
    const Metal::HostKernel overlay=Metal::BuildHostKernel(device,CAMERA_MOTION_DEBUG_COMP,{},
        "camera_motion_debug");
    assert(overlay.pipeline && overlay.texture_slot[0]!=~0u && overlay.texture_slot[1]!=~0u &&
        overlay.buffer_slot[2]!=~0u && overlay.buffer_slot[3]!=~0u);
    std::printf("Camera motion: debug overlay kernel PASS\n");
    assert(device.ErrorCount()==0);
    std::filesystem::remove_all(cache_dir);
}
