// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: MTLDepthStencilState cache (Metal-native backend).

#pragma once

#include <memory>
#include "common/types.h"

namespace Metal {

class Device;

struct StencilFace {
    u8 fail{}, pass{}, depth_fail{}; ///< VkStencilOp values
    u8 compare{7};                   ///< VkCompareOp (7 = Always)
    u8 read_mask{0xff}, write_mask{0xff};
    bool operator==(const StencilFace&) const = default;
};

struct DepthStencilKey {
    bool depth_test{}, depth_write{}, stencil_test{};
    u8 depth_compare{7}; ///< VkCompareOp
    StencilFace front{}, back{};
    bool operator==(const DepthStencilKey&) const = default;
};

/// MTLDepthStencilState per key, made once (thread-safe; states live as long as the cache).
class DepthStencilCache {
public:
    explicit DepthStencilCache(Device& device);
    ~DepthStencilCache();
    void* Get(const DepthStencilKey& key); ///< id<MTLDepthStencilState>

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Metal
