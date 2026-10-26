// SPDX-FileCopyrightText: Copyright 2026 Ronny Stiftel
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import Metal
import MetalFX

@MainActor
enum FrameGenerationSupport {
    static var deviceSupported: Bool {
        // Xcode 26 introduced this SDK API. Older toolchains still build a launcher with the
        // option unavailable, just as the renderer does when built with an older SDK.
        #if compiler(>=6.2)
        if #available(macOS 26.0, *), let device = MTLCreateSystemDefaultDevice() {
            return MTLFXFrameInterpolatorDescriptor.supportsDevice(device)
        }
        #endif
        return false
    }

    static var displayRefreshRates: [Double] {
        NSScreen.screens.map { screen in
            if let number = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber,
               let mode = CGDisplayCopyDisplayMode(number.uint32Value),
               mode.refreshRate.isFinite, mode.refreshRate > 0 {
                // SDL's current display mode reports an integer refresh rate too.
                return mode.refreshRate.rounded()
            }
            // Variable-refresh displays can report zero through CoreGraphics.
            return Double(screen.maximumFramesPerSecond)
        }
    }
}
