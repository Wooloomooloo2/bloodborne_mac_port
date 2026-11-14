// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the Metal pipeline states of the game's pipelines (Metal-native renderer, M3). The
// pipeline cache (renderer_vulkan/vk_pipeline_cache.h) keeps each shader permutation as SPIR-V
// (ShaderModule); GraphicsPipeline/ComputePipeline build their Metal pipeline states through a
// PipelineFactory on the pipeline creation pool. Plain C++ interface; the work is Objective-C++
// (mtl_pipeline_build.mm). The building logic began as the M1 shadow builder (mtl_shadow.mm).

#pragma once

#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_metal/mtl_pso_cache.h"
#include "video_core/renderer_metal/mtl_shader.h"

namespace Metal {

class Device;
namespace Geometry {
class PassThroughCache;
}

/// One guest shader permutation: its SPIR-V (what the recompiler emitted, or a shader patch) and
/// the Metal forms compiled from it on first use (PipelineFactory). Thread-safe; the compiled
/// forms live as long as the module.
class ShaderModule {
public:
    /// `hash` names the module in logs and in the MSL disk cache: the permutation hash
    /// (GraphicsPipelineKey::stage_hashes, ComputePipelineKey::value).
    ShaderModule(std::vector<u32> spirv, u64 hash);
    ~ShaderModule();

    ShaderModule(const ShaderModule&) = delete;
    ShaderModule& operator=(const ShaderModule&) = delete;

    [[nodiscard]] std::span<const u32> Spirv() const noexcept {
        return spirv;
    }
    [[nodiscard]] u64 Hash() const noexcept {
        return hash;
    }
    [[nodiscard]] u64 OutputMask() const noexcept { return output_mask; }
    /// Runtime-only fragment variant for one-pass HDR UI accumulation; null if unsupported.
    const ShaderModule* NativeHdrUi() const;

private:
    friend class PipelineFactory;

    std::vector<u32> spirv;
    u64 hash;
    u64 output_mask{};
    mutable std::once_flag hdr_ui_once;
    mutable std::unique_ptr<ShaderModule> hdr_ui;
    mutable std::once_flag plain_once;
    mutable std::unique_ptr<CompiledShader> plain;
    mutable std::once_flag rect_once; ///< the RectList rewrite of a vertex shader
    mutable std::unique_ptr<CompiledShader> rect;
    mutable std::mutex variant_mutex; ///< tessellation and geometry variants (CompileVariant)
    mutable std::vector<std::pair<CompileVariant, std::unique_ptr<CompiledShader>>> variants;
    /// Fragment shaders: the module with the inputs of a location mask zeroed (inputs the
    /// vertex stage does not write; mtl_spirv_inputs.h), under variant_mutex.
    mutable std::vector<std::pair<u64, std::unique_ptr<CompiledShader>>> zeroed;
};

/// What a graphics pipeline got: exactly one of the three kinds, or nothing with `error` set.
struct GraphicsPsos {
    const RenderPipeline* render{};             ///< every kind's render pipeline
    const TessellationPipeline* tessellation{}; ///< a tessellated draw's kernels too
    const GeometryPipeline* geometry{};         ///< a geometry-shader draw's kernels too
    bool rect_list{}; ///< `render`'s vertex function is the RectList rewrite (6 vertices per rect)
    std::string error; ///< why there is no pipeline (empty when `render` is set)
};

/// Builds the Metal pipeline states of the game's pipelines: owns the shader compiler, the PSO
/// cache (with its binary archive) and the geometry pass-through functions of one device.
/// Thread-safe: the pipeline creation pool builds on several threads.
class PipelineFactory {
public:
    /// `cache_dir`: the MSL disk cache and the binary archive (empty:
    /// ShaderCompiler::DefaultCacheDir()).
    explicit PipelineFactory(Device& device, std::filesystem::path cache_dir = {});
    ~PipelineFactory(); // the PSO cache writes the binary archive

    PipelineFactory(const PipelineFactory&) = delete;
    PipelineFactory& operator=(const PipelineFactory&) = delete;

    ShaderCompiler& Compiler() noexcept {
        return compiler;
    }
    PsoCache& Psos() noexcept {
        return psos;
    }

    /// The pipelines of a draw with `key`. `modules` by Shader::SwStage (null: the stage is not
    /// used). `attributes` in the fetch shader's order (the order of key.vertex_buffer_formats).
    /// `clip_emulation_fragment`: the pipeline has no fragment shader but needs the clip-distance
    /// emulation discard shader (no Metal form: fails, as QuadList does).
    GraphicsPsos BuildGraphics(const Vulkan::GraphicsPipelineKey& key,
                               std::span<const ShaderModule* const, Vulkan::MaxShaderStages> modules,
                               std::span<const VertexAttribute> attributes,
                               bool clip_emulation_fragment);

    /// The compute pipeline of `module`, or null with `error` set.
    const ComputePipeline* BuildCompute(const Vulkan::ComputePipelineKey& key,
                                        const ShaderModule& module, std::string& error);

private:
    const CompiledShader* Plain(const ShaderModule& module, const char* what);
    const CompiledShader* RectVertex(const ShaderModule& module, std::string& error);
    const CompiledShader* Variant(const ShaderModule& module, const CompileVariant& variant);
    /// The fragment shader for a vertex stage that writes the locations in `written`: the plain
    /// one when it reads no other input, else a compile with the others zeroed.
    const CompiledShader* FragmentFor(const ShaderModule& fs_module, const ShaderModule& vs_module,
                                      std::string& error);
    using StageModules = std::span<const ShaderModule* const, Vulkan::MaxShaderStages>;
    GraphicsPsos TessellationPsos(const Vulkan::GraphicsPipelineKey& key, StageModules modules,
                                  std::span<const VertexAttribute> attributes);
    GraphicsPsos GeometryPsos(const Vulkan::GraphicsPipelineKey& key, StageModules modules,
                              std::span<const VertexAttribute> attributes);
    ShaderCompiler compiler;
    PsoCache psos;
    std::unique_ptr<Geometry::PassThroughCache> pass_through;
};

} // namespace Metal
