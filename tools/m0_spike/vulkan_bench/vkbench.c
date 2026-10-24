// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// M0(c) spike: synthetic Vulkan workload shaped like Bloodborne's measured frame
// (docs/FINDINGS.md Part 2), built from this one source as x86-64 (Rosetta) and
// arm64 (native). Both builds link MoltenVK directly (no loader), headless.
//
// Per frame: ~270 dynamic-rendering passes, ~1,600 indexed draws, 20 graphics pipelines,
// ~95 compute dispatches, push descriptors + push constants + dynamic viewport/scissor per
// draw, pipeline barriers, buffer copies, ~60 vkQueueSubmit calls, fence wait at the end.
// Measures CPU (thread + process) and wall time of recording and submission.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include <vulkan/vulkan.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "shaders_spv.h"

#define CHECK(x)                                                                     \
    do {                                                                             \
        VkResult r_ = (x);                                                           \
        if (r_ != VK_SUCCESS) {                                                      \
            fprintf(stderr, "%s:%d: %s -> %d\n", __FILE__, __LINE__, #x, (int)r_);   \
            exit(1);                                                                 \
        }                                                                            \
    } while (0)

// ---------------------------------------------------------------- deterministic RNG
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}
static uint32_t rnd_range(uint32_t lo, uint32_t hi) { return lo + rnd() % (hi - lo + 1); }

// ---------------------------------------------------------------- timing
static mach_timebase_info_data_t g_tb;
static inline uint64_t wall_ns(void) { return mach_absolute_time() * g_tb.numer / g_tb.denom; }
// NOTE: CLOCK_THREAD_CPUTIME_ID is wrong under Rosetta 2 (it reports only a few % of the real
// thread time for translated code), so thread CPU time comes from THREAD_EXTENDED_INFO, which is
// correct in both builds (checked against wall time on a busy loop).
static mach_port_t g_thread;
static inline uint64_t tcpu_ns(void) {
    thread_extended_info_data_t b;
    mach_msg_type_number_t n = THREAD_EXTENDED_INFO_COUNT;
    thread_info(g_thread, THREAD_EXTENDED_INFO, (thread_info_t)&b, &n);
    return b.pth_user_time + b.pth_system_time;
}
static inline uint64_t pcpu_ns(void) { return clock_gettime_nsec_np(CLOCK_PROCESS_CPUTIME_ID); }

// ---------------------------------------------------------------- workload description
enum PassKind { PK_GBUF, PK_SHADOW, PK_SMALL32, PK_SMALL64, PK_FULL, PK_COUNT };
enum { NUM_PIPES = 20, NUM_TEX = 24, NUM_SMALL_RT = 16, NUM_SHADOW_RT = 2, NUM_FULL_RT = 3 };
enum { MESH_VERTS = 64, MESH_INDICES = 4096, UBO_SLOTS = 2048, UBO_STRIDE = 256 };

// pipeline index ranges per pass kind
static const int kPipeFirst[PK_COUNT] = {0, 6, 9, 9, 14};
static const int kPipeCount[PK_COUNT] = {6, 3, 5, 5, 6};

typedef struct {
    uint16_t pipe, tex;
    uint16_t index_count, first_index;
    int16_t vertex_offset;
    uint16_t ubo_slot;
    uint16_t vp_x, vp_y, vp_w, vp_h;
} Draw;

typedef struct {
    uint8_t kind, target, clear, barrier_before;
    uint8_t copies_before, dispatches_before, submit_after;
    uint32_t first_draw, draw_count;
} Pass;

static Pass *g_passes;
static uint32_t g_pass_count;
static Draw *g_draws;
static uint32_t g_draw_count;

static void build_workload(void) {
    // Pass mix (rough shape of the measured frame): a few shadow passes, a G-buffer split into
    // several passes by mid-frame copies, many tiny post/lighting passes, a few fullscreen ones.
    enum { MAX_PASSES = 512, MAX_DRAWS = 4096 };
    g_passes = calloc(MAX_PASSES, sizeof(Pass));
    g_draws = calloc(MAX_DRAWS, sizeof(Draw));
    uint32_t np = 0, nd = 0;
    uint8_t kinds[MAX_PASSES];
    uint32_t nk = 0;
    for (int i = 0; i < 4; i++) kinds[nk++] = PK_SHADOW;
    for (int i = 0; i < 8; i++) kinds[nk++] = PK_GBUF;
    for (int i = 0; i < 246; i++) kinds[nk++] = (rnd() & 1) ? PK_SMALL32 : PK_SMALL64;
    for (int i = 0; i < 12; i++) kinds[nk++] = PK_FULL;
    // keep shadow + gbuffer first (as in a real frame), shuffle the rest lightly
    for (uint32_t i = 12; i < nk; i++) {
        uint32_t j = 12 + rnd() % (nk - 12);
        uint8_t t = kinds[i]; kinds[i] = kinds[j]; kinds[j] = t;
    }
    for (uint32_t p = 0; p < nk; p++) {
        Pass *ps = &g_passes[np++];
        ps->kind = kinds[p];
        uint32_t n;
        switch (ps->kind) {
        case PK_SHADOW: ps->target = p % NUM_SHADOW_RT; n = rnd_range(30, 70); break;
        case PK_GBUF: ps->target = 0; n = rnd_range(20, 80); break;
        case PK_FULL: ps->target = rnd() % NUM_FULL_RT; n = rnd_range(1, 2); break;
        default: {
            ps->target = rnd() % NUM_SMALL_RT;
            uint32_t r = rnd() % 100;
            n = r < 62 ? rnd_range(1, 2) : r < 95 ? rnd_range(3, 8) : rnd_range(20, 30);
        }
        }
        ps->clear = (rnd() % 3) == 0 || ps->kind == PK_SHADOW || p == 4;
        ps->barrier_before = (p > 0) && (rnd() % 2 == 0);
        ps->copies_before = (rnd() % 6 == 0) ? (uint8_t)rnd_range(1, 2) : 0;
        ps->dispatches_before = 0;
        ps->first_draw = nd;
        ps->draw_count = n;
        uint32_t w = 32, h = 32;
        if (ps->kind == PK_SMALL64) w = h = 64;
        if (ps->kind == PK_SHADOW) w = h = 4096;
        if (ps->kind == PK_GBUF || ps->kind == PK_FULL) { w = 1280; h = 720; }
        uint32_t pipe = kPipeFirst[ps->kind] + rnd() % kPipeCount[ps->kind];
        for (uint32_t d = 0; d < n; d++) {
            Draw *dr = &g_draws[nd++];
            if (rnd() % 100 < 45) pipe = kPipeFirst[ps->kind] + rnd() % kPipeCount[ps->kind];
            dr->pipe = (uint16_t)pipe;
            dr->tex = (uint16_t)(rnd() % NUM_TEX);
            dr->index_count = (uint16_t)(rnd_range(12, 100) * 3); // 36..300
            dr->first_index = (uint16_t)(rnd() % (MESH_INDICES - 300));
            dr->vertex_offset = (int16_t)(rnd() % 8);
            dr->ubo_slot = (uint16_t)(rnd() % UBO_SLOTS);
            uint32_t vw = w < 16 ? w : 16, vh = h < 16 ? h : 16; // tiny viewports: CPU-bound
            dr->vp_w = (uint16_t)vw; dr->vp_h = (uint16_t)vh;
            dr->vp_x = (uint16_t)(rnd() % (w - vw + 1));
            dr->vp_y = (uint16_t)(rnd() % (h - vh + 1));
        }
    }
    // ~95 compute dispatches, spread in bursts of 1-4 before passes
    uint32_t left = 95;
    while (left) {
        uint32_t p = 1 + rnd() % (np - 1);
        uint32_t k = rnd_range(1, 4);
        if (k > left) k = left;
        g_passes[p].dispatches_before += (uint8_t)k;
        left -= k;
    }
    // 60 submits, evenly spread over the passes; the last pass always ends a submit.
    enum { SUBMITS = 60 };
    for (uint32_t p = 0; p < np; p++)
        g_passes[p].submit_after = ((p + 1) * SUBMITS) / np != (p * SUBMITS) / np || p == np - 1;
    g_pass_count = np;
    g_draw_count = nd;
}

// ---------------------------------------------------------------- Vulkan objects
static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkPhysicalDeviceMemoryProperties memprops;
static PFN_vkCmdPushDescriptorSetKHR pfnPushDesc;

typedef struct { VkImage img; VkImageView view; VkDeviceMemory mem; VkFormat fmt; uint32_t w, h; } Image;
typedef struct { VkBuffer buf; VkDeviceMemory mem; void *map; VkDeviceSize size; } Buffer;

static Image gbuf_color[5], gbuf_depth, shadow_rt[NUM_SHADOW_RT], small32[NUM_SMALL_RT / 2],
    small64[NUM_SMALL_RT / 2], full_rt[NUM_FULL_RT], textures[NUM_TEX];
static Buffer vbuf, ibuf, ubuf, sbuf, staging;
static VkSampler sampler;
static VkDescriptorSetLayout dsl_gfx, dsl_gfx_ssbo, dsl_comp;
static VkPipelineLayout pl_gfx, pl_gfx_ssbo, pl_comp;
static VkPipeline pipes[NUM_PIPES], comp_pipe;
static int pipe_ssbo[NUM_PIPES];
static VkCommandPool cmd_pool;
static VkCommandBuffer *cmdbufs;
static uint32_t cmdbuf_count;
static VkFence fence;

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    fprintf(stderr, "no memory type\n");
    exit(1);
}

static Buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer b = {.size = size};
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    CHECK(vkCreateBuffer(dev, &ci, NULL, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b.buf, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CHECK(vkAllocateMemory(dev, &ai, NULL, &b.mem));
    CHECK(vkBindBufferMemory(dev, b.buf, b.mem, 0));
    CHECK(vkMapMemory(dev, b.mem, 0, size, 0, &b.map));
    return b;
}

static Image make_image(VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    Image im = {.fmt = fmt, .w = w, .h = h};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    CHECK(vkCreateImage(dev, &ci, NULL, &im.img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, im.img, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CHECK(vkAllocateMemory(dev, &ai, NULL, &im.mem));
    CHECK(vkBindImageMemory(dev, im.img, im.mem, 0));
    int depth = fmt == VK_FORMAT_D32_SFLOAT || fmt == VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im.img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = {depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (fmt == VK_FORMAT_D32_SFLOAT_S8_UINT)
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    CHECK(vkCreateImageView(dev, &vi, NULL, &im.view));
    return im;
}

static VkShaderModule make_module(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
    VkShaderModule m;
    CHECK(vkCreateShaderModule(dev, &ci, NULL, &m));
    return m;
}

typedef struct {
    int kind;      // pass kind (formats)
    int blend;     // 0 none, 1 alpha, 2 additive, 3 max, 4 premult, 5 multiply
    int depth;     // 0 off, 1 less+write, 2 equal no write, 3 greater+write
    VkCullModeFlags cull;
    int ssbo;
    int bias;
} PipeDesc;

static const PipeDesc kPipes[NUM_PIPES] = {
    // G-buffer: 5 colour + D32S8
    {PK_GBUF, 0, 1, VK_CULL_MODE_BACK_BIT, 0, 0}, {PK_GBUF, 0, 2, VK_CULL_MODE_BACK_BIT, 0, 0},
    {PK_GBUF, 1, 1, VK_CULL_MODE_NONE, 0, 0},     {PK_GBUF, 0, 1, VK_CULL_MODE_NONE, 0, 0},
    {PK_GBUF, 0, 1, VK_CULL_MODE_BACK_BIT, 1, 0}, {PK_GBUF, 2, 3, VK_CULL_MODE_FRONT_BIT, 0, 0},
    // shadow: depth only D32
    {PK_SHADOW, 0, 1, VK_CULL_MODE_BACK_BIT, 0, 0}, {PK_SHADOW, 0, 1, VK_CULL_MODE_FRONT_BIT, 0, 0},
    {PK_SHADOW, 0, 1, VK_CULL_MODE_NONE, 0, 1},
    // small R16G16F
    {PK_SMALL32, 0, 0, VK_CULL_MODE_NONE, 0, 0}, {PK_SMALL32, 2, 0, VK_CULL_MODE_NONE, 0, 0},
    {PK_SMALL32, 1, 0, VK_CULL_MODE_NONE, 0, 0}, {PK_SMALL32, 0, 0, VK_CULL_MODE_NONE, 1, 0},
    {PK_SMALL32, 3, 0, VK_CULL_MODE_NONE, 0, 0},
    // fullscreen RGBA8
    {PK_FULL, 0, 0, VK_CULL_MODE_NONE, 0, 0}, {PK_FULL, 1, 0, VK_CULL_MODE_NONE, 0, 0},
    {PK_FULL, 2, 0, VK_CULL_MODE_NONE, 0, 0}, {PK_FULL, 0, 0, VK_CULL_MODE_NONE, 1, 0},
    {PK_FULL, 4, 0, VK_CULL_MODE_NONE, 0, 0}, {PK_FULL, 5, 0, VK_CULL_MODE_NONE, 0, 0},
};

static const VkFormat kGbufFmts[5] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT};
static const VkFormat kSmallFmt = VK_FORMAT_R16G16_SFLOAT;
static const VkFormat kFullFmt = VK_FORMAT_R8G8B8A8_UNORM;

static void create_pipelines(void) {
    VkShaderModule vs = make_module(spv_draw_vert, sizeof(spv_draw_vert));
    VkShaderModule fs1 = make_module(spv_draw1_frag, sizeof(spv_draw1_frag));
    VkShaderModule fs1s = make_module(spv_draw1s_frag, sizeof(spv_draw1s_frag));
    VkShaderModule fs5 = make_module(spv_draw5_frag, sizeof(spv_draw5_frag));
    VkShaderModule fs5s = make_module(spv_draw5s_frag, sizeof(spv_draw5s_frag));
    VkShaderModule cs = make_module(spv_work_comp, sizeof(spv_work_comp));

    VkDescriptorSetLayoutBinding b[4] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 3, .pBindings = b};
    CHECK(vkCreateDescriptorSetLayout(dev, &dci, NULL, &dsl_gfx));
    dci.bindingCount = 4;
    CHECK(vkCreateDescriptorSetLayout(dev, &dci, NULL, &dsl_gfx_ssbo));
    VkDescriptorSetLayoutBinding cb = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    dci.bindingCount = 1; dci.pBindings = &cb;
    CHECK(vkCreateDescriptorSetLayout(dev, &dci, NULL, &dsl_comp));

    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
        .pSetLayouts = &dsl_gfx, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    CHECK(vkCreatePipelineLayout(dev, &lci, NULL, &pl_gfx));
    lci.pSetLayouts = &dsl_gfx_ssbo;
    CHECK(vkCreatePipelineLayout(dev, &lci, NULL, &pl_gfx_ssbo));
    VkPushConstantRange cpcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    lci.pSetLayouts = &dsl_comp; lci.pPushConstantRanges = &cpcr;
    CHECK(vkCreatePipelineLayout(dev, &lci, NULL, &pl_comp));

    VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = cs, .pName = "main"},
        .layout = pl_comp};
    CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &comp_pipe));

    VkVertexInputBindingDescription vib = {0, 12, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription via = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vis = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vib,
        .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &via};
    VkPipelineInputAssemblyStateCreateInfo ias = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dys = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn};

    for (int i = 0; i < NUM_PIPES; i++) {
        const PipeDesc *d = &kPipes[i];
        pipe_ssbo[i] = d->ssbo;
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = d->cull, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .depthBiasEnable = d->bias, .depthBiasConstantFactor = 2.0f, .depthBiasSlopeFactor = 1.5f,
            .lineWidth = 1.0f};
        VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        if (d->depth) {
            ds.depthTestEnable = VK_TRUE;
            ds.depthWriteEnable = d->depth != 2;
            ds.depthCompareOp = d->depth == 1 ? VK_COMPARE_OP_LESS_OR_EQUAL
                              : d->depth == 2 ? VK_COMPARE_OP_EQUAL : VK_COMPARE_OP_GREATER;
        }
        VkPipelineColorBlendAttachmentState att[5];
        for (int a = 0; a < 5; a++) {
            VkPipelineColorBlendAttachmentState s = {.colorWriteMask = 0xF};
            switch (d->blend) {
            case 1: s = (VkPipelineColorBlendAttachmentState){1, VK_BLEND_FACTOR_SRC_ALPHA,
                        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE,
                        VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0xF}; break;
            case 2: s = (VkPipelineColorBlendAttachmentState){1, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE,
                        VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, 0xF}; break;
            case 3: s = (VkPipelineColorBlendAttachmentState){1, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE,
                        VK_BLEND_OP_MAX, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_MAX, 0xF}; break;
            case 4: s = (VkPipelineColorBlendAttachmentState){1, VK_BLEND_FACTOR_ONE,
                        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE,
                        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, 0xF}; break;
            case 5: s = (VkPipelineColorBlendAttachmentState){1, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ZERO,
                        VK_BLEND_OP_ADD, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0xF}; break;
            }
            att[a] = s;
        }
        VkPipelineRenderingCreateInfo rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        VkPipelineShaderStageCreateInfo st[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .pName = "main"},
        };
        uint32_t nstages = 2, ncolor = 0;
        switch (d->kind) {
        case PK_GBUF:
            ncolor = 5; rci.pColorAttachmentFormats = kGbufFmts;
            rci.depthAttachmentFormat = rci.stencilAttachmentFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
            st[1].module = d->ssbo ? fs5s : fs5; break;
        case PK_SHADOW:
            nstages = 1; rci.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT; break;
        case PK_SMALL32:
            ncolor = 1; rci.pColorAttachmentFormats = &kSmallFmt; st[1].module = d->ssbo ? fs1s : fs1; break;
        case PK_FULL:
            ncolor = 1; rci.pColorAttachmentFormats = &kFullFmt; st[1].module = d->ssbo ? fs1s : fs1; break;
        }
        rci.colorAttachmentCount = ncolor;
        VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = ncolor, .pAttachments = att};
        VkGraphicsPipelineCreateInfo gci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rci,
            .stageCount = nstages, .pStages = st, .pVertexInputState = &vis, .pInputAssemblyState = &ias,
            .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
            .pDepthStencilState = &ds, .pColorBlendState = &cbs, .pDynamicState = &dys,
            .layout = d->ssbo ? pl_gfx_ssbo : pl_gfx};
        CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipes[i]));
    }
}

static void one_time_init_layouts(void) {
    // Move attachments to attachment-optimal and textures to shader-read (cleared).
    VkCommandBuffer cb = cmdbufs[0];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cb, &bi));
    VkImageMemoryBarrier2 bars[64];
    uint32_t n = 0;
#define BAR(im_, layout_, aspect_)                                                                  \
    bars[n++] = (VkImageMemoryBarrier2){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL,              \
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,               \
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, layout_, \
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (im_).img, {aspect_, 0, 1, 0, 1}}
    for (int i = 0; i < 5; i++) BAR(gbuf_color[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    BAR(gbuf_depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    for (int i = 0; i < NUM_SHADOW_RT; i++) BAR(shadow_rt[i], VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    for (int i = 0; i < NUM_SMALL_RT / 2; i++) {
        BAR(small32[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        BAR(small64[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    for (int i = 0; i < NUM_FULL_RT; i++) BAR(full_rt[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    for (int i = 0; i < NUM_TEX; i++) BAR(textures[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = n, .pImageMemoryBarriers = bars};
    vkCmdPipelineBarrier2(cb, &di);
    VkClearColorValue cc = {.float32 = {0.5f, 0.25f, 0.75f, 1.0f}};
    VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    n = 0;
    for (int i = 0; i < NUM_TEX; i++) {
        vkCmdClearColorImage(cb, textures[i].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rr);
        bars[n++] = (VkImageMemoryBarrier2){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
            textures[i].img, rr};
    }
    di.imageMemoryBarrierCount = n;
    vkCmdPipelineBarrier2(cb, &di);
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CHECK(vkQueueSubmit(queue, 1, &si, fence));
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    CHECK(vkResetFences(dev, 1, &fence));
    CHECK(vkResetCommandPool(dev, cmd_pool, 0));
}

static void init_vulkan(void) {
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
    VkExtensionProperties *exts = calloc(n, sizeof(*exts));
    vkEnumerateInstanceExtensionProperties(NULL, &n, exts);
    const char *iext[2];
    uint32_t niext = 0;
    VkInstanceCreateFlags iflags = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(exts[i].extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            iext[niext++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
            iflags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
    free(exts);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vkbench",
        .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .flags = iflags,
        .pApplicationInfo = &app, .enabledExtensionCount = niext, .ppEnabledExtensionNames = iext};
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    n = 1;
    VkResult er = vkEnumeratePhysicalDevices(inst, &n, &phys);
    if (er < 0 || n == 0) { fprintf(stderr, "no device\n"); exit(1); }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
    fprintf(stderr, "device: %s, API %u.%u.%u\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
            VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq, sizeof(*qp));
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qp);
    qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq && qfam == UINT32_MAX; i++)
        if ((qp[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) qfam = i;
    free(qp);

    vkEnumerateDeviceExtensionProperties(phys, NULL, &n, NULL);
    exts = calloc(n, sizeof(*exts));
    vkEnumerateDeviceExtensionProperties(phys, NULL, &n, exts);
    const char *dext[4];
    uint32_t ndext = 0;
    int have_push = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!strcmp(exts[i].extensionName, "VK_KHR_portability_subset")) dext[ndext++] = "VK_KHR_portability_subset";
        if (!strcmp(exts[i].extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) have_push = 1;
    }
    free(exts);
    if (!have_push) { fprintf(stderr, "VK_KHR_push_descriptor missing\n"); exit(1); }
    dext[ndext++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;

    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE};
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam,
        .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci, .enabledExtensionCount = ndext, .ppEnabledExtensionNames = dext};
    CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    pfnPushDesc = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(dev, "vkCmdPushDescriptorSetKHR");
    if (!pfnPushDesc) { fprintf(stderr, "no vkCmdPushDescriptorSetKHR\n"); exit(1); }

    // resources
    const VkImageUsageFlags rt = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkImageUsageFlags dt = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    for (int i = 0; i < 5; i++) gbuf_color[i] = make_image(kGbufFmts[i], 1280, 720, rt);
    gbuf_depth = make_image(VK_FORMAT_D32_SFLOAT_S8_UINT, 1280, 720, dt);
    for (int i = 0; i < NUM_SHADOW_RT; i++) shadow_rt[i] = make_image(VK_FORMAT_D32_SFLOAT, 4096, 4096, dt);
    for (int i = 0; i < NUM_SMALL_RT / 2; i++) {
        small32[i] = make_image(kSmallFmt, 32, 32, rt);
        small64[i] = make_image(kSmallFmt, 64, 64, rt);
    }
    for (int i = 0; i < NUM_FULL_RT; i++) full_rt[i] = make_image(kFullFmt, 1280, 720, rt);
    for (int i = 0; i < NUM_TEX; i++)
        textures[i] = make_image(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

    vbuf = make_buffer(MESH_VERTS * 17 * 12, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    float *v = vbuf.map;
    for (int i = 0; i < MESH_VERTS * 17; i++) {
        v[i * 3 + 0] = (float)((int)(rnd() % 200) - 100) / 100.0f;
        v[i * 3 + 1] = (float)((int)(rnd() % 200) - 100) / 100.0f;
        v[i * 3 + 2] = (float)(rnd() % 100) / 100.0f;
    }
    ibuf = make_buffer(MESH_INDICES * 2, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    uint16_t *ix = ibuf.map;
    for (int i = 0; i < MESH_INDICES; i++) ix[i] = (uint16_t)(rnd() % MESH_VERTS);
    ubuf = make_buffer(UBO_SLOTS * UBO_STRIDE, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    float *u = ubuf.map;
    for (int s = 0; s < UBO_SLOTS; s++) {
        float *m = u + s * (UBO_STRIDE / 4);
        memset(m, 0, 80);
        m[0] = m[5] = m[10] = m[15] = 1.0f;
        m[16] = m[17] = m[18] = m[19] = 1.0f;
    }
    sbuf = make_buffer(1024 * 16 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    memset(sbuf.map, 0, sbuf.size);
    staging = make_buffer(64 * 1024, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    memset(staging.map, 0, staging.size);

    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .maxLod = 1.0f};
    CHECK(vkCreateSampler(dev, &sci, NULL, &sampler));

    create_pipelines();

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfam,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT};
    CHECK(vkCreateCommandPool(dev, &pci, NULL, &cmd_pool));
    cmdbuf_count = 0;
    for (uint32_t p = 0; p < g_pass_count; p++) cmdbuf_count += g_passes[p].submit_after;
    cmdbufs = calloc(cmdbuf_count, sizeof(*cmdbufs));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = cmdbuf_count};
    CHECK(vkAllocateCommandBuffers(dev, &cai, cmdbufs));
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(dev, &fci, NULL, &fence));
    one_time_init_layouts();
}

// ---------------------------------------------------------------- frame recording
enum { C_BEGIN_RENDERING, C_END_RENDERING, C_BIND_PIPE, C_PUSH_DESC, C_PUSH_CONST, C_VIEWPORT, C_SCISSOR,
       C_BIND_VB, C_BIND_IB, C_DRAW, C_DISPATCH, C_BARRIER, C_COPY, C_SUBMIT, C_COUNT };
static const char *kCmdNames[C_COUNT] = {"beginRendering", "endRendering", "bindPipeline", "pushDescriptorSet",
    "pushConstants", "setViewport", "setScissor", "bindVertexBuffers", "bindIndexBuffer", "drawIndexed",
    "dispatch", "pipelineBarrier2", "copyBuffer", "queueSubmit"};
static uint64_t g_counts[C_COUNT];
static int g_count_enabled;
#define COUNT(c) do { if (g_count_enabled) g_counts[c]++; } while (0)

static Image *pass_target(const Pass *p) {
    switch (p->kind) {
    case PK_SHADOW: return &shadow_rt[p->target];
    case PK_SMALL32: return &small32[p->target % (NUM_SMALL_RT / 2)];
    case PK_SMALL64: return &small64[p->target % (NUM_SMALL_RT / 2)];
    case PK_FULL: return &full_rt[p->target];
    default: return &gbuf_color[0];
    }
}

static uint32_t g_frame;

static void record_pass_prologue(VkCommandBuffer cb, const Pass *p, uint32_t pi) {
    for (uint32_t c = 0; c < p->copies_before; c++) {
        VkBufferCopy rg = {(pi * 256 + c * 4096) % (staging.size - 4096), (pi * 512 + c * 1024) % (sbuf.size - 4096), 4096};
        vkCmdCopyBuffer(cb, staging.buf, sbuf.buf, 1, &rg);
        COUNT(C_COPY);
    }
    if (p->dispatches_before) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, comp_pipe);
        COUNT(C_BIND_PIPE);
        for (uint32_t d = 0; d < p->dispatches_before; d++) {
            VkDescriptorBufferInfo bi = {sbuf.buf, 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi};
            pfnPushDesc(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl_comp, 0, 1, &w);
            COUNT(C_PUSH_DESC);
            uint32_t pc[4] = {pi * 64 + d, g_frame & 7, 0, 0};
            vkCmdPushConstants(cb, pl_comp, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pc);
            COUNT(C_PUSH_CONST);
            vkCmdDispatch(cb, 1, 1, 1);
            COUNT(C_DISPATCH);
        }
    }
    if (p->barrier_before || p->copies_before || p->dispatches_before) {
        VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT};
        VkImageMemoryBarrier2 ib;
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        if (p->barrier_before && p->kind != PK_SHADOW) {
            // render-target -> sampled style dependency on this pass's colour target (layout kept)
            Image *t = pass_target(p);
            ib = (VkImageMemoryBarrier2){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, t->img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            di.imageMemoryBarrierCount = 1;
            di.pImageMemoryBarriers = &ib;
        }
        vkCmdPipelineBarrier2(cb, &di);
        COUNT(C_BARRIER);
    }
}

static void record_pass(VkCommandBuffer cb, const Pass *p) {
    VkRenderingAttachmentInfo color[5], depth;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .layerCount = 1};
    VkAttachmentLoadOp load = p->clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    int has_depth = 0, has_stencil = 0;
    Image *t = pass_target(p);
    ri.renderArea.extent = (VkExtent2D){t->w, t->h};
    uint32_t ncolor = 0;
    if (p->kind == PK_GBUF) {
        for (int i = 0; i < 5; i++) color[ncolor++] = (VkRenderingAttachmentInfo){
            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, gbuf_color[i].view,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_NULL_HANDLE, 0, load, VK_ATTACHMENT_STORE_OP_STORE, {{{0}}}};
        depth = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, gbuf_depth.view,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, VK_NULL_HANDLE, 0, load, VK_ATTACHMENT_STORE_OP_STORE,
            {.depthStencil = {1.0f, 0}}};
        has_depth = has_stencil = 1;
    } else if (p->kind == PK_SHADOW) {
        depth = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, t->view,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, 0, VK_NULL_HANDLE, 0, load, VK_ATTACHMENT_STORE_OP_STORE,
            {.depthStencil = {1.0f, 0}}};
        has_depth = 1;
    } else {
        color[ncolor++] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, t->view,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_NULL_HANDLE, 0, load, VK_ATTACHMENT_STORE_OP_STORE, {{{0}}}};
    }
    ri.colorAttachmentCount = ncolor;
    ri.pColorAttachments = color;
    ri.pDepthAttachment = has_depth ? &depth : NULL;
    ri.pStencilAttachment = has_stencil ? &depth : NULL;
    vkCmdBeginRendering(cb, &ri);
    COUNT(C_BEGIN_RENDERING);

    int cur_pipe = -1;
    for (uint32_t i = 0; i < p->draw_count; i++) {
        const Draw *d = &g_draws[p->first_draw + i];
        if (d->pipe != cur_pipe) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes[d->pipe]);
            COUNT(C_BIND_PIPE);
            cur_pipe = d->pipe;
        }
        int ssbo = pipe_ssbo[d->pipe];
        VkPipelineLayout layout = ssbo ? pl_gfx_ssbo : pl_gfx;
        VkDescriptorBufferInfo ub = {ubuf.buf, (VkDeviceSize)d->ubo_slot * UBO_STRIDE, 80};
        VkDescriptorImageInfo ti = {VK_NULL_HANDLE, textures[d->tex].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo si = {sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        VkDescriptorBufferInfo sb = {sbuf.buf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[4] = {
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &ub},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ti},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &si},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &sb},
        };
        pfnPushDesc(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, ssbo ? 4 : 3, w);
        COUNT(C_PUSH_DESC);
        float pc[16] = {(float)i, 0, 0, 0, 0.01f * (float)(d->tex), 0, 0, 0, 0.1f, 0.1f, 0.1f, 0, (float)(d->tex), 0, 0, 0};
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, pc);
        COUNT(C_PUSH_CONST);
        VkViewport vp = {d->vp_x, d->vp_y, d->vp_w, d->vp_h, 0.0f, 1.0f};
        vkCmdSetViewport(cb, 0, 1, &vp);
        COUNT(C_VIEWPORT);
        VkRect2D sc = {{d->vp_x, d->vp_y}, {d->vp_w, d->vp_h}};
        vkCmdSetScissor(cb, 0, 1, &sc);
        COUNT(C_SCISSOR);
        VkDeviceSize voff = (VkDeviceSize)(d->tex % 16) * MESH_VERTS * 12;
        vkCmdBindVertexBuffers(cb, 0, 1, &vbuf.buf, &voff);
        COUNT(C_BIND_VB);
        vkCmdBindIndexBuffer(cb, ibuf.buf, 0, VK_INDEX_TYPE_UINT16);
        COUNT(C_BIND_IB);
        vkCmdDrawIndexed(cb, d->index_count, 1, d->first_index, d->vertex_offset, 0);
        COUNT(C_DRAW);
    }
    vkCmdEndRendering(cb);
    COUNT(C_END_RENDERING);
}

typedef struct {
    uint64_t rec_cpu, rec_wall, sub_cpu, sub_wall, frame_cpu, frame_wall, wait_wall, proc_cpu;
} FrameTimes;

static void run_frame(FrameTimes *ft) {
    uint64_t f_w0 = wall_ns(), f_c0 = tcpu_ns(), p0 = pcpu_ns();
    uint64_t rc = 0, rw = 0, sc = 0, sw = 0;
    uint64_t c0 = tcpu_ns(), w0 = wall_ns();
    CHECK(vkResetCommandPool(dev, cmd_pool, 0));
    uint32_t cbi = 0;
    VkCommandBuffer cb = cmdbufs[0];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(cb, &bi));
    for (uint32_t pi = 0; pi < g_pass_count; pi++) {
        const Pass *p = &g_passes[pi];
        record_pass_prologue(cb, p, pi);
        record_pass(cb, p);
        if (p->submit_after) {
            CHECK(vkEndCommandBuffer(cb));
            uint64_t c1 = tcpu_ns(), w1 = wall_ns();
            rc += c1 - c0; rw += w1 - w0;
            VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
            int last = pi == g_pass_count - 1;
            CHECK(vkQueueSubmit(queue, 1, &si, last ? fence : VK_NULL_HANDLE));
            COUNT(C_SUBMIT);
            c0 = tcpu_ns(); w0 = wall_ns();
            sc += c0 - c1; sw += w0 - w1;
            if (!last) {
                cb = cmdbufs[++cbi];
                CHECK(vkBeginCommandBuffer(cb, &bi));
            }
        }
    }
    uint64_t f_c1 = tcpu_ns(), f_w1 = wall_ns();
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    uint64_t f_w2 = wall_ns();
    CHECK(vkResetFences(dev, 1, &fence));
    uint64_t p1 = pcpu_ns();
    ft->rec_cpu = rc; ft->rec_wall = rw; ft->sub_cpu = sc; ft->sub_wall = sw;
    ft->frame_cpu = f_c1 - f_c0; ft->frame_wall = f_w1 - f_w0; ft->wait_wall = f_w2 - f_w1;
    ft->proc_cpu = p1 - p0;
    g_frame++;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static void stats(uint64_t *v, int n, double *med, double *p95) {
    qsort(v, n, sizeof(uint64_t), cmp_u64);
    *med = v[n / 2] / 1e6;
    *p95 = v[(int)(n * 0.95)] / 1e6;
}

int main(int argc, char **argv) {
    int warmup = 60, frames = 300;
    if (argc > 1) frames = atoi(argv[1]);
    if (argc > 2) warmup = atoi(argv[2]);
    mach_timebase_info(&g_tb);
    g_thread = mach_thread_self();
#if defined(__x86_64__)
    const char *arch = "x86_64";
#elif defined(__aarch64__)
    const char *arch = "arm64";
#else
    const char *arch = "unknown";
#endif
    const char *prefill = getenv("MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS");
    build_workload();
    init_vulkan();

    // checksum: FNV-1a over the workload description + per-type command counts of one frame
    g_count_enabled = 1;
    FrameTimes ft;
    run_frame(&ft);
    g_count_enabled = 0;
    uint64_t h = 1469598103934665603ull;
#define FNV(x) do { uint64_t x_ = (uint64_t)(x); for (int b_ = 0; b_ < 8; b_++) { h ^= (x_ >> (b_ * 8)) & 0xFF; h *= 1099511628211ull; } } while (0)
    for (uint32_t i = 0; i < g_pass_count; i++) {
        const Pass *p = &g_passes[i];
        FNV(p->kind); FNV(p->target); FNV(p->clear); FNV(p->barrier_before); FNV(p->copies_before);
        FNV(p->dispatches_before); FNV(p->submit_after); FNV(p->draw_count);
    }
    for (uint32_t i = 0; i < g_draw_count; i++) {
        const Draw *d = &g_draws[i];
        FNV(d->pipe); FNV(d->tex); FNV(d->index_count); FNV(d->first_index); FNV(d->ubo_slot); FNV(d->vp_x);
    }
    for (int c = 0; c < C_COUNT; c++) FNV(g_counts[c]);
    fprintf(stderr, "arch=%s prefill=%s passes=%u draws=%u pipelines=%d+1 cmdbufs=%u\n", arch,
            prefill ? prefill : "default", g_pass_count, g_draw_count, NUM_PIPES, cmdbuf_count);
    fprintf(stderr, "per-frame commands:");
    for (int c = 0; c < C_COUNT; c++) fprintf(stderr, " %s=%llu", kCmdNames[c], (unsigned long long)g_counts[c]);
    fprintf(stderr, "\nchecksum=%016llx\n", (unsigned long long)h);

    // cost of the timer calls themselves
    uint64_t t0 = wall_ns();
    volatile uint64_t sink = 0;
    for (int i = 0; i < 10000; i++) sink += tcpu_ns();
    double tcpu_cost_ns = (double)(wall_ns() - t0) / 10000.0;
    (void)sink;

    for (int i = 0; i < warmup; i++) run_frame(&ft);
    uint64_t *a[8];
    for (int k = 0; k < 8; k++) a[k] = calloc(frames, sizeof(uint64_t));
    for (int i = 0; i < frames; i++) {
        run_frame(&ft);
        a[0][i] = ft.rec_cpu; a[1][i] = ft.rec_wall; a[2][i] = ft.sub_cpu; a[3][i] = ft.sub_wall;
        a[4][i] = ft.frame_cpu; a[5][i] = ft.frame_wall; a[6][i] = ft.wait_wall; a[7][i] = ft.proc_cpu;
    }
    static const char *names[8] = {"record_cpu", "record_wall", "submit_cpu", "submit_wall", "frame_cpu",
                                   "frame_wall", "fence_wait", "process_cpu"};
    printf("RESULT arch=%s prefill=%s frames=%d checksum=%016llx timer_ns=%.0f", arch, prefill ? prefill : "default",
           frames, (unsigned long long)h, tcpu_cost_ns);
    for (int k = 0; k < 8; k++) {
        double med, p95;
        stats(a[k], frames, &med, &p95);
        printf(" %s=%.3f/%.3f", names[k], med, p95);
    }
    printf("\n");
    vkDeviceWaitIdle(dev);
    return 0;
}
