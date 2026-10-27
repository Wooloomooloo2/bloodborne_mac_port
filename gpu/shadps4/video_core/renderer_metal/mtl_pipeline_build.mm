// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the Metal pipeline states of the game's pipelines (mtl_pipeline_build.h). Moved from
// the M1 shadow builder (mtl_shadow.mm Graphics/Compute), which followed Huax-1839's
// PipelineCache (TessellationShaders, GeometryShaders).

#import <Metal/Metal.h>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_metal/mtl_geometry.h"
#include "video_core/renderer_metal/mtl_pipeline_build.h"
#include "video_core/renderer_metal/mtl_spirv_geometry.h"
#include "video_core/renderer_metal/mtl_spirv_inputs.h"
#include "video_core/renderer_metal/mtl_spirv_rectlist.h"

namespace Metal {

namespace {
using Stage = ::Shader::SwStage;
using Kind = CompileVariant::Kind;
using Modules = std::span<const ShaderModule* const, Vulkan::MaxShaderStages>;

const ShaderModule* ModuleOf(Modules modules, Stage stage) {
    return modules[static_cast<u32>(stage)];
}
} // namespace

ShaderModule::ShaderModule(std::vector<u32> spirv_, u64 hash_)
    : spirv{std::move(spirv_)}, hash{hash_}, output_mask{OutputLocations(spirv).value_or(0)} {}

ShaderModule::~ShaderModule() = default;

const ShaderModule* ShaderModule::NativeHdrUi() const {
    std::call_once(hdr_ui_once, [&] {
        auto rewritten = DuplicateFragmentOutput(spirv);
        if (rewritten.ok)
            hdr_ui = std::make_unique<ShaderModule>(std::move(rewritten.spirv), hash ^ 0x48445255494d5254ull);
        else
            LOG_WARNING(Render, "HDR UI shader {:#x}: {}", hash, rewritten.error);
    });
    return hdr_ui.get();
}

PipelineFactory::PipelineFactory(Device& device, std::filesystem::path cache_dir)
    : compiler{device, std::move(cache_dir)}, psos{device, compiler.CacheDir()},
      pass_through{std::make_unique<Geometry::PassThroughCache>(device)} {}

PipelineFactory::~PipelineFactory() = default;

const CompiledShader* PipelineFactory::Plain(const ShaderModule& module, const char* what) {
    std::call_once(module.plain_once, [&] {
        auto result = compiler.Compile(module.spirv, module.hash, what);
        if (result.status == CompileStatus::Ok) {
            module.plain = std::move(result.shader);
        }
    });
    return module.plain.get();
}

const CompiledShader* PipelineFactory::RectVertex(const ShaderModule& module, std::string& error) {
    // RectList: the vertex shader rewritten to emit two triangles per rectangle (MoltenVK
    // emulated it with a tessellation control shader instead). Failures are remembered.
    std::call_once(module.rect_once, [&] {
        const auto rewrite = RewriteRectListVertex(module.spirv);
        if (!rewrite.ok) {
            LOG_WARNING(Render, "Rect list vertex shader {:#x} not rewritable: {}", module.hash,
                        rewrite.error);
            return;
        }
        auto result = compiler.Compile(rewrite.spirv, module.hash ^ 0x5265637443ull, "rect");
        if (result.status == CompileStatus::Ok) {
            module.rect = std::move(result.shader);
        }
    });
    if (!module.rect) {
        error = "the rect list vertex shader has no Metal form";
    }
    return module.rect.get();
}

const CompiledShader* PipelineFactory::Variant(const ShaderModule& module,
                                               const CompileVariant& variant) {
    std::scoped_lock lock{module.variant_mutex};
    for (const auto& [known, shader] : module.variants) {
        if (known == variant) {
            return shader.get();
        }
    }
    // A failed variant is remembered (null) like a compiled one.
    auto result = compiler.Compile(module.spirv, module.hash, "variant", variant);
    auto& made = module.variants.emplace_back(variant, std::move(result.shader));
    return made.second.get();
}

const CompiledShader* PipelineFactory::FragmentFor(const ShaderModule& fs_module,
                                                   const ShaderModule& vs_module,
                                                   std::string& error) {
    const CompiledShader* fs = Plain(fs_module, "fragment");
    if (!fs) {
        error = "the fragment shader did not compile";
        return nullptr;
    }
    const std::optional<u64> written = OutputLocations(vs_module.spirv);
    if (!written) {
        return fs; // outputs of unknown locations: assume they match
    }
    u64 unwritten = 0;
    for (const StageInput& input : fs->stage_inputs) {
        if (input.location < 64 && !(*written & (u64(1) << input.location))) {
            unwritten |= u64(1) << input.location;
        }
    }
    if (unwritten == 0) {
        return fs;
    }
    std::scoped_lock lock{fs_module.variant_mutex};
    for (const auto& [mask, shader] : fs_module.zeroed) {
        if (mask == unwritten) {
            if (!shader) {
                error = "the fragment shader with its unwritten inputs zeroed did not compile";
            }
            return shader.get();
        }
    }
    // A failed rewrite or compile is remembered (null) like a compiled one.
    auto& made = fs_module.zeroed.emplace_back(unwritten, nullptr);
    const auto rewrite = ZeroUnwrittenInputs(fs_module.spirv, unwritten);
    if (!rewrite.ok) {
        LOG_WARNING(Render, "Fragment shader {:#x}: inputs {:#x} not written by vertex shader "
                    "{:#x} and not rewritable: {}", fs_module.hash, unwritten, vs_module.hash,
                    rewrite.error);
    } else {
        auto result = compiler.Compile(rewrite.spirv, fs_module.hash ^ (unwritten * 0x9E3779B97F4A7C15ull),
                                       "fragment (unwritten inputs zeroed)");
        if (result.status == CompileStatus::Ok) {
            made.second = std::move(result.shader);
            LOG_INFO(Render, "Fragment shader {:#x}: inputs {:#x} not written by vertex shader "
                     "{:#x} read as zero", fs_module.hash, unwritten, vs_module.hash);
        }
    }
    if (!made.second) {
        error = "the fragment shader with its unwritten inputs zeroed did not compile";
    }
    return made.second.get();
}

GraphicsPsos PipelineFactory::BuildGraphics(const Vulkan::GraphicsPipelineKey& key,
                                            Modules modules,
                                            std::span<const VertexAttribute> attributes,
                                            bool clip_emulation_fragment) {
    GraphicsPsos result;
    @autoreleasepool { // pipeline-creation threads drain no pool of their own
        const bool tessellated = ModuleOf(modules, Stage::TessellationControl) ||
                                 ModuleOf(modules, Stage::TessellationEval);
        if (tessellated && ModuleOf(modules, Stage::Geometry)) {
            // LsHsEsGs: the evaluation shader is compiled as an export shader (ES ring, no
            // position), so it cannot be the post-tessellation vertex function; skip the draws.
            result.error = "tessellation with a geometry shader has no Metal path";
        } else if (tessellated) {
            result = TessellationPsos(key, modules, attributes);
        } else if (ModuleOf(modules, Stage::Geometry)) {
            result = GeometryPsos(key, modules, attributes);
        } else if (key.prim_type == AmdGpu::PrimitiveType::QuadList) {
            result.error = "quad lists have no Metal pipeline";
        } else if (clip_emulation_fragment) {
            result.error = "the clip-distance emulation fragment shader has no Metal form";
        } else if (const ShaderModule* vs_module = ModuleOf(modules, Stage::Vertex); !vs_module) {
            result.error = "no vertex shader";
        } else {
            const bool rect_list = key.prim_type == AmdGpu::PrimitiveType::RectList;
            const CompiledShader* vs =
                rect_list ? RectVertex(*vs_module, result.error) : Plain(*vs_module, "vertex");
            const CompiledShader* fs = nullptr;
            bool ok = vs != nullptr;
            if (!vs && result.error.empty()) {
                result.error = "the vertex shader did not compile";
            }
            if (ok) {
                if (const ShaderModule* fs_module = ModuleOf(modules, Stage::Fragment)) {
                    fs = FragmentFor(*fs_module, *vs_module, result.error);
                    ok = fs != nullptr;
                }
            }
            if (ok) {
                result.render = psos.GetRenderPipeline(
                    key, vs, fs,
                    {.vertex_attributes = attributes, .rect_list_triangles = rect_list});
                result.rect_list = rect_list;
                if (!result.render) {
                    result.error = "the Metal pipeline state failed";
                }
            }
        }
    }
    return result;
}

GraphicsPsos PipelineFactory::TessellationPsos(const Vulkan::GraphicsPipelineKey& key,
                                               Modules modules,
                                               std::span<const VertexAttribute> attributes) {
    // The vertex shader as a kernel per index type, the control shader as a kernel, the
    // evaluation shader as the post-tessellation vertex function.
    GraphicsPsos result;
    const ShaderModule* vs_module = ModuleOf(modules, Stage::Vertex);
    const ShaderModule* tcs_module = ModuleOf(modules, Stage::TessellationControl);
    const ShaderModule* tes_module = ModuleOf(modules, Stage::TessellationEval);
    if (!vs_module || !tcs_module || !tes_module) {
        result.error = "a tessellation stage is missing";
        return result;
    }
    const auto tes_info = ReadTessellationInfo(tes_module->Spirv());
    const auto tcs_info = ReadTessellationInfo(tcs_module->Spirv());
    if ((tes_info.domain != TessDomain::Triangles && tes_info.domain != TessDomain::Quads) ||
        tes_info.point_mode) {
        result.error = "the tessellation domain or point mode is not possible in Metal";
        return result;
    }
    if (tcs_info.output_vertices == 0) {
        result.error = "the control shader declares no output vertices";
        return result;
    }
    TessellationInput input;
    input.vertex_attributes = attributes;
    bool ok = true;
    for (u32 index_type = 0; ok && index_type < 3; ++index_type) {
        input.vertex[index_type] =
            Variant(*vs_module, {.kind = Kind::TessVertex, .index_type = index_type});
        ok = input.vertex[index_type] != nullptr;
    }
    if (ok) {
        input.control = Variant(*tcs_module, {.kind = Kind::TessControl, .domain = tes_info.domain});
        input.eval = Variant(*tes_module,
                             {.kind = Kind::TessEval,
                              .output_vertices = tcs_info.output_vertices,
                              .patch_first = tcs_info.patch_first,
                              .patch_end = tcs_info.patch_end});
        ok = input.control && input.eval;
    }
    if (!ok) {
        result.error = "a tessellation shader variant did not compile";
        return result;
    }
    const CompiledShader* fs = nullptr;
    if (const ShaderModule* fs_module = ModuleOf(modules, Stage::Fragment)) {
        fs = Plain(*fs_module, "fragment");
        if (!fs) {
            result.error = "the fragment shader did not compile";
            return result;
        }
    }
    result.tessellation = psos.GetTessellationPipeline(key, input, fs);
    if (!result.tessellation) {
        result.error = "the tessellation pipeline states failed";
        return result;
    }
    result.render = result.tessellation->render;
    return result;
}

GraphicsPsos PipelineFactory::GeometryPsos(const Vulkan::GraphicsPipelineKey& key,
                                           Modules modules,
                                           std::span<const VertexAttribute> attributes) {
    // The export shader as a kernel per index type, the geometry shader as a kernel, a generated
    // pass-through vertex function (Metal has no geometry stage: always emulated).
    GraphicsPsos result;
    const ShaderModule* es_module = ModuleOf(modules, Stage::Vertex);
    const ShaderModule* gs_module = ModuleOf(modules, Stage::Geometry);
    if (!es_module || !gs_module) {
        result.error = "a geometry stage is missing";
        return result;
    }
    const auto analysis = AnalyzeGeometryShader(gs_module->Spirv());
    if (!analysis.ok) {
        result.error = analysis.error;
        return result;
    }
    const bool matches = (analysis.info.input == GsInputPrimitive::Points &&
                          key.prim_type == AmdGpu::PrimitiveType::PointList) ||
                         (analysis.info.input == GsInputPrimitive::Lines &&
                          key.prim_type == AmdGpu::PrimitiveType::LineList) ||
                         (analysis.info.input == GsInputPrimitive::Triangles &&
                          key.prim_type == AmdGpu::PrimitiveType::TriangleList);
    if (!matches) {
        result.error = "the primitive type differs from the geometry shader's input";
        return result;
    }
    GeometryPipelineInput input;
    input.vertex_attributes = attributes;
    bool ok = true;
    for (u32 index_type = 0; ok && index_type < 3; ++index_type) {
        input.es[index_type] =
            Variant(*es_module, {.kind = Kind::TessVertex, .index_type = index_type});
        ok = input.es[index_type] != nullptr;
    }
    if (ok) {
        CompileVariant variant{.kind = Kind::GeometryKernel};
        variant.es.stride = input.es[0]->tess_layout.output;
        variant.es.members = input.es[0]->tess_layout.members;
        input.gs = Variant(*gs_module, variant);
        ok = input.gs != nullptr;
    }
    if (!ok) {
        result.error = "a geometry shader variant did not compile";
        return result;
    }
    const CompiledShader* fs = nullptr;
    if (const ShaderModule* fs_module = ModuleOf(modules, Stage::Fragment)) {
        fs = Plain(*fs_module, "fragment");
        if (!fs) {
            result.error = "the fragment shader did not compile";
            return result;
        }
    }
    std::span<const StageInput> fs_inputs;
    if (fs) {
        fs_inputs = fs->stage_inputs;
    }
    input.pass_through = pass_through->Get(input.gs->geometry, fs_inputs,
                                           key.clip_space == AmdGpu::ClipSpace::MinusWToW,
                                           result.error);
    if (!input.pass_through) {
        if (result.error.empty()) {
            result.error = "the pass-through vertex function failed";
        }
        return result;
    }
    result.geometry = psos.GetGeometryPipeline(key, input, fs);
    if (!result.geometry) {
        result.error = "the geometry pipeline states failed";
        return result;
    }
    result.render = result.geometry->render;
    return result;
}

const ComputePipeline* PipelineFactory::BuildCompute(const Vulkan::ComputePipelineKey& key,
                                                     const ShaderModule& module,
                                                     std::string& error) {
    const ComputePipeline* pipeline = nullptr;
    @autoreleasepool { // pipeline-creation threads drain no pool of their own
        const CompiledShader* cs = Plain(module, "compute");
        pipeline = cs ? psos.GetComputePipeline(key, cs) : nullptr;
        if (!cs) {
            error = "the compute shader did not compile";
        } else if (!pipeline) {
            error = "the Metal pipeline state failed";
        }
    }
    return pipeline;
}

} // namespace Metal
