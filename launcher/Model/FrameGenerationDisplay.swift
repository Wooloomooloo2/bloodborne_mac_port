// SPDX-FileCopyrightText: Copyright 2026 Ronny Stiftel
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct FrameGenerationDisplay: Equatable {
    var refreshRate: Double
    /// Zero for fixed/unknown modes. A positive value describes the active variable range.
    var minimumRefreshRate: Double = 0
    var requiresFullscreen = false

    func supports(realCap: Int, fullscreen: Bool) -> Bool {
        guard realCap > 0, refreshRate.isFinite, refreshRate > 0 else { return false }
        let pairedRate = Double(realCap) * 2
        if minimumRefreshRate != 0, !requiresFullscreen || fullscreen {
            return minimumRefreshRate.isFinite && minimumRefreshRate > 0
                && minimumRefreshRate < refreshRate && pairedRate >= minimumRefreshRate * 0.998
                && pairedRate <= refreshRate * 1.002
        }
        // Absolute pair deadlines do not round a minimum hold up on every image.
        // Same 0.2% tolerance as BbFrameGen::DisplaySupported, including 119.88 Hz.
        return pairedRate <= refreshRate * 1.002
    }
}
