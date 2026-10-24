// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: MTLDepthStencilState cache (Metal-native backend).

#import <Metal/Metal.h>

#include "video_core/renderer_metal/mtl_depth_stencil.h"

#include <cstring>
#include <mutex>
#include <unordered_map>
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_object.h"
#include "video_core/renderer_metal/vk_to_mtl.h"

namespace Metal {

namespace {

struct KeyHash {
    size_t operator()(const DepthStencilKey& key) const noexcept {
        // FNV-1a over the fields (the struct has padding, so not over its bytes).
        u64 h = 1469598103934665603ull;
        const auto mix = [&h](u64 v) { h = (h ^ v) * 1099511628211ull; };
        mix(key.depth_test | key.depth_write << 1 | key.stencil_test << 2);
        mix(key.depth_compare);
        for (const StencilFace* face : {&key.front, &key.back}) {
            mix(face->fail | face->pass << 8 | face->depth_fail << 16 | u64(face->compare) << 24 |
                u64(face->read_mask) << 32 | u64(face->write_mask) << 40);
        }
        return h;
    }
};

MTLStencilDescriptor* MakeStencil(const StencilFace& face) {
    MTLStencilDescriptor* d = [[MTLStencilDescriptor alloc] init];
    d.stencilCompareFunction = static_cast<MTLCompareFunction>(
        VkToMtl::CompareFunction(static_cast<vk::CompareOp>(face.compare)));
    d.stencilFailureOperation = static_cast<MTLStencilOperation>(
        VkToMtl::StencilOp(static_cast<vk::StencilOp>(face.fail)));
    d.depthFailureOperation = static_cast<MTLStencilOperation>(
        VkToMtl::StencilOp(static_cast<vk::StencilOp>(face.depth_fail)));
    d.depthStencilPassOperation = static_cast<MTLStencilOperation>(
        VkToMtl::StencilOp(static_cast<vk::StencilOp>(face.pass)));
    d.readMask = face.read_mask;
    d.writeMask = face.write_mask;
    return d;
}

} // namespace

struct DepthStencilCache::Impl {
    id<MTLDevice> device;
    std::mutex mutex;
    std::unordered_map<DepthStencilKey, Object, KeyHash> states;
};

DepthStencilCache::DepthStencilCache(Device& device) : impl{std::make_unique<Impl>()} {
    impl->device = (__bridge id<MTLDevice>)device.NativeDevice();
}

DepthStencilCache::~DepthStencilCache() = default;

void* DepthStencilCache::Get(const DepthStencilKey& key) {
    std::scoped_lock lock{impl->mutex};
    auto it = impl->states.find(key);
    if (it == impl->states.end()) {
        MTLDepthStencilDescriptor* d = [[MTLDepthStencilDescriptor alloc] init];
        d.depthCompareFunction =
            key.depth_test ? static_cast<MTLCompareFunction>(VkToMtl::CompareFunction(
                                 static_cast<vk::CompareOp>(key.depth_compare)))
                           : MTLCompareFunctionAlways;
        d.depthWriteEnabled = key.depth_test && key.depth_write;
        if (key.stencil_test) {
            d.frontFaceStencil = MakeStencil(key.front);
            d.backFaceStencil = MakeStencil(key.back);
        }
        id<MTLDepthStencilState> state = [impl->device newDepthStencilStateWithDescriptor:d];
        it = impl->states
                 .emplace(key, Object::Adopt((__bridge_retained void*)state))
                 .first;
    }
    return it->second.Get();
}

} // namespace Metal
