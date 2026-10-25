// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// The keyboard and mouse bindings (src/runtime_pad.c), shown in the launcher's Controls section.
enum Controls {
    /// (action, keys) in the order players look for them.
    static func rows(mouseLook: Bool) -> [(String, String)] {
        var rows: [(String, String)] = [
            ("Move", "W A S D"),
            ("Camera", mouseLook ? "Mouse (click in the window)" : "Arrow keys"),
            ("Confirm / interact (Cross)", "Space or Return"),
            ("Dodge, sprint, back (Circle)", "Left Shift or Esc"),
            ("Use item (Square)", "E"),
            ("Transform weapon (L1)", "1"),
            ("Attack (R1)", mouseLook ? "Left click or 3" : "3"),
            ("Firearm (L2)", mouseLook ? "Right click or R" : "R"),
            ("Charged attack (R2)", mouseLook ? "Back side button or F" : "F"),
            ("Interact / Triangle", "Q"),
            ("Lock on (R3)", mouseLook ? "Middle click or C" : "C"),
            ("L3", "Z"),
            ("D-pad (menus, weapons, items)", mouseLook ? "Arrow keys or I J K L" : "I J K L"),
            ("Menu (Options)", "O or F1"),
            ("Gestures / touchpad left, right", "Tab, Backspace"),
            ("Settings overlay", "\u{2318},"),
        ]
        if mouseLook { rows.insert(("Transform (L1), alternative", "Forward side button"), at: 5) }
        return rows
    }
}
