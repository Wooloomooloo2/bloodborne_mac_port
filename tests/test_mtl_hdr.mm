// SPDX-License-Identifier: GPL-2.0-or-later
// GPU readbacks of the production HDR passes. No game data or real display is needed.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_hdr.h"
#include "video_core/renderer_metal/mtl_present.h"
#include "video_core/renderer_metal/mtl_texture.h"

using namespace Metal;
namespace {
int failures = 0;
void Check(bool ok, const char* message) {
    if (!ok) { std::printf("FAIL: %s\n", message); ++failures; }
}
float Linear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
Object Upload(Device& device, u32 format, u32 w, u32 h, const void* pixels, u32 stride) {
    id<MTLDevice> native = (__bridge id<MTLDevice>)device.NativeDevice();
    auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(format)
        width:w height:h mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [native newTextureWithDescriptor:desc];
    [texture replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:pixels bytesPerRow:stride];
    return Object::Adopt((__bridge_retained void*)texture);
}
std::vector<float> Read(Device& device, Encoder& encoder, void* texture, u32 w, u32 h) {
    id<MTLDevice> native = (__bridge id<MTLDevice>)device.NativeDevice();
    id<MTLBuffer> buffer = [native newBufferWithLength:w*h*8 options:MTLResourceStorageModeShared];
    encoder.CopyTextureToBuffer(texture, 0, 0, {}, {w,h,1}, {(__bridge void*)buffer,0}, w*8,w*h*8);
    device.Wait(encoder.Submit());
    std::vector<float> result(w*h*4);
    const auto* data = static_cast<const __fp16*>(buffer.contents);
    for (u32 i=0;i<w*h*4;++i) result[i] = data[i];
    return result;
}
void Run(Device& device) {
    HdrPasses hdr(device);
    constexpr u32 w=64,h=36;
    std::vector<u8> pixels(w*h*4,84); // sRGB ~0.2
    std::vector<__fp16> clean(w*h*4, __fp16(Linear(84.0f/255.0f)));
    std::vector<__fp16> scene=clean;
    for (u32 i=0;i<w*h;++i) { pixels[i*4+3]=255; clean[i*4+3]=scene[i*4+3]=1; }
    // Large lamp (not an edge), an opaque HUD element, and an invalid scene sample.
    for (u32 y=10;y<26;++y) for (u32 x=24;x<40;++x) {
        for (u32 c=0;c<3;++c) {
            pixels[(y*w+x)*4+c]=218;
            clean[(y*w+x)*4+c]=__fp16(Linear(218.0f/255.0f));
            scene[(y*w+x)*4+c]=4;
        }
    }
    for (u32 c=0;c<3;++c) { pixels[(18*w+8)*4+c]=255; scene[c]=__fp16(std::numeric_limits<float>::infinity()); }
    auto frame=Upload(device,MTLPixelFormatBGRA8Unorm,w,h,pixels.data(),w*4);
    auto before=Upload(device,MTLPixelFormatRGBA16Float,w,h,scene.data(),w*8);
    auto hudless=Upload(device,MTLPixelFormatRGBA16Float,w,h,clean.data(),w*8);
    auto output=CreateTexture(device,{.pixel_format=MTLPixelFormatRGBA16Float,.width=w,.height=h,
        .shader_write=true},"HDR test output",nullptr);
    auto convert=[&](float headroom,float highlights,bool captures) {
        Encoder e(device);
        hdr.Convert(e,frame.Get(),captures?before.Get():nullptr,captures?hudless.Get():nullptr,
                    output.Get(),headroom,highlights);
        return Read(device,e,output.Get(),w,h);
    };
    const auto bright=convert(3,0.5f,true);
    Check(bright[(18*w+32)*4] > 2.5f,"lamp recovers brightness above SDR white");
    Check(std::abs(bright[(18*w+8)*4]-1) < 0.005f,"game HUD stays at SDR white");
    Check(std::abs(bright[(18*w+50)*4]-Linear(84.0f/255.0f)) < 0.005f,"midtones retain SDR luminance");
    for (float value:bright) Check(std::isfinite(value)&&value>=0&&value<=3.001f,"finite bounded HDR output");
    for (const auto& result : {convert(1,0.5f,true),convert(3,0,false),convert(3,0,true),
                             convert(3,0.5f,false),convert(NAN,0.5f,true)}) {
        for (u32 i=0;i<w*h;++i) for (u32 c=0;c<3;++c) {
            Check(std::abs(result[i*4+c]-Linear(pixels[i*4+c]/255.0f)) < 0.005f,
                  "SDR fallback/no captures/zero strength matches sRGB decode");
        }
    }
    convert(3,0.5f,true);
    constexpr u32 tw=80,th=60;
    std::vector<u8> ui(tw*th*4,0);
    for (u32 y=0;y<4;++y) for (u32 x=0;x<4;++x) for (u32 c=0;c<4;++c) ui[(y*tw+x)*4+c]=255;
    for (u32 c=0;c<4;++c) ui[(10*tw+10)*4+c]=128;
    auto overlay=Upload(device,MTLPixelFormatBGRA8Unorm,tw,th,ui.data(),tw*4);
    auto target=CreateTexture(device,{.pixel_format=MTLPixelFormatRGBA16Float,.width=tw,.height=th,
        .render_target=true},"HDR test drawable",nullptr);
    Encoder e(device);
    e.EndEncoding();
    hdr.Present(e.CommandBuffer(),output.Get(),w,h,overlay.Get(),target.Get(),tw,th,2);
    auto presented=Read(device,e,target.Get(),tw,th);
    Check(std::abs(presented[(2*tw+2)*4]-1)<0.005f,"port overlay stays at SDR white");
    Check(presented[(2*tw+40)*4]==0,"letterbox remains black");
    Check(std::abs(presented[(10*tw+10)*4] -
          (128.0f/255.0f + Linear(84.0f/255.0f)*(127.0f/255.0f))) < 0.005f,
          "translucent host UI composites at SDR brightness in linear light");
    Check(presented[(30*tw+40)*4]>1.9f&&presented[(30*tw+40)*4]<2.005f,
          "present adapts to reduced display headroom");
    Encoder blank(device);
    blank.EndEncoding();
    hdr.Present(blank.CommandBuffer(),nullptr,0,0,overlay.Get(),target.Get(),tw,th,3);
    auto empty=Read(device,blank,target.Get(),tw,th);
    Check(empty[(30*tw+40)*4]==0,"blank frame never reuses a scene");
    Encoder snap(device);
    snap.EndEncoding();
    Object snapshot=hdr.Snapshot(device,snap.CommandBuffer(),target.Get(),tw,th);
    Check(bool(snapshot),"HDR drawable supports an SDR diagnostic snapshot");
    device.Wait(snap.Submit());
    Check(static_cast<const u8*>(((__bridge id<MTLBuffer>)snapshot.Get()).contents)[0] == 255,
          "snapshot conversion/readback survives temporary texture release");
    CAMetalLayer* layer=[CAMetalLayer layer];
    ConfigureLayer(device,(__bridge void*)layer,{.pixel_format=MTLPixelFormatRGBA16Float,.extended_range=true});
    Check(layer.wantsExtendedDynamicRangeContent&&layer.pixelFormat==MTLPixelFormatRGBA16Float,
          "EDR layer uses float output and extended-range flag");
    CFStringRef name = CGColorSpaceCopyName(layer.colorspace);
    Check(name && CFEqual(name, kCGColorSpaceExtendedLinearSRGB),"EDR layer uses extended linear sRGB");
    if (name) CFRelease(name);
    ConfigureLayer(device,(__bridge void*)layer,{});
    Check(!layer.wantsExtendedDynamicRangeContent&&layer.pixelFormat==MTLPixelFormatBGRA8Unorm,
          "SDR layer configuration restores the original presentation");
}
}
int main() { @autoreleasepool { Device device; Run(device); }
    std::printf("HDR GPU checks: %s\n",failures?"FAIL":"PASS"); return failures?1:0; }
