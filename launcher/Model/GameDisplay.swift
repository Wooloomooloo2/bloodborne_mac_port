// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// A display the game can open on: bbport.ini `display=` holds its CoreGraphics UUID, which
/// run.sh passes as BB_DISPLAY (scripts/launch_settings.sh); the GPU host centres the window on
/// it, and full screen opens there (gpu/shim/window.cpp). Absent = the main display.
struct GameDisplay: Hashable, Identifiable {
    let uuid: String
    let name: String
    var id: String { uuid }

    /// The form launch_settings.sh accepts: 36 characters of hex digits and dashes.
    static func isUUID(_ text: String) -> Bool {
        text.count == 36 && text.allSatisfy { $0 == "-" || $0.isHexDigit }
    }

    /// The picker's entries: the connected displays, plus the saved one when it isn't connected
    /// (so the choice stays visible; the game then uses the main display).
    static func choices(connected: [GameDisplay], saved: String?) -> [GameDisplay] {
        guard let saved, !connected.contains(where: { $0.uuid == saved }) else { return connected }
        return connected + [GameDisplay(uuid: saved, name: "Display not connected")]
    }
}
