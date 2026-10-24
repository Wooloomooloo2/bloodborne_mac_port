// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_gpu_capture.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace BbGpuCapture {

void OnPresent(void* device) {
    static const double delay = [] {
        const char* env = std::getenv("BB_GPU_CAPTURE");
        return env ? std::atof(env) : -1.0;
    }();
    if (delay < 0) {
        return;
    }
    static const auto first = std::chrono::steady_clock::now();
    static enum { Waiting, Capturing, Done } state = Waiting;
    if (state == Done) {
        return;
    }
    @autoreleasepool {
        MTLCaptureManager* manager = [MTLCaptureManager sharedCaptureManager];
        if (state == Capturing) {
            [manager stopCapture];
            state = Done;
            std::printf("GPU capture: saved (open the .gputrace in Xcode)\n");
            return;
        }
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - first).count() < delay) {
            return;
        }
        state = Done; // one attempt
        if (![manager supportsDestination:MTLCaptureDestinationGPUTraceDocument]) {
            std::printf("GPU capture: unavailable (launch with METAL_CAPTURE_ENABLED=1)\n");
            return;
        }
        const char* file = std::getenv("BB_GPU_CAPTURE_FILE");
        NSString* path = [NSString stringWithUTF8String:file ? file : "frame.gputrace"];
        [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
        MTLCaptureDescriptor* descriptor = [[MTLCaptureDescriptor alloc] init];
        descriptor.captureObject = (__bridge id<MTLDevice>)device;
        descriptor.destination = MTLCaptureDestinationGPUTraceDocument;
        descriptor.outputURL = [NSURL fileURLWithPath:path];
        NSError* error = nil;
        if (![manager startCaptureWithDescriptor:descriptor error:&error]) {
            std::printf("GPU capture: failed to start: %s\n",
                        error ? error.localizedDescription.UTF8String : "unknown error");
            return;
        }
        state = Capturing;
        std::printf("GPU capture: capturing one frame to %s\n", path.UTF8String);
    }
}

} // namespace BbGpuCapture
