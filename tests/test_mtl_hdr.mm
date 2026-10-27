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
    std::vector<u8> pixels(w*h*4,84);
    std::vector<__fp16> light(w*h*4, __fp16(0.18f)), bloom(w*h*4, __fp16(0));
    for (u32 i=0;i<w*h;++i) { pixels[i*4+3]=255; light[i*4+3]=0; }
    for (u32 y=10;y<26;++y) for (u32 x=24;x<40;++x)
        for (u32 c=0;c<3;++c) light[(y*w+x)*4+c]=64;
    // Both highlights quantise to the same SDR white. Native HDR must retain their difference.
    for (u32 c=0;c<3;++c) { light[(5*w+5)*4+c]=16; light[(5*w+10)*4+c]=32; }
    for (u32 c=0;c<3;++c) { bloom[(5*w+15)*4+c]=1.2f; bloom[(5*w+20)*4+c]=3; }
    light[0]=__fp16(std::numeric_limits<float>::infinity());
    for (u32 c=0;c<3;++c) light[4+c]=0;
    auto frame=Upload(device,MTLPixelFormatBGRA8Unorm,w,h,pixels.data(),w*4);
    auto before=Upload(device,MTLPixelFormatRGBA16Float,w,h,light.data(),w*8);
    auto bloom_image=Upload(device,MTLPixelFormatRGBA16Float,w,h,bloom.data(),w*8);
    std::vector<u8> game_ui(w*h*4,0);
    for (u32 c=0;c<4;++c) game_ui[(18*w+8)*4+c]=255;
    for (u32 c=0;c<4;++c) game_ui[(18*w+12)*4+c]=128;
    auto game_overlay=Upload(device,MTLPixelFormatBGRA8Unorm,w,h,game_ui.data(),w*4);
    auto output=CreateTexture(device,{.pixel_format=MTLPixelFormatRGBA16Float,.width=w,.height=h,
        .shader_write=true},"HDR test output",nullptr);
    auto convert=[&](float headroom,float white=1.0f,float peak=0.0f) {
        Encoder e(device);
        NativeHdrParams p;
        p.headroom=headroom; p.paper_white=white; p.peak=peak; p.has_ui=1;
        hdr.ConvertNative(e,before.Get(),bloom_image.Get(),nullptr,game_overlay.Get(),output.Get(),p);
        return Read(device,e,output.Get(),w,h);
    };
    const auto bright=convert(4);
    Check(bright[(18*w+32)*4]>3.9f,"native lamp reaches HDR peak");
    Check(bright[4]==0 && bright[5]==0 && bright[6]==0,"black stays black");
    Check(bright[(5*w+10)*4]-bright[(5*w+5)*4]>0.01f,
          "different scene highlights survive identical clipped SDR whites");
    Check(bright[(5*w+20)*4]>bright[(5*w+15)*4]+0.05f,
          "bloom above the old SDR clamp retains different intensities");
    Check(std::abs(bright[(18*w+8)*4]-1)<0.005f,"opaque game UI stays at SDR white");
    const float midtone=bright[(18*w+50)*4];
    Check(std::abs(bright[(18*w+12)*4]-(128.0f/255.0f+midtone*127.0f/255.0f))<0.005f,
          "translucent game UI uses actual coverage and linear composition");
    const auto sdr=convert(1);
    Check(std::abs(sdr[(18*w+50)*4]-midtone)<0.005f,"HDR shoulder preserves dark midtones");
    for (float value:bright) Check(std::isfinite(value)&&value>=0&&value<=4.001f,"finite bounded HDR output");
    const auto capped=convert(8,1,2);
    Check(capped[(18*w+32)*4]>1.99f&&capped[(18*w+32)*4]<=2.005f,"explicit peak cap is respected");
    const auto white=convert(4,1.5f);
    Check(white[(18*w+50)*4]>midtone*1.45f,"paper white changes scene reference brightness");
    Check(std::abs(white[(18*w+8)*4]-1)<0.005f,"paper white does not brighten UI");
    {
        id<MTLDevice> native=(__bridge id<MTLDevice>)device.NativeDevice();
        MTLTextureDescriptor* desc=[MTLTextureDescriptor new];
        desc.textureType=MTLTextureType3D; desc.pixelFormat=MTLPixelFormatRGBA32Float;
        desc.width=desc.height=desc.depth=2; desc.storageMode=MTLStorageModeShared;
        id<MTLTexture> lut=[native newTextureWithDescriptor:desc];
        float texels[32]{};
        for (u32 i=0;i<8;++i) { texels[i*4]=1; texels[i*4+3]=1; }
        [lut replaceRegion:MTLRegionMake3D(0,0,0,2,2,2) mipmapLevel:0 slice:0
                 withBytes:texels bytesPerRow:32 bytesPerImage:64];
        Encoder e(device);
        NativeHdrParams p; p.headroom=4; p.has_lut=1;
        hdr.ConvertNative(e,before.Get(),bloom_image.Get(),(__bridge void*)lut,nullptr,output.Get(),p);
        const auto graded=Read(device,e,output.Get(),w,h);
        Check(graded[(18*w+32)*4]>3.9f && graded[(18*w+32)*4+1]==0 && graded[(18*w+32)*4+2]==0,
              "game 3D grading preserves HDR excursion and RGB colour");
    }
    {
        std::vector<__fp16> black(w*h*4, __fp16(0)), white_layer(w*h*4, __fp16(1));
        for (u32 i=0;i<w*h;++i) { black[i*4+3]=0; white_layer[i*4+3]=0; }
        for (u32 c=0;c<3;++c) {
            // Additive UI must retain A=1, B=0.2 even though its coverage alpha is 0.5.
            black[(18*w+16)*4+c]=0.2f; white_layer[(18*w+16)*4+c]=1.2f;
            // A multiply layer cannot be represented by the coverage alpha alone.
            white_layer[(18*w+20)*4+c]=0.5f;
        }
        black[(18*w+16)*4+3]=black[(18*w+20)*4+3]=0.25f;
        // A red premultiplied UI pixel in the guest's BGR target layout.
        black[(18*w+8)*4+2]=0.5f; black[(18*w+8)*4+3]=0.5f;
        white_layer[(18*w+8)*4]=white_layer[(18*w+8)*4+1]=0.5f;
        auto b=Upload(device,MTLPixelFormatRGBA16Float,w,h,black.data(),w*8);
        auto wt=Upload(device,MTLPixelFormatRGBA16Float,w,h,white_layer.data(),w*8);
        NativeHdrParams p; p.headroom=4; p.has_ui=1; p.flags=1;
        Encoder e(device);
        hdr.ConvertNative(e,before.Get(),bloom_image.Get(),nullptr,b.Get(),output.Get(),p,wt.Get());
        auto result=Read(device,e,output.Get(),w,h);
        Check(result[(18*w+16)*4]>midtone*1.8f,"additive game UI retains its additive blend");
        Check(result[(18*w+20)*4]<midtone*0.35f,"multiply game UI retains its transmission");
        Check(result[(18*w+8)*4]>0.5f && result[(18*w+8)*4+2]<0.1f,
              "guest BGR UI is composited in the correct colour order");
    }
    {
        Encoder copy(device);
        hdr.CopyFrame(copy,frame.Get(),output.Get(),false);
        const auto fallback=Read(device,copy,output.Get(),w,h);
        Check(std::abs(fallback[4]-Linear(84.0f/255.0f))<0.005f,"missing native scene decodes SDR fallback");
    }
    convert(4);
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
          (128.0f/255.0f + midtone*(127.0f/255.0f))) < 0.005f,
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
