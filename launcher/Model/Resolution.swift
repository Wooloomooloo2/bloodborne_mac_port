// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// An output or render size in pixels.
struct PixelSize: Hashable, CustomStringConvertible {
    var width: Int
    var height: Int

    init(_ width: Int, _ height: Int) {
        self.width = width
        self.height = height
    }

    /// "2560x1440", as bbport.ini's output_res.
    init?(iniValue: String) {
        let parts = iniValue.lowercased().split(separator: "x")
        guard parts.count == 2, let w = Int(parts[0]), let h = Int(parts[1]), w > 0, h > 0 else {
            return nil
        }
        self.init(w, h)
    }

    var iniValue: String { "\(width)x\(height)" }
    var description: String { "\(width) × \(height)" }
}

enum Resolution {
    /// The in-game menu's output list (gpu/shim/bbport_settings.h OutputWidths / OutputHeights).
    static let outputs = [PixelSize(1280, 720), PixelSize(1920, 1080), PixelSize(2560, 1440),
                          PixelSize(3456, 1944), PixelSize(3840, 2160)]
    static let defaultOutput = PixelSize(1920, 1080)
    /// Native aspect-ratio patches (scripts/patches.py ULTRAWIDE_PATCHES): ultrawide and 16:10
    /// (the MacBook displays' shape, #75).
    static let ultrawideOutputs = [PixelSize(2560, 720), PixelSize(3840, 1080),
                                  PixelSize(5120, 1440), PixelSize(2560, 1080), PixelSize(3440, 1440),
                                  PixelSize(1280, 800), PixelSize(1920, 1200), PixelSize(2560, 1600)]
    static func wideLabel(_ size: PixelSize) -> String {
        let aspect = switch size.width * 10 / size.height {
        case 16: "16:10"
        case 23, 24: "21:9"
        default: "32:9"
        }
        return "\(size) · \(aspect)"
    }
    /// Native 1440p ultrawide and 2560x1600 add large targets/textures to the 7 GiB GPU arena.
    static func ultrawideMemoryWarning(_ size: PixelSize, memoryBytes: UInt64) -> String? {
        guard memoryBytes < 24 * 1024 * 1024 * 1024,
              [PixelSize(3440, 1440), PixelSize(5120, 1440), PixelSize(2560, 1600)].contains(size) else { return nil }
        return "This resolution may exceed available graphics memory on Macs with less than 24 GB of memory, causing slowdowns or crashes. Try a smaller resolution of the same shape if this happens."
    }
    /// Upscaler quality levels (bbport_settings.cpp PresetName / PresetScale).
    static let presetNames = ["Native AA", "Quality", "Balanced", "Performance", "Ultra Performance"]
    static let presetScales = [1.0, 1.5, 1.7, 2.0, 3.0]

    /// The scene size the game renders for an output and quality level: even sizes, rounded as
    /// scripts/patches.py does (its round() ties to even; no output in the list hits a tie).
    static func renderSize(output: PixelSize, preset: Int) -> PixelSize {
        let scale = presetScales[min(max(preset, 0), presetScales.count - 1)]
        func side(_ v: Int) -> Int { max(2, Int((Double(v) / scale / 2).rounded()) * 2) }
        return PixelSize(side(output.width), side(output.height))
    }
}
