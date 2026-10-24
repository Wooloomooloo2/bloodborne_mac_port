// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifndef __OBJC__
#error "mtl_device_impl.h is for Objective-C++ (.mm) files only"
#endif

#import <Metal/Metal.h>

#include <array>
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_order.h"

namespace Metal {

struct Device::Impl {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLResidencySet> residency API_AVAILABLE(macos(15.0)); ///< arena and address-reached buffers
    id<MTLSharedEvent> event; ///< signalled with each command buffer's tick (for GPU-side waits)
    /// The ordering fences of untracked buffers (OrderTracker indices), primed once; empty with
    /// hazard tracking (BB_METAL_HAZARD_TRACKING=1).
    std::array<id<MTLFence>, OrderTracker::NumFences> fences;
};

} // namespace Metal
