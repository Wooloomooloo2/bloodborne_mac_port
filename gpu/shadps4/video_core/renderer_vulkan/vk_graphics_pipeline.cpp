// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <utility>

#include "common/logging/log.h"
#include "video_core/renderer_metal/mtl_pipeline_build.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"

namespace Vulkan {

// bbport: the blend state of one color attachment, shared with the Metal pipelines
// (renderer_metal/mtl_pso_cache.mm) so both apply the same GCN emulation.
AttachmentBlend AttachmentBlendState(const GraphicsPipelineKey& key, u32 i) {
    const auto& control = key.blend_controls[i];

    const auto src_color = LiverpoolToVK::BlendFactor(control.color_src_factor);
    const auto dst_color = LiverpoolToVK::BlendFactor(control.color_dst_factor);
    const auto color_blend = LiverpoolToVK::BlendOp(control.color_func);

    const auto src_alpha = control.separate_alpha_blend
                               ? LiverpoolToVK::BlendFactor(control.alpha_src_factor)
                               : src_color;
    const auto dst_alpha = control.separate_alpha_blend
                               ? LiverpoolToVK::BlendFactor(control.alpha_dst_factor)
                               : dst_color;
    const auto alpha_blend =
        control.separate_alpha_blend ? LiverpoolToVK::BlendOp(control.alpha_func) : color_blend;

    // Vulkan ignores blend factors for min/max, but a factor that zeroes one operand
    // makes the operation collapse to a plain selection: min(s, 0) is 0 and max(s, 0)
    // is s for normalized alpha. Rewrite those to the equivalent add so the result is
    // exact instead of leaving the other operand to survive.
    auto eff_src_alpha = src_alpha;
    auto eff_dst_alpha = dst_alpha;
    auto eff_alpha_blend = alpha_blend;
    if (alpha_blend == vk::BlendOp::eMin || alpha_blend == vk::BlendOp::eMax) {
        const bool takes_max = alpha_blend == vk::BlendOp::eMax;
        if (src_alpha == vk::BlendFactor::eOne && dst_alpha == vk::BlendFactor::eZero) {
            eff_alpha_blend = vk::BlendOp::eAdd;
            eff_src_alpha = takes_max ? vk::BlendFactor::eOne : vk::BlendFactor::eZero;
            eff_dst_alpha = vk::BlendFactor::eZero;
        } else if (src_alpha == vk::BlendFactor::eZero && dst_alpha == vk::BlendFactor::eOne) {
            eff_alpha_blend = vk::BlendOp::eAdd;
            eff_src_alpha = vk::BlendFactor::eZero;
            eff_dst_alpha = takes_max ? vk::BlendFactor::eOne : vk::BlendFactor::eZero;
        }
    }

    AttachmentBlend blend{
        .enable = bool(control.enable),
        .src_color = src_color,
        .dst_color = dst_color,
        .src_alpha = eff_src_alpha,
        .dst_alpha = eff_dst_alpha,
        .color_op = color_blend,
        .alpha_op = eff_alpha_blend,
    };
    const auto color_scaled_min_max =
        (color_blend == vk::BlendOp::eMin || color_blend == vk::BlendOp::eMax) &&
        (src_color != vk::BlendFactor::eOne || dst_color != vk::BlendFactor::eOne) &&
        !key.color_buffers[i].blend_self_scale;
    const auto alpha_scaled_min_max =
        (eff_alpha_blend == vk::BlendOp::eMin || eff_alpha_blend == vk::BlendOp::eMax) &&
        (eff_src_alpha != vk::BlendFactor::eOne || eff_dst_alpha != vk::BlendFactor::eOne);
    blend.unimplemented_min_max = color_scaled_min_max || alpha_scaled_min_max;

    // The shader squares its color output for this attachment (see PsColorBuffer), so the
    // factors must not scale the operands again.
    if (key.color_buffers[i].blend_self_scale) {
        blend.self_scaled = true;
        blend.src_color = vk::BlendFactor::eOne;
        blend.dst_color = vk::BlendFactor::eOne;
    }

    // On GCN GPU there is an additional mask which allows to control color components exported
    // from a pixel shader. A situation possible, when the game may mask out the alpha channel,
    // while it is still need to be used in blending ops. For such cases, HW will default alpha
    // to 1 and perform the blending, while shader normally outputs 0 in the last component.
    // Unfortunatelly, Vulkan doesn't provide any control on blend inputs, so below we detecting
    // such cases and override alpha value in order to emulate HW behaviour.
    const auto has_alpha_masked_out =
        (key.cb_shader_mask.GetMask(i) & AmdGpu::ColorBufferMask::ComponentA) == 0;
    const auto has_src_alpha_in_src_blend = src_color == vk::BlendFactor::eSrcAlpha ||
                                            src_color == vk::BlendFactor::eOneMinusSrcAlpha;
    const auto has_src_alpha_in_dst_blend = dst_color == vk::BlendFactor::eSrcAlpha ||
                                            dst_color == vk::BlendFactor::eOneMinusSrcAlpha;
    if (has_alpha_masked_out && has_src_alpha_in_src_blend) {
        blend.src_color = src_color == vk::BlendFactor::eSrcAlpha ? vk::BlendFactor::eOne
                                                                   : vk::BlendFactor::eZero; // 1-A
    }
    if (has_alpha_masked_out && has_src_alpha_in_dst_blend) {
        blend.dst_color = dst_color == vk::BlendFactor::eSrcAlpha ? vk::BlendFactor::eOne
                                                                   : vk::BlendFactor::eZero; // 1-A
    }
    return blend;
}

GraphicsPipeline::GraphicsPipeline(
    Metal::PipelineFactory& factory, const Shader::Profile& profile,
    const GraphicsPipelineKey& key_, std::span<const Shader::Info*, MaxShaderStages> infos,
    std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
    std::optional<const Shader::Gcn::FetchShaderData> fetch_shader_,
    std::span<const Metal::ShaderModule* const, MaxShaderStages> modules_,
    SerializationSupport& sdata, bool preloading)
    : Pipeline{factory, profile}, key{key_}, fetch_shader{std::move(fetch_shader_)} {
    std::ranges::copy(infos, stages.begin());
    for (u32 i = 0; i < MaxShaderStages; ++i) {
        modules[i] = infos[i] ? modules_[i] : nullptr;
    }
    if (!preloading) {
        // The preload brings the attributes in `sdata`; here they come from the fetch shader.
        VertexInputs<AmdGpu::Buffer> guest_buffers;
        const auto& vs_info = runtime_infos[u32(Shader::SwStage::Vertex)].sw.vs;
        sdata.vertex_attributes.clear();
        GetVertexInputs(sdata.vertex_attributes, guest_buffers, vs_info.step_rate_0,
                        vs_info.step_rate_1);
    }
    attributes = sdata.vertex_attributes;

    // Without a fragment shader, clip-distance emulation would need the generated discard
    // shader (EmitDiscardFragmentShader): it has no Metal form, the build fails. (The Metal
    // profile never asks for the emulation: needs_clip_distance_emulation is off.)
    const bool clip_emulation_fragment =
        !infos[u32(Shader::SwStage::Fragment)] &&
        runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs.clip_distance_emulation;

    // bbport: the rest reads only the key, the modules and the attributes, so it runs on the
    // pipeline creation pool (vk_pipeline_common.h).
    BuildAsync([this, clip_emulation_fragment, debug_str = GetDebugString()] {
        Build(clip_emulation_fragment, debug_str);
    });
}

bool GraphicsPipeline::SupportsHdrUi() const {
    const auto* fs = modules[u32(Shader::SwStage::Fragment)];
    return key.mrt_mask == 1 && fs && fs->NativeHdrUi();
}

bool GraphicsPipeline::WritesFragmentColor() const {
    const auto* fs = modules[u32(Shader::SwStage::Fragment)];
    return fs && fs->OutputMask();
}

GraphicsPipeline::GraphicsPipeline(Metal::PipelineFactory& factory, const Shader::Profile& profile,
                                   const GraphicsPipeline& source)
    : Pipeline{factory, profile}, key{source.key}, fetch_shader{source.fetch_shader},
      modules{source.modules}, attributes{source.attributes} {
    stages = source.stages;
    constexpr u32 fs = u32(Shader::SwStage::Fragment);
    modules[fs] = source.modules[fs]->NativeHdrUi();
    ASSERT(modules[fs]); // PipelineCache admits the module before constructing this variant.
    for (u32 cb : {1u, 2u}) {
        key.color_buffers[cb] = {};
        key.color_buffers[cb].data_format = AmdGpu::DataFormat::Format16_16_16_16;
        key.color_buffers[cb].num_format = AmdGpu::NumberFormat::Float;
        key.color_buffers[cb].swizzle = AmdGpu::IdentityMapping;
        auto& blend = key.blend_controls[cb];
        blend = key.blend_controls[0];
        if (blend.enable) {
            blend.separate_alpha_blend = 1;
            blend.alpha_src_factor = AmdGpu::BlendControl::BlendFactor::One;
            blend.alpha_dst_factor = AmdGpu::BlendControl::BlendFactor::OneMinusSrcAlpha;
            blend.alpha_func = AmdGpu::BlendControl::BlendFunc::Add;
        }
        key.write_masks[cb] = key.write_masks[0] | vk::ColorComponentFlagBits::eA;
        key.color_samples[cb] = key.color_samples[0];
    }
    key.mrt_mask = 7;
    key.num_color_attachments = 3;
    key.cb_shader_mask.raw |= 0xff0u;
    BuildAsync([this, debug_str = GetDebugString()] { Build(false, debug_str); });
}

void GraphicsPipeline::Build(bool clip_emulation_fragment, const std::string& debug_str) {
    for (u32 i = 0; i < key.num_color_attachments; i++) {
        const AttachmentBlend blend = AttachmentBlendState(key, i);
        if (blend.unimplemented_min_max) {
            LOG_WARNING(
                Render_Vulkan,
                "Unimplemented use of min/max blend op with blend factor not equal to one.");
        }
        if (blend.self_scaled) {
            LOG_WARNING(
                Render_Vulkan,
                "Emulating scaled min/max blend with squared shader output on attachment {}", i);
        }
    }
    auto result = factory.BuildGraphics(key, modules, attributes, clip_emulation_fragment);
    psos = {.render = result.render,
            .tessellation = result.tessellation,
            .geometry = result.geometry,
            .rect_list = result.rect_list};
    if (!psos.render) {
        Fail("Graphics", debug_str, result.error);
    }
}

GraphicsPipeline::~GraphicsPipeline() {
    WaitReady(); // a build still on the creation pool uses this object
}

void GraphicsPipeline::GetVertexInputs(VertexInputs<VertexAttribute>& out_attributes,
                                       VertexInputs<AmdGpu::Buffer>& guest_buffers,
                                       u32 step_rate_0, u32 step_rate_1) const {
    using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
    if (!fetch_shader || fetch_shader->attributes.empty()) {
        return;
    }
    const auto& vs_info = GetStage(Shader::SwStage::Vertex);
    for (const auto& attrib : fetch_shader->attributes) {
        const auto step_rate = attrib.GetStepRate();
        out_attributes.push_back(VertexAttribute{
            .location = attrib.semantic,
            .per_instance = step_rate != InstanceIdType::None,
            .divisor = step_rate == InstanceIdType::OverStepRate0
                           ? step_rate_0
                           : (step_rate == InstanceIdType::OverStepRate1 ? step_rate_1 : 1),
        });
        guest_buffers.emplace_back(attrib.GetSharp(vs_info));
    }
}

} // namespace Vulkan
