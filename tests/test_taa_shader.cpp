// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// Run the production TAA shader on synthetic inputs, including undefined/invalid history.
// Metal-native (M5): the kernels are built by Taa::Build (Metal::BuildHostKernel) and recorded by
// Taa::Record / Taa::RecordSharpen, as in the renderer. argv[1] (optional): a SPIR-V file to run
// instead of the TAA kernel.
#include <cassert>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_texture.h"
#include "video_core/renderer_metal/vk_to_mtl.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_taa.h"

int main(int argc, char** argv) {
    // No game: the host kernels' MSL disk cache goes to a temporary directory.
    std::string dir_template =
        (std::filesystem::temp_directory_path() / "bb-taa-shader-test.XXXXXX").string();
    const char* cache_dir = mkdtemp(dir_template.data());
    assert(cache_dir);
    Metal::SetHostKernelCacheDir(cache_dir);
    Metal::Device device;
    Vulkan::Scheduler scheduler(device, false);
    // Odd dimensions and a pixel beyond 2000 exercise bounds and subpixel precision.
    constexpr u32 W = 2049, H = 7, N = W * H, X = 2010, center = 3 * W + X;
    std::array<Metal::Object, 7> images;
    for (u32 i = 0; i < images.size(); ++i) {
        const auto format = i != 4 && i != 6 ? vk::Format::eR32G32B32A32Sfloat
                                  : vk::Format::eR16G16B16A16Sfloat;
        images[i] = Metal::CreateTexture(device, {.pixel_format=Metal::VkToMtl::PixelFormat(format),
            .width=W, .height=H, .shader_write=true}, "taa-shader-test", nullptr);
        assert(images[i]);
    }
    std::vector<u32> override_code;
    if (argc > 1) {
        std::ifstream source(argv[1],std::ios::binary|std::ios::ate);
        const auto bytes=source.tellg();
        assert(source && bytes>0 && bytes%4==0);
        override_code.resize(size_t(bytes)/4);
        source.seekg(0); source.read(reinterpret_cast<char*>(override_code.data()),bytes);
        assert(source);
    }
    const Vulkan::Taa::Kernels kernels = Vulkan::Taa::Build(device, override_code);
    // The renderer's scene depth is D32S8, sampled where the kernel declares a sampler2D.
    const Metal::Object scene_depth_image=Metal::CreateTexture(device,{.pixel_format=
        Metal::VkToMtl::PixelFormat(vk::Format::eD32SfloatS8Uint),.width=W,.height=H},
        "taa-shader-test depth",nullptr);
    assert(scene_depth_image);
    // Four RGBA32F inputs, then D32 depth values for the D32S8 depth.
    VideoCore::Buffer staging(device,VideoCore::MemoryType::HostCached,0,N*16*4+N*4,"staging");
    assert(!staging.mapped_data.empty());
    void* const mapped = staging.mapped_data.data();
    const auto half = [](u16 v) {
        const int exponent=(v>>10)&31;
        const float mantissa=float(v&1023);
        return (v&0x8000 ? -1.f : 1.f)*
            (exponent==0 ? std::ldexp(mantissa,-24) : std::ldexp(1.f+mantissa/1024.f,exponent-15));
    };
    const auto run = [&](const char* label, u32 reset, float history, float history_depth,
                         float motion, float high, float expected, bool poison=false,
                         float scene_depth=0.5f, float z_translation=0.f,
                         float jitter_x=0.f, bool edge=false, float depth_slope=0.f,
                         bool thin_edge=false, bool history_edge=false,
                         bool thin_history=false, bool d32s8=false) {
        auto* pixels = static_cast<float*>(mapped);
        for (u32 image = 0; image < 4; ++image) {
            for (u32 p = 0; p < N; ++p) {
                float* c=pixels+image*N*4+p*4;
                c[0]=c[1]=c[2]=image==0 ? (p==center ? 0.25f : high) :
                    image==1 ? scene_depth : image==2 ? 0.f : history;
                c[3]=image==3 ? history_depth : 1.f;
                if (image==2) c[0]=motion;
                if (image==1) c[0]=std::clamp(scene_depth+(int(p%W)-int(X))*depth_slope,0.f,1.f);
                if (image==1 && thin_edge && p%W==X-1) c[0]=0.99f;
                if (image==3 && poison) c[0]=c[1]=c[2]=std::numeric_limits<float>::quiet_NaN();
                // A tap of another surface (behind this one) must not blend in.
                if (image==3 && edge && p%W==X+1) c[3]=0.75f;
                // A one-pixel line in front in the history only (missed by this jitter phase).
                if (image==3 && thin_history && p%W!=X) c[3]=scene_depth;
                if (image==3 && history_edge && p%W!=X) c[0]=c[1]=c[2]=0.25f;
            }
        }
        // The same depth values for the D32S8 variant (the red channel of image 1).
        for (u32 p = 0; p < N; ++p) pixels[N*16+p]=pixels[N*4+p*4];
        // A dark neighbor and a bright neighbor make history clipping measurable.
        pixels[(center-1)*4]=pixels[(center-1)*4+1]=pixels[(center-1)*4+2]=0.f;
        const Vulkan::Taa::Params params{{jitter_x,0},reset,0,
            {{{1,1,1,-0.05f},{1,1,1,-0.05f},{0,0,1,z_translation}}}};
        void* const depth_texture = d32s8 ? scene_depth_image.Get() : images[1].Get();
        scheduler.Record([&,buffer=staging.Handle(),depth_texture](Metal::Encoder& e) {
            for (u32 i=0;i<4;++i) {
                e.CopyBufferToTexture({buffer,u64(i)*N*16},W*16,N*16,images[i].Get(),0,0,{},
                    {W,H,1});
            }
            if (d32s8) {
                e.CopyBufferToTexture({buffer,u64(N)*16*4},W*4,N*4,depth_texture,0,0,{},{W,H,1},
                    Metal::BlitAspect::Depth);
            }
            Vulkan::Taa::Record(e,kernels,{images[0].Get(),depth_texture,images[2].Get(),
                images[3].Get(),images[4].Get(),images[5].Get(),images[6].Get()},params,W,H);
            for (u32 i=4;i<6;++i) {
                e.CopyTextureToBuffer(images[i].Get(),0,0,{X,3,0},{1,1,1},{buffer,(i-4)*16},
                    i==4 ? 8 : 16,i==4 ? 8 : 16);
            }
        });
        scheduler.Finish();
        const auto* result=static_cast<u16*>(mapped);
        if (std::abs(half(result[0])-expected)>=0.001f) {
            std::fprintf(stderr,"TAA regression: %s: actual %.6f, expected %.6f\n",
                         label,half(result[0]),expected);
            std::fflush(stdout);
        }
        assert(std::abs(half(result[0])-expected)<0.001f);
        const auto* next=reinterpret_cast<const float*>(static_cast<const char*>(mapped)+16);
        assert(half(result[3])==1.f &&
               std::abs(next[3]-(thin_edge ? 0.99f : scene_depth+jitter_x*depth_slope))<1e-7f);
        assert(std::abs(next[0]-expected)<0.001f);
        std::printf("TAA shader: %s PASS (%.3f)\n",label,half(result[0]));
    };
    run("reset ignores poisoned history",1,0.75f,0.5f,0,1,0.25f,true);
    run("reset unjitters current color",1,0.75f,0.5f,0,1,0.625f,true,0.5f,0,0.5f);
    run("temporal accumulation",0,0.75f,0.5f,0,1,0.74f);
    run("motion reduces persistence",0,0.75f,0.5f,4,1,0.7075f);
    run("neighborhood clipping",0,1,0.5f,0,0.3f,0.299f);
    // Foreground history over this surface without motion (an object that moved away, or a
    // thin one this jitter phase missed) is kept but clipped to the current neighbourhood.
    run("static disocclusion is clipped",0,0.75f,0.25f,0,0.3f,0.299f);
    run("moving disocclusion is rejected",0,0.75f,0.25f,2,1,0.25f);
    run("out-of-frame motion",0,0.75f,0.5f,50,1,0.25f);
    run("invalid history",0,0.75f,0.5f,0,1,0.25f,true);
    run("far depth retains full precision",0,0.75f,0.99998f,0,1,0.74f,false,0.99998f);
    run("far unrelated surface rejected",0,0.75f,0.9998f,2,1,0.25f,false,0.99998f);
    run("camera translation preserves history",0,0.75f,0.75f,0,1,0.74f,false,0.5f,0.1f);
    run("bilinear edge keeps matching tap",0,0.75f,0.5f,0.5f,1,
        0.25f+0.5f*(0.98f-0.13f*0.5f/8.f)*0.5f,false,0.5f,0,0,true);
    run("sky retains history",0,0.75f,1.f,0,1,0.74f,false,1.f);
    run("sky rejects geometry",0,0.75f,0.99998f,0,1,0.25f,false,1.f);
    run("sloping depth positive jitter",0,0.75f,0.99036f,0,1,0.746f,
        false,0.99f,0,0.4f,false,0.0009f);
    run("sloping depth negative jitter",0,0.75f,0.98964f,0,1,0.738f,
        false,0.99f,0,-0.4f,false,0.0009f);
    run("thin foreground retains history",0,0.75f,0.99f,0,1,0.74f,
        false,1.f,0,0,false,0,true);
    run("thin line missed by this frame keeps history",0,0.75f,0.25f,0,1,0.74f,
        false,0.5f,0,0,false,0,false,false,true);
    run("tiny motion at large pixel coordinate",0,0.75f,0.5f,-0.00003f,1,0.739985f,
        false,0.5f,0,0,false,0,false,true);
    // Metal-native additions: the renderer's D32S8 scene depth instead of a float colour image.
    run("temporal accumulation, D32S8 depth",0,0.75f,0.5f,0,1,0.74f,
        false,0.5f,0,0,false,0,false,false,false,true);
    run("sloping depth positive jitter, D32S8 depth",0,0.75f,0.99036f,0,1,0.746f,
        false,0.99f,0,0.4f,false,0.0009f,false,false,false,true);
    const auto sharpen=[&](const char* label,float strength,float scale,bool flat) {
        auto* input=static_cast<float*>(mapped);
        for (u32 p=0;p<N;++p) {
            for (u32 c=0;c<3;++c) input[p*4+c]=(p==center && !flat ? 0.55f : 0.5f)*scale;
            input[p*4+3]=0.99998f;
        }
        scheduler.Record([&,buffer=staging.Handle()](Metal::Encoder& e) {
            e.CopyBufferToTexture({buffer,0},W*16,N*16,images[5].Get(),0,0,{},{W,H,1});
            Vulkan::Taa::RecordSharpen(e,kernels.sharpen,images[5].Get(),images[4].Get(),
                strength,W,H);
            e.CopyTextureToBuffer(images[5].Get(),0,0,{},{W,H,1},{buffer,u64(N)*16},W*16,N*16);
            e.CopyTextureToBuffer(images[4].Get(),0,0,{},{W,H,1},{buffer,u64(N)*32},W*8,N*8);
        });
        scheduler.Finish();
        const auto* bytes=static_cast<const char*>(mapped);
        assert(!std::memcmp(bytes,bytes+N*16,N*16)); // Color and depth history remain exact.
        const auto* output=reinterpret_cast<const u16*>(bytes+N*32);
        for (u32 p=0;p<N;++p) {
            for (u32 c=0;c<3;++c) {
                const float value=half(output[p*4+c]);
                assert(std::isfinite(value) && value>=0 && value<=std::max(1.f,scale));
                if (flat || strength<=0) {
                    if (std::abs(value-input[p*4+c])>=0.003f)
                        std::fprintf(stderr,"RCAS %s pixel %u: %.6f expected %.6f\n",label,p,value,input[p*4+c]);
                    assert(std::abs(value-input[p*4+c])<0.003f);
                }
            }
            assert(half(output[p*4+3])==1.f);
        }
        const float value=half(output[center*4]);
        if (!flat && strength>0 && scale==1) assert(value>input[center*4]+0.002f);
        std::printf("TAA RCAS: %s PASS (%.4f), history unchanged\n",label,value);
        return value;
    };
    sharpen("zero bypass",0,1,false);
    sharpen("lower limit",-1,1,false);
    const float middle=sharpen("half strength",0.5f,1,false);
    const float maximum=sharpen("full strength",1,1,false);
    assert(maximum>middle);
    assert(sharpen("above one (extra pass for FSR)",2,1,false)>maximum);
    sharpen("flat gray",1,1,true);
    sharpen("flat black",1,0,true);
    sharpen("flat HDR",1,8,true);
    sharpen("HDR detail",1,8,false);
    // Metal-native addition: the LDR variant (ExtraSharpen of the UI image) writes an sRGB RGBA8
    // texture through its RGBA8Unorm view: the stored bytes are the sharpened values, unencoded.
    {
        const Metal::Object ldr=Metal::CreateTexture(device,{.pixel_format=
            Metal::VkToMtl::PixelFormat(vk::Format::eR8G8B8A8Srgb),.width=W,.height=H,
            .shader_write=true,.format_views=true},"taa-shader-test LDR",nullptr);
        assert(ldr);
        const Metal::Object unorm=Metal::CreateTextureView(ldr.Get(),
            Metal::VkToMtl::PixelFormat(vk::Format::eR8G8B8A8Unorm),Metal::TextureType::Tex2D,
            0,1,0,1,{2,3,4,5});
        assert(unorm);
        auto* input=static_cast<float*>(mapped);
        for (u32 p=0;p<N;++p) {
            for (u32 c=0;c<3;++c) input[p*4+c]=p==center ? 0.55f : 0.5f;
            input[p*4+3]=0.99998f;
        }
        scheduler.Record([&,buffer=staging.Handle()](Metal::Encoder& e) {
            e.CopyBufferToTexture({buffer,0},W*16,N*16,images[5].Get(),0,0,{},{W,H,1});
            Vulkan::Taa::RecordSharpen(e,kernels.sharpen_ldr,images[5].Get(),unorm.Get(),1,W,H);
            e.CopyTextureToBuffer(ldr.Get(),0,0,{},{W,H,1},{buffer,u64(N)*16},W*4,N*4);
        });
        scheduler.Finish();
        const auto* out=reinterpret_cast<const u8*>(static_cast<const char*>(mapped)+N*16);
        for (u32 p=0;p<N;++p) {
            if (p==center || p==center-1 || p==center+1 || p==center-W || p==center+W) continue;
            for (u32 c=0;c<3;++c) assert(std::abs(int(out[p*4+c])-128)<=1);
            assert(out[p*4+3]==255);
        }
        const float value=out[center*4]/255.f;
        assert(std::abs(value-maximum)<=1.f/255+0.001f);
        std::printf("TAA RCAS: LDR output through the Unorm view of an sRGB texture PASS (%.4f)\n",
                    value);
    }
    assert(device.ErrorCount()==0);
    std::filesystem::remove_all(cache_dir);
}
