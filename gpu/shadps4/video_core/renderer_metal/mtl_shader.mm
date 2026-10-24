// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: from Huax-1839/bloodborne-macos (ee3120a, GPL-2.0-or-later), renderer_metal/.

// SPIR-V (the recompiler's output) -> MSL (SPIRV-Cross) -> MTLLibrary/MTLFunction. Resource
// bindings use the direct-slot layout of docs/METAL_BACKEND_PLAN.md D5 (see mtl_shader.h); the MSL
// text and the binding map are cached on disk (format: "Disk cache" below).

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <xxhash.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <span>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

#include <spirv_cross/spirv_msl.hpp>

#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "video_core/renderer_metal/mtl_device_impl.h"
#include "video_core/renderer_metal/mtl_shader.h"
#include "video_core/renderer_metal/mtl_spirv_geometry.h"
#include "video_core/renderer_metal/mtl_spirv_pervertex.h"

#ifndef BB_SPIRV_CROSS_VERSION
#define BB_SPIRV_CROSS_VERSION "unknown"
#endif

namespace Metal {

namespace {

namespace sc = spirv_cross;
using Kind = ShaderBinding::Kind;

constexpr const char* kSpirvCrossVersion = BB_SPIRV_CROSS_VERSION;
constexpr const char* kCacheMagic = "// bbport-msl";
constexpr const char* kCacheMslMarker = "// ---- MSL";

const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::Buffer:
        return "buffer";
    case Kind::Texture:
        return "texture";
    case Kind::StorageTexture:
        return "storage_texture";
    case Kind::Sampler:
        return "sampler";
    case Kind::TexelBuffer:
        return "texel_buffer";
    case Kind::PushConstants:
        return "push_constants";
    }
    return "?";
}

std::string FirstLine(const std::string& text) {
    const auto end = text.find('\n');
    return end == std::string::npos ? text : text.substr(0, end);
}

/// What the module declares, read from the SPIR-V words without SPIRV-Cross (cheap enough to run
/// on every call, also on a disk cache hit and for modules SPIRV-Cross would reject).
struct SpirvScan {
    bool valid{};
    ShaderStage stage{ShaderStage::Unknown};
    bool per_vertex{};      ///< a PerVertexKHR decoration (pull-model fragment inputs)
    bool device_address{};  ///< Capability PhysicalStorageBufferAddresses
    bool bda_pagetable{};   ///< a variable named "bda_pagetable"
    std::array<u32, 3> local_size{0, 0, 0}; ///< OpExecutionMode LocalSize
    TessellationInfo tess;                  ///< the tessellation execution modes
};

SpirvScan ScanSpirv(std::span<const u32> code) {
    SpirvScan scan;
    if (code.size() < 5 || code[0] != spv::MagicNumber) {
        return scan;
    }
    scan.valid = true;
    std::vector<u32> pagetable_candidates;
    std::unordered_set<u32> patch_ids;
    std::unordered_map<u32, u32> locations;
    for (size_t i = 5; i < code.size();) {
        const u32 word = code[i];
        const u32 length = word >> spv::WordCountShift;
        const u32 op = word & spv::OpCodeMask;
        if (length == 0 || i + length > code.size()) {
            scan.valid = false;
            return scan;
        }
        switch (op) {
        case spv::OpCapability:
            if (length >= 2 && code[i + 1] == spv::CapabilityPhysicalStorageBufferAddresses) {
                scan.device_address = true;
            }
            break;
        case spv::OpEntryPoint:
            if (length >= 2 && scan.stage == ShaderStage::Unknown) {
                switch (code[i + 1]) {
                case spv::ExecutionModelVertex:
                    scan.stage = ShaderStage::Vertex;
                    break;
                case spv::ExecutionModelFragment:
                    scan.stage = ShaderStage::Fragment;
                    break;
                case spv::ExecutionModelGLCompute:
                    scan.stage = ShaderStage::Compute;
                    break;
                case spv::ExecutionModelTessellationControl:
                    scan.stage = ShaderStage::TessellationControl;
                    break;
                case spv::ExecutionModelTessellationEvaluation:
                    scan.stage = ShaderStage::TessellationEval;
                    break;
                case spv::ExecutionModelGeometry:
                    scan.stage = ShaderStage::Geometry;
                    break;
                default:
                    break;
                }
            }
            break;
        case spv::OpExecutionMode:
            if (length >= 6 && code[i + 2] == spv::ExecutionModeLocalSize) {
                scan.local_size = {code[i + 3], code[i + 4], code[i + 5]};
            }
            if (length >= 3) {
                switch (code[i + 2]) {
                case spv::ExecutionModeTriangles:
                    scan.tess.domain = TessDomain::Triangles;
                    break;
                case spv::ExecutionModeQuads:
                    scan.tess.domain = TessDomain::Quads;
                    break;
                case spv::ExecutionModeIsolines:
                    scan.tess.domain = TessDomain::Isolines;
                    break;
                case spv::ExecutionModeSpacingEqual:
                    scan.tess.spacing = TessSpacing::Equal;
                    break;
                case spv::ExecutionModeSpacingFractionalOdd:
                    scan.tess.spacing = TessSpacing::FractionalOdd;
                    break;
                case spv::ExecutionModeSpacingFractionalEven:
                    scan.tess.spacing = TessSpacing::FractionalEven;
                    break;
                case spv::ExecutionModeVertexOrderCw:
                    scan.tess.clockwise = true;
                    break;
                case spv::ExecutionModeVertexOrderCcw:
                    scan.tess.clockwise = false;
                    break;
                case spv::ExecutionModePointMode:
                    scan.tess.point_mode = true;
                    break;
                case spv::ExecutionModeOutputVertices:
                    if (length >= 4) {
                        scan.tess.output_vertices = code[i + 3];
                    }
                    break;
                default:
                    break;
                }
            }
            break;
        case spv::OpName:
            if (length >= 3 &&
                std::strncmp(reinterpret_cast<const char*>(&code[i + 2]), "bda_pagetable",
                             (length - 2) * sizeof(u32)) == 0) {
                scan.bda_pagetable = true;
            }
            break;
        case spv::OpDecorate:
            if (length >= 3 && code[i + 2] == spv::DecorationPerVertexKHR) {
                scan.per_vertex = true;
            }
            if (length >= 3 && code[i + 2] == spv::DecorationPatch) {
                patch_ids.insert(code[i + 1]);
            }
            if (length >= 4 && code[i + 2] == spv::DecorationLocation) {
                locations[code[i + 1]] = code[i + 3];
            }
            break;
        default:
            break;
        }
        i += length;
    }
    for (const u32 id : patch_ids) {
        if (const auto it = locations.find(id); it != locations.end()) {
            const bool first = scan.tess.patch_end == 0;
            scan.tess.patch_first = first ? it->second : std::min(scan.tess.patch_first, it->second);
            scan.tess.patch_end = std::max(scan.tess.patch_end, it->second + 1);
        }
    }
    return scan;
}

ScalarType ToScalarType(sc::SPIRType::BaseType base) {
    switch (base) {
    case sc::SPIRType::Float:
        return ScalarType::Float;
    case sc::SPIRType::Int:
        return ScalarType::Int;
    case sc::SPIRType::UInt:
        return ScalarType::Uint;
    default:
        return ScalarType::Other;
    }
}

/// CompilerMSL with access to the interface structs SPIRV-Cross declares (the size of the data
/// between the stages of a tessellated draw).
class TessCompilerMSL final : public sc::CompilerMSL {
public:
    using sc::CompilerMSL::CompilerMSL;

    /// MSL sizes of the structs; valid after compile().
    CompiledShader::TessLayout Layout() {
        CompiledShader::TessLayout layout;
        if (stage_in_var_id) {
            layout.input = InterfaceStructSize(get_stage_in_struct_type());
        }
        if (stage_out_var_id) {
            layout.output = InterfaceStructSize(get_stage_out_struct_type());
            layout.members = InterfaceMembers(get_stage_out_struct_type());
        }
        if (patch_stage_in_var_id) {
            layout.patch_input = InterfaceStructSize(get_patch_stage_in_struct_type());
        }
        if (patch_stage_out_var_id) {
            layout.patch_output = InterfaceStructSize(get_patch_stage_out_struct_type());
        }
        return layout;
    }

private:
    /// sizeof() of an interface struct as MSL lays it out: members in order at their natural
    /// alignment, the size rounded up to the largest. (The synthesized structs have no Offset
    /// decorations, which get_declared_struct_size_msl wants.)
    u32 InterfaceStructSize(const sc::SPIRType& type) const {
        u32 offset = 0, alignment = 1;
        for (u32 i = 0; i < type.member_types.size(); ++i) {
            const u32 member_alignment = get_declared_struct_member_alignment_msl(type, i);
            offset = (offset + member_alignment - 1) / member_alignment * member_alignment;
            offset += get_declared_struct_member_size_msl(type, i);
            alignment = std::max(alignment, member_alignment);
        }
        return (offset + alignment - 1) / alignment * alignment;
    }

    /// The scalar / vector members of an interface struct with their byte offsets (the same
    /// layout as InterfaceStructSize) and the Location or Position they carry.
    std::vector<InterfaceMember> InterfaceMembers(const sc::SPIRType& type) const {
        std::vector<InterfaceMember> members;
        u32 offset = 0;
        for (u32 i = 0; i < type.member_types.size(); ++i) {
            const u32 member_alignment = get_declared_struct_member_alignment_msl(type, i);
            offset = (offset + member_alignment - 1) / member_alignment * member_alignment;
            const sc::SPIRType& member_type = get_type(type.member_types[i]);
            const bool builtin = has_member_decoration(type.self, i, spv::DecorationBuiltIn);
            if (member_type.array.empty() && member_type.columns == 1 &&
                (builtin || has_member_decoration(type.self, i, spv::DecorationLocation))) {
                InterfaceMember m;
                m.offset = offset;
                m.components = member_type.vecsize;
                m.type = ToScalarType(member_type.basetype);
                if (builtin) {
                    m.position = get_member_decoration(type.self, i, spv::DecorationBuiltIn) ==
                                 spv::BuiltInPosition;
                } else {
                    m.location = get_member_decoration(type.self, i, spv::DecorationLocation);
                }
                if (builtin ? m.position : true) {
                    members.push_back(m);
                }
            }
            offset += get_declared_struct_member_size_msl(type, i);
        }
        return members;
    }
};

/// SPIR-V -> MSL; fills the binding map, the flags and the entry name of `out` (not the Metal
/// objects). The entry point is renamed `entry_name`. Throws spirv_cross::CompilerError, or
/// std::runtime_error for a resource limit.
std::string Convert(std::span<const u32> spirv, const SpirvScan& scan,
                    const CompileVariant& variant, const std::string& entry_name,
                    CompiledShader& out) {
    TessCompilerMSL comp(spirv.data(), spirv.size());
    const auto entry_points = comp.get_entry_points_and_stages();
    if (entry_points.empty()) {
        throw std::runtime_error("no entry point");
    }
    const auto model = entry_points[0].execution_model;
    // bbport: one function name per shader, not SPIRV-Cross' `main0` for all. Shaders with
    // different guest hashes but the same SPIR-V would otherwise be identical functions in
    // separate libraries, and the binary archive cannot be serialized once it holds both
    // ("missing 'vertex' stage in pipeline no. N"; tests/test_mtl_archive.mm, case k).
    comp.rename_entry_point(entry_points[0].name, entry_name, model);

    const sc::ShaderResources res = comp.get_shader_resources();
    out.uses_push_constants = !res.push_constant_buffers.empty();

    // One slot per (set, binding) per Metal resource class. Variables that alias a (set, binding)
    // (the recompiler's struct views of one SSBO) share it.
    struct Slot {
        sc::SPIRType::BaseType basetype{};
        Kind kind{};
        bool buffer{}, texture{}, sampler{};
        u32 count{1};
        u32 buffer_index{}, texture_index{}, sampler_index{};
    };
    std::map<std::pair<u32, u32>, Slot> slots;
    auto array_count = [&](const sc::Resource& r) -> u32 {
        const sc::SPIRType& type = comp.get_type(r.type_id);
        if (type.array.empty()) {
            return 1;
        }
        if (type.array.size() != 1 || !type.array_size_literal[0] || type.array[0] == 0) {
            throw std::runtime_error("unsupported resource array (specialization-constant, "
                                     "unsized or multi-dimensional)");
        }
        return type.array[0];
    };
    auto collect = [&](const sc::SmallVector<sc::Resource>& list, sc::SPIRType::BaseType basetype,
                       Kind kind, bool buffer, bool texture, bool sampler) {
        for (const sc::Resource& r : list) {
            const u32 set = comp.get_decoration(r.id, spv::DecorationDescriptorSet);
            const u32 binding = comp.get_decoration(r.id, spv::DecorationBinding);
            if (variant.kind == CompileVariant::Kind::GeometryKernel &&
                set == kGeometryKernelSet) {
                continue; // the kernel's own buffers: fixed slots below
            }
            if (slots.contains({set, binding})) {
                continue;
            }
            Kind slot_kind = kind;
            if (kind == Kind::Texture || kind == Kind::StorageTexture) {
                if (comp.get_type(r.type_id).image.dim == spv::DimBuffer) {
                    slot_kind = Kind::TexelBuffer;
                }
            }
            slots[{set, binding}] = {.basetype = basetype,
                                     .kind = slot_kind,
                                     .buffer = buffer,
                                     .texture = texture,
                                     .sampler = sampler,
                                     .count = array_count(r)};
        }
    };
    collect(res.uniform_buffers, sc::SPIRType::Struct, Kind::Buffer, true, false, false);
    collect(res.storage_buffers, sc::SPIRType::Struct, Kind::Buffer, true, false, false);
    collect(res.storage_images, sc::SPIRType::Image, Kind::StorageTexture, false, true, false);
    collect(res.sampled_images, sc::SPIRType::SampledImage, Kind::Texture, false, true, true);
    collect(res.separate_images, sc::SPIRType::Image, Kind::Texture, false, true, false);
    collect(res.separate_samplers, sc::SPIRType::Sampler, Kind::Sampler, false, false, true);
    collect(res.subpass_inputs, sc::SPIRType::Image, Kind::Texture, false, true, false);

    u32 next_buffer = kFirstResourceBuffer, next_texture = 0, next_sampler = 0;
    out.bindings.clear();
    out.bindings.push_back({~0u, ~0u, Kind::PushConstants, kPushConstantBuffer, 1});
    for (auto& [key, slot] : slots) { // std::map: sorted by (set, binding)
        const auto [set, binding] = key;
        sc::MSLResourceBinding b;
        b.stage = model;
        b.basetype = slot.basetype;
        b.desc_set = set;
        b.binding = binding;
        b.count = slot.count;
        if (slot.buffer) {
            slot.buffer_index = b.msl_buffer = next_buffer;
            out.bindings.push_back({set, binding, Kind::Buffer, next_buffer, slot.count});
            next_buffer += slot.count;
        }
        if (slot.texture) {
            slot.texture_index = b.msl_texture = next_texture;
            out.bindings.push_back({set, binding, slot.kind, next_texture, slot.count});
            next_texture += slot.count;
        }
        if (slot.sampler) {
            slot.sampler_index = b.msl_sampler = next_sampler;
            out.bindings.push_back({set, binding, Kind::Sampler, next_sampler, slot.count});
            next_sampler += slot.count;
        }
        comp.add_msl_resource_binding(b);
    }
    {
        sc::MSLResourceBinding b;
        b.stage = model;
        b.basetype = sc::SPIRType::Struct;
        b.desc_set = sc::kPushConstDescSet;
        b.binding = sc::kPushConstBinding;
        b.msl_buffer = kPushConstantBuffer;
        comp.add_msl_resource_binding(b);
    }

    if (variant.kind == CompileVariant::Kind::GeometryKernel) {
        for (u32 binding = 0; binding < 4; ++binding) {
            sc::MSLResourceBinding b;
            b.stage = model;
            b.basetype = sc::SPIRType::Struct;
            b.desc_set = kGeometryKernelSet;
            b.binding = binding;
            b.msl_buffer = kGeoEsBuffer + binding;
            comp.add_msl_resource_binding(b);
        }
    }
    // The fragment shader's used stage inputs (the geometry pass-through function declares them).
    out.stage_inputs.clear();
    if (model == spv::ExecutionModelFragment) {
        for (const sc::Resource& r : res.stage_inputs) {
            if (comp.has_decoration(r.id, spv::DecorationBuiltIn)) {
                continue;
            }
            const sc::SPIRType& type = comp.get_type(r.type_id);
            const u32 location = comp.get_decoration(r.id, spv::DecorationLocation);
            u32 count = 1;
            if (!type.array.empty()) {
                count = type.array.size() == 1 && type.array_size_literal[0] ? type.array[0] : 1;
            }
            for (u32 e = 0; e < count; ++e) {
                out.stage_inputs.push_back({location + e, ToScalarType(type.basetype),
                                            type.columns == 1 ? type.vecsize : 0});
            }
        }
        std::sort(out.stage_inputs.begin(), out.stage_inputs.end(),
                  [](const StageInput& a, const StageInput& b) { return a.location < b.location; });
    }

    // SPIRV-Cross' buffer of buffer sizes (OpArrayLength) goes behind the resource buffers.
    const u32 size_buffer_slot = next_buffer;
    sc::CompilerMSL::Options options;
    options.platform = sc::CompilerMSL::Options::macOS;
    options.set_msl_version(3, 1);
    options.argument_buffers = false;
    // bbport: 1D images as 2D, as MoltenVK compiles them (its 1D images are 2D textures).
    options.texture_1D_as_2D = true;
    options.buffer_size_buffer_index = size_buffer_slot;
    using VariantKind = CompileVariant::Kind;
    if (variant.kind != VariantKind::Plain && variant.kind != VariantKind::GeometryKernel) {
        // Tessellation (P4.4): the slots between the stages (mtl_shader.h) and the stage's form.
        options.shader_output_buffer_index = kTessOutputBuffer;
        options.shader_index_buffer_index = kTessIndexBuffer;
        options.shader_patch_output_buffer_index = kTessPatchBuffer;
        options.shader_patch_input_buffer_index = kTessPatchBuffer;
        options.shader_tess_factor_buffer_index = kTessFactorBuffer;
        options.indirect_params_buffer_index = kTessParamsBuffer;
        options.shader_input_buffer_index = kTessInputBuffer;
        options.tess_domain_origin_lower_left = false; // Vulkan's default, Metal's native origin
        switch (variant.kind) {
        case VariantKind::TessVertex:
            options.vertex_for_tessellation = true;
            options.capture_output_to_buffer = true;
            options.vertex_index_type =
                variant.index_type == 1   ? sc::CompilerMSL::Options::IndexType::UInt16
                : variant.index_type == 2 ? sc::CompilerMSL::Options::IndexType::UInt32
                                          : sc::CompilerMSL::Options::IndexType::None;
            break;
        case VariantKind::TessControl:
            // One thread per output control point, across patches.
            options.multi_patch_workgroup = true;
            options.capture_output_to_buffer = true;
            break;
        case VariantKind::TessEval:
            options.raw_buffer_tese_input = true;
            break;
        case VariantKind::Plain:
        case VariantKind::GeometryKernel:
            break;
        }
    }
    comp.set_msl_options(options);
    if (variant.kind == VariantKind::TessControl) {
        // The tessellation factor struct (quad or triangle) follows the domain, which the control
        // shader need not declare.
        const auto& modes = comp.get_execution_mode_bitset();
        if (!modes.get(spv::ExecutionModeTriangles) && !modes.get(spv::ExecutionModeQuads) &&
            !modes.get(spv::ExecutionModeIsolines)) {
            if (variant.domain != TessDomain::Triangles && variant.domain != TessDomain::Quads) {
                throw std::runtime_error("no triangle or quad domain for the control shader");
            }
            comp.set_execution_mode(variant.domain == TessDomain::Triangles
                                        ? spv::ExecutionModeTriangles
                                        : spv::ExecutionModeQuads);
        }
    } else if (variant.kind == VariantKind::TessEval) {
        // The patch size of the post-tessellation function (patch(quad, N), the stride of the
        // control point buffer) is the control shader's output count.
        if (variant.output_vertices == 0) {
            throw std::runtime_error("no output vertex count for the evaluation shader");
        }
        comp.set_execution_mode(spv::ExecutionModeOutputVertices, variant.output_vertices);
        // Every patch constant the control shader writes, read or not: SPIRV-Cross then adds
        // placeholders for the unread ones (raw_buffer_tese_input), so the struct matches the
        // control shader's in layout and size. (A TES reading locations 4-13 of 0-13 had them at
        // offset 0 and a 160-byte stride against the TCS's 224: the pipeline was refused.)
        for (u32 location = variant.patch_first; location < variant.patch_end; ++location) {
            sc::MSLShaderInterfaceVariable input;
            input.location = location;
            input.vecsize = 4;
            input.rate = sc::MSL_SHADER_VARIABLE_RATE_PER_PATCH;
            comp.add_msl_shader_input(input);
        }
    }

    std::string msl = comp.compile();

    out.entry = comp.get_cleansed_entry_point_name(entry_name, model);
    out.num_buffers = next_buffer;
    out.num_textures = next_texture;
    out.num_samplers = next_sampler;
    if (comp.needs_buffer_size_buffer()) {
        out.buffer_size_buffer = size_buffer_slot;
        ++out.num_buffers;
    }
    const u32 max_buffers = variant.kind != VariantKind::Plain ? kTessFirstReservedBuffer
                            : scan.stage == ShaderStage::Vertex ? kMaxBuffers - kMaxVertexBuffers
                                                                : kMaxBuffers;
    if (out.num_buffers > max_buffers || out.num_textures > kMaxTextures ||
        out.num_samplers > kMaxSamplers) {
        throw std::runtime_error(fmt::format(
            "resource limit exceeded: {} buffers (max {}), {} textures (max {}), {} samplers "
            "(max {})",
            out.num_buffers, max_buffers, out.num_textures, kMaxTextures, out.num_samplers,
            kMaxSamplers));
    }
    std::stable_sort(out.bindings.begin() + 1, out.bindings.end(), [](const ShaderBinding& a, const ShaderBinding& b) {
        return std::tie(a.set, a.binding, a.kind) < std::tie(b.set, b.binding, b.kind);
    });
    if (variant.kind != VariantKind::Plain && variant.kind != VariantKind::GeometryKernel) {
        out.tess_layout = comp.Layout();
    }
    return msl;
}

// ---- Disk cache ----------------------------------------------------------------------------
//
// <dir>/<hash:016x>_<stage>_<xxh3(SPIR-V words):016x>.metal, plain text:
//   // bbport-msl <kMslCacheVersion>
//   // spirv-cross <version>
//   // stage <name>
//   // entry <MSL function name>
//   // flags push=<0|1> bda=<0|1> pagetable=<0|1> buffer_size=<slot or ->
//   // counts <buffers> <textures> <samplers>
//   // tess <input> <output> <patch input> <patch output>          (tessellation variants only)
//   // bind <set> <binding> <kind name> <msl index> <count>        (one line per binding)
//   // msl-bytes <n>
//   // ---- MSL
//   <the MSL, n bytes>
// A file that does not parse, has another version or SPIRV-Cross version, or whose MSL is not n
// bytes long, is a miss and is overwritten. Writes go to a temporary name and are renamed.

std::string CacheText(const CompiledShader& shader, const std::string& msl) {
    std::ostringstream text;
    text << kCacheMagic << ' ' << kMslCacheVersion << '\n'
         << "// spirv-cross " << kSpirvCrossVersion << '\n'
         << "// stage " << ShaderStageName(shader.stage) << '\n'
         << "// entry " << shader.entry << '\n'
         << "// flags push=" << shader.uses_push_constants << " bda=" << shader.uses_device_address
         << " pagetable=" << shader.uses_bda_pagetable << " buffer_size=";
    if (shader.buffer_size_buffer == ~0u) {
        text << '-';
    } else {
        text << shader.buffer_size_buffer;
    }
    text << '\n'
         << "// counts " << shader.num_buffers << ' ' << shader.num_textures << ' '
         << shader.num_samplers << '\n';
    if (shader.variant.kind != CompileVariant::Kind::Plain &&
        shader.variant.kind != CompileVariant::Kind::GeometryKernel) {
        text << "// tess " << shader.tess_layout.input << ' ' << shader.tess_layout.output << ' '
             << shader.tess_layout.patch_input << ' ' << shader.tess_layout.patch_output << '\n';
        text << "// tessout " << shader.tess_layout.members.size();
        for (const InterfaceMember& m : shader.tess_layout.members) {
            text << ' ';
            if (m.position) {
                text << 'P';
            } else {
                text << m.location;
            }
            text << ':' << m.offset << ':' << m.components << ':' << u32(m.type);
        }
        text << '\n';
    }
    text << "// stagein " << shader.stage_inputs.size();
    for (const StageInput& input : shader.stage_inputs) {
        text << ' ' << input.location << ':' << input.components << ':' << u32(input.type);
    }
    text << '\n';
    for (const ShaderBinding& b : shader.bindings) {
        text << "// bind " << b.set << ' ' << b.binding << ' ' << KindName(b.kind) << ' '
             << b.msl_index << ' ' << b.count << '\n';
    }
    text << "// msl-bytes " << msl.size() << '\n' << kCacheMslMarker << '\n' << msl;
    return text.str();
}

bool ParseCache(const std::string& text, CompiledShader& shader, std::string& msl) {
    size_t pos = 0;
    auto next_line = [&](std::string& line) {
        if (pos >= text.size()) {
            return false;
        }
        const size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            return false;
        }
        line = text.substr(pos, end - pos);
        pos = end + 1;
        return true;
    };
    auto field = [&](const char* prefix, std::string& value) {
        std::string line;
        const std::string p = std::string("// ") + prefix + ' ';
        if (!next_line(line) || !line.starts_with(p)) {
            return false;
        }
        value = line.substr(p.size());
        return true;
    };
    std::string line, value;
    if (!next_line(line) || line != std::string(kCacheMagic) + ' ' + std::to_string(kMslCacheVersion)) {
        return false;
    }
    if (!field("spirv-cross", value) || value != kSpirvCrossVersion) {
        return false;
    }
    if (!field("stage", value)) {
        return false;
    }
    shader.stage = ShaderStage::Unknown;
    for (ShaderStage s : {ShaderStage::Vertex, ShaderStage::Fragment, ShaderStage::Compute,
                          ShaderStage::TessellationControl, ShaderStage::TessellationEval,
                          ShaderStage::Geometry}) {
        if (value == ShaderStageName(s)) {
            shader.stage = s;
        }
    }
    if (shader.stage == ShaderStage::Unknown || !field("entry", shader.entry) ||
        shader.entry.empty()) {
        return false;
    }
    if (!field("flags", value)) {
        return false;
    }
    {
        int push = 0, bda = 0, pagetable = 0;
        char size_buffer[16]{};
        if (std::sscanf(value.c_str(), "push=%d bda=%d pagetable=%d buffer_size=%15s", &push, &bda,
                        &pagetable, size_buffer) != 4) {
            return false;
        }
        shader.uses_push_constants = push != 0;
        shader.uses_device_address = bda != 0;
        shader.uses_bda_pagetable = pagetable != 0;
        shader.buffer_size_buffer =
            size_buffer[0] == '-' ? ~0u : static_cast<u32>(std::strtoul(size_buffer, nullptr, 10));
    }
    if (!field("counts", value) ||
        std::sscanf(value.c_str(), "%u %u %u", &shader.num_buffers, &shader.num_textures,
                    &shader.num_samplers) != 3) {
        return false;
    }
    shader.bindings.clear();
    shader.tess_layout = {};
    shader.stage_inputs.clear();
    bool first_line = true;
    bool have_stage_in = false;
    for (;;) {
        if (!next_line(line)) {
            return false;
        }
        if (first_line && line.starts_with("// tess ")) {
            first_line = false;
            if (std::sscanf(line.c_str(), "// tess %u %u %u %u", &shader.tess_layout.input,
                            &shader.tess_layout.output, &shader.tess_layout.patch_input,
                            &shader.tess_layout.patch_output) != 4) {
                return false;
            }
            if (!next_line(line) || !line.starts_with("// tessout ")) {
                return false;
            }
            std::istringstream members(line.substr(11));
            size_t count = 0;
            members >> count;
            for (size_t i = 0; i < count; ++i) {
                std::string token;
                if (!(members >> token)) {
                    return false;
                }
                InterfaceMember m;
                unsigned offset = 0, components = 0, type = 0;
                char location[16]{};
                if (std::sscanf(token.c_str(), "%15[^:]:%u:%u:%u", location, &offset, &components,
                                &type) != 4 ||
                    type > 3) {
                    return false;
                }
                if (location[0] == 'P') {
                    m.position = true;
                } else {
                    m.location = static_cast<u32>(std::strtoul(location, nullptr, 10));
                }
                m.offset = offset;
                m.components = components;
                m.type = static_cast<ScalarType>(type);
                shader.tess_layout.members.push_back(m);
            }
            continue;
        }
        first_line = false;
        if (!have_stage_in) {
            // Always present, before the bindings.
            if (!line.starts_with("// stagein ")) {
                return false;
            }
            have_stage_in = true;
            std::istringstream inputs(line.substr(11));
            size_t count = 0;
            inputs >> count;
            for (size_t i = 0; i < count; ++i) {
                std::string token;
                if (!(inputs >> token)) {
                    return false;
                }
                StageInput input;
                unsigned location = 0, components = 0, type = 0;
                if (std::sscanf(token.c_str(), "%u:%u:%u", &location, &components, &type) != 3 ||
                    type > 3) {
                    return false;
                }
                input.location = location;
                input.components = components;
                input.type = static_cast<ScalarType>(type);
                shader.stage_inputs.push_back(input);
            }
            continue;
        }
        if (!line.starts_with("// bind ")) {
            break;
        }
        ShaderBinding b;
        char kind_name[32]{};
        if (std::sscanf(line.c_str(), "// bind %u %u %31s %u %u", &b.set, &b.binding, kind_name,
                        &b.msl_index, &b.count) != 5) {
            return false;
        }
        bool known = false;
        for (Kind k : {Kind::Buffer, Kind::Texture, Kind::StorageTexture, Kind::Sampler,
                       Kind::TexelBuffer, Kind::PushConstants}) {
            if (std::strcmp(kind_name, KindName(k)) == 0) {
                b.kind = k;
                known = true;
            }
        }
        if (!known) {
            return false;
        }
        shader.bindings.push_back(b);
    }
    // `line` is the msl-bytes line.
    unsigned long long bytes = 0;
    if (std::sscanf(line.c_str(), "// msl-bytes %llu", &bytes) != 1) {
        return false;
    }
    if (!next_line(line) || line != kCacheMslMarker) {
        return false;
    }
    if (text.size() - pos != bytes) {
        return false;
    }
    msl = text.substr(pos);
    return true;
}

bool ReadFile(const std::filesystem::path& path, std::string& text) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return file.good() || file.eof();
}

bool WriteFileAtomic(const std::filesystem::path& path, const std::string& text) {
    static std::atomic<u32> counter{0};
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto temp = path;
    temp += fmt::format(".tmp{}_{}", getpid(), counter.fetch_add(1));
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.flush();
        if (!file) {
            file.close();
            std::filesystem::remove(temp, ec);
            return false;
        }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

// ---- Metal ---------------------------------------------------------------------------------

struct MetalObjects {
    id<MTLLibrary> library;
    id<MTLFunction> function;
    std::string error; ///< empty on success
};

/// The first "error:" line of a Metal compiler message, without the "program_source:L:C:" prefix.
std::string FirstMetalError(const std::string& text) {
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t end = text.find('\n', pos);
        const std::string line =
            text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        const auto at = line.find("error:");
        if (at != std::string::npos) {
            return line.substr(at);
        }
        if (end == std::string::npos) {
            break;
        }
        pos = end + 1;
    }
    return FirstLine(text);
}

MetalObjects BuildLibrary(id<MTLDevice> device, const std::string& msl, const std::string& entry,
                          std::string_view label) {
    MetalObjects objects{};
    @autoreleasepool {
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion3_1;
        // bbport: fast math, as MoltenVK compiles our shaders today (its default); the recompiler
        // marks the operations that must stay exact (vertex stages, SPIR-V NoContraction) and
        // SPIRV-Cross keeps those exact. BB_MSL_SAFE_MATH=1: Metal's safe math everywhere.
        static const bool safe_math = [] {
            const char* env = std::getenv("BB_MSL_SAFE_MATH");
            return env && env[0] == '1';
        }();
        if (@available(macOS 15.0, *)) {
            options.mathMode = safe_math ? MTLMathModeSafe : MTLMathModeFast;
        } else {
            options.fastMathEnabled = !safe_math;
        }
        // bbport: positions declared Invariant (BB_SPIRV_PRECISE=invariant) only match across
        // shaders with this, as MoltenVK set it (emit_spirv_floating_point.cpp).
        options.preserveInvariance = msl.find(", invariant]]") != std::string::npos;
        NSError* error = nil;
        id<MTLLibrary> library =
            [device newLibraryWithSource:[NSString stringWithUTF8String:msl.c_str()]
                                 options:options
                                   error:&error];
        if (!library) {
            objects.error = "Metal: " + FirstMetalError(error ? error.localizedDescription.UTF8String
                                                              : "unknown error");
            return objects;
        }
        library.label = [[NSString alloc] initWithBytes:label.data()
                                                 length:label.size()
                                               encoding:NSUTF8StringEncoding];
        id<MTLFunction> function =
            [library newFunctionWithName:[NSString stringWithUTF8String:entry.c_str()]];
        if (!function) {
            objects.error = fmt::format("Metal: function '{}' not found in the library", entry);
            return objects;
        }
        objects.library = library;
        objects.function = function;
    }
    return objects;
}

} // namespace

const char* ShaderStageName(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex:
        return "vertex";
    case ShaderStage::Fragment:
        return "fragment";
    case ShaderStage::Compute:
        return "compute";
    case ShaderStage::TessellationControl:
        return "tess_control";
    case ShaderStage::TessellationEval:
        return "tess_eval";
    case ShaderStage::Geometry:
        return "geometry";
    case ShaderStage::Unknown:
        break;
    }
    return "unknown";
}

// CompiledShader's destructor and Find are in mtl_compiled_shader.mm (bbmetal_core).

std::filesystem::path ShaderCompiler::DefaultCacheDir() {
    using namespace Common::FS;
    return GetUserPath(PathType::CacheDir) / std::filesystem::path{Common::ElfInfo::Instance().GameSerial()} /
           "metal";
}

ShaderCompiler::ShaderCompiler(Device& device_, std::filesystem::path cache_dir_)
    : device{device_}, cache_dir{std::move(cache_dir_)} {
    if (cache_dir.empty()) {
        cache_dir = DefaultCacheDir();
    }
}

ShaderCompiler::~ShaderCompiler() = default;

TessellationInfo ReadTessellationInfo(std::span<const u32> spirv) {
    return ScanSpirv(spirv).tess;
}

ShaderStage ReadShaderStage(std::span<const u32> spirv) {
    return ScanSpirv(spirv).stage;
}

CompileResult ShaderCompiler::Compile(std::span<const u32> spirv, u64 hash, std::string_view name,
                                      const CompileVariant& variant) {
    CompileResult result;
    const SpirvScan scan = ScanSpirv(spirv);
    result.stage = scan.stage;
    const char* stage_name = ShaderStageName(scan.stage);
    auto fail = [&](CompileStatus status, std::string message) -> CompileResult {
        result.status = status;
        result.error = std::move(message);
        std::atomic<u64>* counter = nullptr;
        switch (status) {
        case CompileStatus::UnsupportedStage:
            counter = &counters.unsupported_stages;
            break;
        case CompileStatus::PerVertexInput:
            counter = &counters.pervertex_failures;
            break;
        default:
            counter = &counters.compile_failures;
            break;
        }
        ++*counter;
        LOG_ERROR(Render, "Metal shader {:#018x} {} ({}): {}", hash, name, stage_name, result.error);
        return std::move(result);
    };

    if (!scan.valid) {
        return fail(CompileStatus::Failed, "not a valid SPIR-V module");
    }
    if (scan.stage == ShaderStage::Unknown) {
        return fail(CompileStatus::Failed, "no supported entry point");
    }
    using VariantKind = CompileVariant::Kind;
    switch (variant.kind) {
    case VariantKind::Plain:
        if (scan.stage == ShaderStage::TessellationControl ||
            scan.stage == ShaderStage::TessellationEval) {
            return fail(CompileStatus::UnsupportedStage,
                        "a tessellation stage compiles only as a variant of a tessellated draw");
        }
        if (scan.stage == ShaderStage::Geometry) {
            return fail(CompileStatus::UnsupportedStage,
                        "a geometry stage compiles only as the kernel of a geometry draw");
        }
        if (scan.stage != ShaderStage::Vertex && scan.stage != ShaderStage::Fragment &&
            scan.stage != ShaderStage::Compute) {
            return fail(CompileStatus::UnsupportedStage, "stage not supported");
        }
        break;
    case VariantKind::TessVertex:
        if (scan.stage != ShaderStage::Vertex) {
            return fail(CompileStatus::Failed, "tessellation vertex variant of another stage");
        }
        break;
    case VariantKind::TessControl:
        if (scan.stage != ShaderStage::TessellationControl) {
            return fail(CompileStatus::Failed, "tessellation control variant of another stage");
        }
        break;
    case VariantKind::TessEval:
        if (scan.stage != ShaderStage::TessellationEval) {
            return fail(CompileStatus::Failed, "tessellation evaluation variant of another stage");
        }
        if (scan.tess.domain != TessDomain::Triangles && scan.tess.domain != TessDomain::Quads) {
            return fail(CompileStatus::Failed, "Metal tessellates triangles and quads only");
        }
        break;
    case VariantKind::GeometryKernel:
        if (scan.stage != ShaderStage::Geometry) {
            return fail(CompileStatus::Failed, "geometry kernel variant of another stage");
        }
        break;
    }

    auto shader = std::make_unique<CompiledShader>();
    shader->stage = scan.stage;
    shader->uses_device_address = scan.device_address;
    shader->local_size = scan.local_size;
    shader->uses_bda_pagetable = scan.bda_pagetable;
    shader->variant = variant;
    shader->tess = scan.tess;

    // The geometry shader becomes a compute kernel before anything else; its shape is read from
    // the SPIR-V every time (also on a disk cache hit).
    GeometryRewrite geometry;
    if (variant.kind == VariantKind::GeometryKernel) {
        geometry = RewriteGeometryShader(spirv, variant.es);
        if (!geometry.ok) {
            return fail(CompileStatus::UnsupportedStage, "geometry shader: " + geometry.error);
        }
        shader->geometry = geometry.info;
        shader->local_size = {kGeometryThreads, 1, 1};
    }

    const u64 spirv_hash = XXH3_64bits(spirv.data(), spirv.size_bytes());
    std::string tag; // the variants have their own cache files
    switch (variant.kind) {
    case VariantKind::Plain:
        break;
    case VariantKind::TessVertex:
        tag = fmt::format("_tv{}", variant.index_type);
        break;
    case VariantKind::TessControl:
        tag = fmt::format("_tc{}", u32(variant.domain));
        break;
    case VariantKind::TessEval:
        tag = fmt::format("_te{}", variant.output_vertices);
        if (variant.patch_end != 0) {
            tag += fmt::format("p{}_{}", variant.patch_first, variant.patch_end);
        }
        break;
    case VariantKind::GeometryKernel: {
        // The rewritten module depends on the export layout.
        std::vector<u32> layout{variant.es.stride};
        for (const InterfaceMember& m : variant.es.members) {
            layout.insert(layout.end(),
                          {m.location, u32(m.position), m.offset, m.components, u32(m.type)});
        }
        tag = fmt::format("_gk{:016x}", XXH3_64bits(layout.data(), layout.size() * sizeof(u32)));
        break;
    }
    }
    const std::filesystem::path cache_file =
        cache_dir / fmt::format("{:016x}_{}{}_{:016x}.metal", hash, stage_name, tag, spirv_hash);
    id<MTLDevice> mtl_device = device.GetImpl().device;
    const std::string label{name};

    MetalObjects objects{};
    std::string msl;
    bool cached = false;
    {
        std::string text;
        if (ReadFile(cache_file, text)) {
            CompiledShader parsed;
            std::string cached_msl;
            if (ParseCache(text, parsed, cached_msl) && parsed.stage == scan.stage) {
                objects = BuildLibrary(mtl_device, cached_msl, parsed.entry, label);
                if (objects.error.empty()) {
                    shader->entry = std::move(parsed.entry);
                    shader->bindings = std::move(parsed.bindings);
                    shader->uses_push_constants = parsed.uses_push_constants;
                    shader->buffer_size_buffer = parsed.buffer_size_buffer;
                    shader->num_buffers = parsed.num_buffers;
                    shader->num_textures = parsed.num_textures;
                    shader->num_samplers = parsed.num_samplers;
                    shader->tess_layout = std::move(parsed.tess_layout);
                    shader->stage_inputs = std::move(parsed.stage_inputs);
                    msl = std::move(cached_msl);
                    cached = true;
                } else {
                    // A cached text the Metal compiler rejects now (new OS?): convert again.
                    std::error_code ec;
                    std::filesystem::remove(cache_file, ec);
                }
            }
        }
    }

    if (!cached) {
        // PerVertexKHR inputs: SPIRV-Cross cannot translate them, the rewrite replaces them with
        // ordinary inputs and barycentric arithmetic (P3.1b).
        std::span<const u32> module = spirv;
        PerVertexRewrite rewritten;
        if (variant.kind == VariantKind::GeometryKernel) {
            module = geometry.spirv;
        } else if (scan.per_vertex) {
            rewritten = RewritePerVertexInputs(spirv);
            if (!rewritten.ok) {
                return fail(CompileStatus::PerVertexInput,
                            "PerVertexKHR input (P3.1b): " + rewritten.error);
            }
            module = rewritten.spirv;
        }
        try {
            // Unique per MSL cache file: the binary archive must never see the same function
            // in two libraries (Convert).
            const std::string entry_name =
                fmt::format("{}{}_{:016x}_{:016x}", stage_name, tag, hash, spirv_hash);
            msl = Convert(module, scan, variant, entry_name, *shader);
        } catch (const std::exception& e) {
            return fail(CompileStatus::Failed, "SPIRV-Cross: " + FirstLine(e.what()));
        }
        objects = BuildLibrary(mtl_device, msl, shader->entry, label);
        if (!objects.error.empty()) {
            return fail(CompileStatus::Failed, objects.error);
        }
        if (WriteFileAtomic(cache_file, CacheText(*shader, msl))) {
            ++counters.cache_stores;
        }
    } else {
        ++counters.cache_hits;
    }
    shader->from_disk_cache = cached;
    shader->library = (__bridge_retained void*)objects.library;
    shader->function = (__bridge_retained void*)objects.function;

    ++counters.compiled;
    result.status = CompileStatus::Ok;
    result.shader = std::move(shader);
    return result;
}

} // namespace Metal
