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

    static func launchDisplay(preferred: String? = nil) -> FrameGenerationDisplay? {
        // Match the preferred-display picker and BB_DISPLAY override. A disconnected saved
        // display falls back to the primary one, as the SDL window does.
        let selected = preferred.flatMap { preferred in
            NSScreen.screens.first { screen in
                guard let number = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber,
                      let uuid = CGDisplayCreateUUIDFromDisplayID(number.uint32Value)?.takeRetainedValue(),
                      let text = CFUUIDCreateString(nil, uuid) as String? else { return false }
                return text.caseInsensitiveCompare(preferred) == .orderedSame
            }
        }
        guard let screen = selected ?? NSScreen.screens.first,
              let number = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber else { return nil }
        let shortest = screen.minimumRefreshInterval
        let longest = screen.maximumRefreshInterval
        let validInterval = shortest.isFinite && shortest > 0
        let variable = validInterval && longest.isFinite && longest > shortest
        let modeRate = CGDisplayCopyDisplayMode(number.uint32Value)?.refreshRate ?? 0
        let rate = variable ? 1 / shortest
            : (modeRate.isFinite && modeRate > 0 ? modeRate : (validInterval ? 1 / shortest : 0))
        return FrameGenerationDisplay(refreshRate: rate, minimumRefreshRate: variable ? 1 / longest : 0,
                                      requiresFullscreen: CGDisplayIsBuiltin(number.uint32Value) == 0)
    }
}
