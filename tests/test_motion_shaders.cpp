// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the actual SPIR-V backend without opening a window or loading the game.
// The SPIR-V is written to argv[1] (default: a temporary directory) for spirv-val (see
// docs/upscaler.md). bbport, Metal-native M5: every variant also compiles to MSL through the
// renderer's ShaderCompiler (the motion vertex shader reaches its buffers by GPU address), and
// ObjectMotion runs on a Metal device: history buffers resident at fixed addresses, the motion
// target bound as colour attachment 7, cleared once per frame, recreated at a new size.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>
#include <setjmp.h>
#include <unistd.h>
#include "bbport_settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_shader.h"
#include "video_core/renderer_metal/mtl_texture.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace {

int failures = 0;

void Expect(bool ok, const std::string& what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what.c_str());
    failures += ok ? 0 : 1;
}

struct Variant {
    std::string name;
    bool addresses{}; ///< the motion vertex shader stores and loads positions by GPU address
    std::vector<u32> spirv;
};

std::vector<u32> Emit(bool vertex, bool motion) {
    using namespace Shader;
    Info info{};
    info.hw_stage = vertex ? HwStage::Vertex : HwStage::Fragment;
    info.sw_stage = vertex ? SwStage::Vertex : SwStage::Fragment;
    RuntimeInfo runtime{};
    runtime.Initialize(info.hw_stage, info.sw_stage);
    if (vertex) {
        runtime.hw.vs.motion_vectors = motion;
    } else {
        runtime.hw.fs.motion_vectors = motion;
        runtime.hw.fs.color_buffers[0].num_format = AmdGpu::NumberFormat::Float;
    }
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    const auto output = vertex ? IR::Attribute::Position0 : IR::Attribute::RenderTarget0;
    for (u32 component = 0; component < 4; ++component) {
        info.stores.Set(output, component);
        if (!vertex && component < 2) {
            info.loads.Set(IR::Attribute::FragCoord, component);
            ir.SetAttribute(output, ir.GetAttribute(IR::Attribute::FragCoord, component), component);
        } else {
            ir.SetAttribute(output, ir.Imm32(component == 3 ? 1.0f : 0.0f), component);
        }
    }
    ir.Epilogue();
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.syntax_list.push_back({.data = {.block = &block},
                                   .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = true;
    Backend::Bindings bindings{};
    return Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
}

std::filesystem::path TempDir(const char* tag) {
    std::string pattern =
        (std::filesystem::temp_directory_path() / (std::string(tag) + "-XXXXXX")).string();
    if (!mkdtemp(pattern.data())) {
        std::perror("mkdtemp");
        std::exit(1);
    }
    return pattern;
}

/// SPIR-V -> MSL -> MTLFunction for each variant; the motion vertex shader uses device addresses
/// (the fragment shader only writes attachment 7 from its varyings).
void TestMslCompile(Metal::Device& device, const std::vector<Variant>& variants) {
    const auto cache = TempDir("motion-msl");
    {
        Metal::ShaderCompiler compiler(device, cache);
        u64 hash = 0x6d6f74696f6e0000ull;
        for (const auto& v : variants) {
            const auto result = compiler.Compile(v.spirv, ++hash, v.name);
            const bool ok = result.status == Metal::CompileStatus::Ok && result.shader &&
                            result.shader->function;
            Expect(ok, v.name + " compiles to MSL" + (ok ? "" : " (" + result.error + ")"));
            if (ok) {
                Expect(result.shader->uses_device_address == v.addresses,
                       v.name + (v.addresses ? " reaches the history by GPU address"
                                             : " uses no GPU addresses"));
            }
        }
    }
    std::filesystem::remove_all(cache);
}

/// A pass that clears a small texture: opens a render encoder, which declares every buffer
/// declared on the scheduler's encoder.
void ClearPass(Vulkan::Scheduler& scheduler, void* texture) {
    Vulkan::RenderState state{};
    state.colors[0] = {.texture = texture, .clear = true};
    state.num_colors = 1;
    state.width = state.height = 4;
    scheduler.BeginRendering(state);
    scheduler.EndRendering();
}

void TestObjectMotion(Metal::Device& device) {
    using namespace Vulkan;
    auto& settings = BbSettings::Get();
    settings.object_motion = true;
    settings.upscaler = BbSettings::UpscalerTaa;
    Scheduler scheduler(device, false);
    Metal::Object small = Metal::CreateTexture(
        device,
        Metal::TextureDesc{.pixel_format = 70 /*MTLPixelFormatRGBA8Unorm*/, .width = 4,
                           .height = 4, .render_target = true},
        "small", nullptr);
    Shader::MotionVectors::params_address = 0;
    Shader::MotionVectors::positions_address = 0;
    {
        ObjectMotion motion(device, scheduler);
        Expect(motion.Enabled(), "object motion is on with object_motion=1 and an upscaler");
        Expect(Shader::MotionVectors::params_address != 0 &&
                   Shader::MotionVectors::positions_address != 0 &&
                   Shader::MotionVectors::params_address !=
                       Shader::MotionVectors::positions_address,
               "the parameter and position buffers have GPU addresses");

        motion.OnFrameStart();
        const Motion::Draw draw{.shader = 1, .geometry = 2, .index_count = 3, .instances = 1,
                                .vertices = {0, 3}};
        const u32 first = motion.PrepareDraw(draw);
        Expect(first != 0, "a draw gets a parameter slot (stores this frame's positions)");

        constexpr u32 mv = Shader::MotionVectors::Output;
        RenderState state{};
        state.num_colors = 5;
        motion.Attach(state, 64, 32);
        void* texture = state.colors[mv].texture;
        Expect(texture != nullptr, "Attach binds the motion target as colour attachment 7");
        Expect(state.num_colors == mv + 1, "Attach extends the pass to eight colours");
        Expect(state.colors[mv].clear, "the first pass of a frame clears the motion target");
        Expect(texture && Metal::TexturePixelFormat(texture) == 125 /*MTLPixelFormatRGBA32Float*/,
               "the motion target is RGBA32Float");

        RenderState again{};
        again.num_colors = 8;
        motion.Attach(again, 64, 32);
        Expect(again.colors[mv].texture == texture && !again.colors[mv].clear,
               "later passes of the frame load the motion target");

        bool valid = false;
        Expect(motion.PrepareRead(64, 32, valid) == texture && valid,
               "the compose pass reads this frame's vectors");
        motion.PrepareRead(128, 64, valid);
        Expect(!valid, "vectors of another size are not this frame's");
        Expect(motion.Image(64, 32) == texture && motion.Image(32, 32) == nullptr,
               "Image() answers only for the size written");

        motion.OnFrameStart();
        Expect(motion.PrepareRead(64, 32, valid) == texture && !valid,
               "a new frame has no vectors until a motion pass ran");
        const u32 second = motion.PrepareDraw(draw);
        Expect(second != 0 && second != first,
               "the next frame's draw uses another parameter slot (frames in flight)");
        RenderState next{};
        motion.Attach(next, 64, 32);
        Expect(next.colors[mv].texture == texture && next.colors[mv].clear,
               "the next frame clears the motion target again");

        RenderState resized{};
        motion.Attach(resized, 96, 48);
        Expect(resized.colors[mv].texture != nullptr && resized.colors[mv].clear,
               "a new render size gets a cleared target");

        // The pass with the motion attachment and the declarations run on the GPU.
        resized.width = 96;
        resized.height = 48;
        scheduler.BeginRendering(resized);
        scheduler.EndRendering();
        ClearPass(scheduler, small.Get());
        scheduler.Finish();
        Expect(device.ErrorCount() == 0, "command buffers completed without errors");
    }
    // The history buffers are released and no longer declared on new encoders.
    ClearPass(scheduler, small.Get());
    scheduler.Finish();
    Expect(device.ErrorCount() == 0, "command buffers after ObjectMotion is gone run");

    settings.object_motion = false;
    settings.upscaler = BbSettings::UpscalerOff;
    ObjectMotion off(device, scheduler);
    Expect(!off.Enabled() && off.PrepareDraw({}) == 0, "object motion is off without an upscaler");
}

} // namespace

int main(int argc, char** argv) {
    using namespace Shader;
    const std::filesystem::path dir =
        argc > 1 ? std::filesystem::path(argv[1]) : TempDir("motion-spv");
    std::filesystem::create_directories(dir);
    MotionVectors::params_address = 0x10000;
    MotionVectors::positions_address = 0x20000;
    std::vector<Variant> variants;
    for (bool vertex : {true, false}) {
        for (bool motion : {false, true}) {
            Variant v{std::string(vertex ? "vertex" : "fragment") + (motion ? "-motion" : "-plain"),
                      vertex && motion, Emit(vertex, motion)};
            std::ofstream out(dir / (v.name + ".spv"), std::ios::binary);
            out.write(reinterpret_cast<const char*>(v.spirv.data()), v.spirv.size() * sizeof(u32));
            if (!out) {
                return 1;
            }
            variants.push_back(std::move(v));
        }
    }
    Metal::Device device;
    TestMslCompile(device, variants);
    TestObjectMotion(device);
    if (argc <= 1) {
        std::filesystem::remove_all(dir);
    }
    std::printf(failures ? "motion-shader-test: %d FAILED\n" : "motion-shader-test: all PASS\n",
                failures);
    return failures ? 1 : 0;
}
