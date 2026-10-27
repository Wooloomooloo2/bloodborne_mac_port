// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string>

#include <boost/container/static_vector.hpp>
#include <xxhash.h>

#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_depth.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/renderer_metal/mtl_pso_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_pipeline_key.h"

namespace Metal {
class ShaderModule;
} // namespace Metal

namespace Vulkan {

template <typename T>
using VertexInputs = boost::container::static_vector<T, MaxVertexBufferCount>;

/// One vertex attribute of a pipeline (location, step rate), in the fetch shader's order.
using VertexAttribute = Metal::VertexAttribute;

class GraphicsPipeline : public Pipeline {
public:
    struct SerializationSupport {
        /// GetVertexInputs' attributes (the preload has no guest registers to derive them).
        VertexInputs<VertexAttribute> vertex_attributes{};

        void Serialize(Serialization::Archive& ar) const;
        bool Deserialize(Serialization::Archive& ar);
    };

    /// `modules` by Shader::SwStage (null: not used); they outlive the pipeline (the pipeline
    /// cache's programs own them).
    GraphicsPipeline(Metal::PipelineFactory& factory, const Shader::Profile& profile,
                     const GraphicsPipelineKey& key,
                     std::span<const Shader::Info*, MaxShaderStages> stages,
                     std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
                     std::optional<const Shader::Gcn::FetchShaderData> fetch_shader,
                     std::span<const Metal::ShaderModule* const, MaxShaderStages> modules,
                     SerializationSupport& sdata, bool preloading);
    ~GraphicsPipeline();
    /// Runtime-only one-pass UI variant: original SDR plus two float blend accumulators.
    GraphicsPipeline(Metal::PipelineFactory& factory, const Shader::Profile& profile,
                     const GraphicsPipeline& source);
    bool SupportsHdrUi() const;
    bool WritesFragmentColor() const;

    const std::optional<const Shader::Gcn::FetchShaderData>& GetFetchShader() const noexcept {
        return fetch_shader;
    }

    const GraphicsPipelineKey& GetKey() const noexcept {
        return key;
    }
    const GraphicsPipelineKey& GetGraphicsKey() const noexcept {
        return key;
    }

    // The Metal pipeline states, once ready (IsReady/WaitReady); null when Failed().
    /// The render pipeline of every kind of draw (for tessellation and geometry draws the
    /// evaluation / pass-through stage with the fragment shader).
    const Metal::RenderPipeline* Render() const noexcept {
        return psos.render;
    }
    /// A tessellated draw's kernels (null otherwise).
    const Metal::TessellationPipeline* Tessellation() const noexcept {
        return psos.tessellation;
    }
    /// A geometry-shader draw's kernels (null otherwise).
    const Metal::GeometryPipeline* Geometry() const noexcept {
        return psos.geometry;
    }
    /// The vertex function is the RectList rewrite: draws count 6 vertices per rectangle.
    bool IsRectList() const noexcept {
        return psos.rect_list;
    }
    /// The vertex attributes the pipeline was built with (fetch shader order).
    std::span<const VertexAttribute> Attributes() const noexcept {
        return attributes;
    }

    /// The vertex attributes of the fetch shader for the draw's step rates, and the V#s of their
    /// buffers (same order).
    void GetVertexInputs(VertexInputs<VertexAttribute>& out_attributes,
                         VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0,
                         u32 step_rate_1) const;

private:
    struct Psos {
        const Metal::RenderPipeline* render{};
        const Metal::TessellationPipeline* tessellation{};
        const Metal::GeometryPipeline* geometry{};
        bool rect_list{};
    };
    void Build(bool clip_emulation_fragment, const std::string& debug_str);

    GraphicsPipelineKey key;
    std::optional<const Shader::Gcn::FetchShaderData> fetch_shader{};
    std::array<const Metal::ShaderModule*, MaxShaderStages> modules{};
    VertexInputs<VertexAttribute> attributes;
    Psos psos; ///< written by the build, read once ready
};

struct ClipDistanceShaderKey {
    std::array<std::tuple<u8, u8>, 8> clip_locations;

    bool operator==(const ClipDistanceShaderKey& key) const noexcept {
        return std::memcmp(this, &key, sizeof(key)) == 0;
    }
};

} // namespace Vulkan

template <>
struct std::hash<Vulkan::ClipDistanceShaderKey> {
    std::size_t operator()(const Vulkan::ClipDistanceShaderKey& key) const noexcept {
        return XXH3_64bits(&key, sizeof(key));
    }
};
