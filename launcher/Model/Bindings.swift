// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// One thing a key or mouse button can do: a pad input of the game (src/runtime_bindings.c, same
/// ini keys and defaults).
struct BindAction: Identifiable, Hashable {
    let key: String
    let label: String
    let group: String
    let defaults: [String]
    var id: String { key }
}

/// The keyboard and mouse bindings: bbport.ini bind_<action>=Name,Name (SDL scancode names, the
/// key's position, or a mouse name). An absent key is the default, an empty one unbinds.
struct Bindings: Equatable {
    static let slots = 4
    static let mouseNames = ["Mouse Left", "Mouse Right", "Mouse Middle", "Mouse Back", "Mouse Forward",
                             "Wheel Up", "Wheel Down"]
    /// Cmd is the Mac's (shortcuts, and it gives the cursor back); the game ignores these.
    static let reserved = ["Left GUI", "Right GUI"]

    static let actions: [BindAction] = [
        BindAction(key: "bind_move_forward", label: "Forward", group: "Movement", defaults: ["W"]),
        BindAction(key: "bind_move_back", label: "Back", group: "Movement", defaults: ["S"]),
        BindAction(key: "bind_move_left", label: "Left", group: "Movement", defaults: ["A"]),
        BindAction(key: "bind_move_right", label: "Right", group: "Movement", defaults: ["D"]),
        BindAction(key: "bind_walk", label: "Walk (hold)", group: "Movement", defaults: ["Left Alt"]),
        BindAction(key: "bind_camera_up", label: "Up", group: "Camera", defaults: ["Up"]),
        BindAction(key: "bind_camera_down", label: "Down", group: "Camera", defaults: ["Down"]),
        BindAction(key: "bind_camera_left", label: "Left", group: "Camera", defaults: ["Left"]),
        BindAction(key: "bind_camera_right", label: "Right", group: "Camera", defaults: ["Right"]),
        BindAction(key: "bind_r1", label: "Attack (R1)", group: "Combat", defaults: ["Mouse Left", "3"]),
        BindAction(key: "bind_r2", label: "Charged attack (R2)", group: "Combat", defaults: ["Mouse Back", "F"]),
        BindAction(key: "bind_l1", label: "Transform weapon (L1)", group: "Combat", defaults: ["1", "Mouse Forward"]),
        BindAction(key: "bind_l2", label: "Firearm (L2)", group: "Combat", defaults: ["Mouse Right", "R"]),
        BindAction(key: "bind_circle", label: "Dodge, sprint (Circle)", group: "Combat", defaults: ["Left Shift", "Escape"]),
        BindAction(key: "bind_r3", label: "Lock on (R3)", group: "Combat", defaults: ["Mouse Middle", "C"]),
        BindAction(key: "bind_cross", label: "Interact, confirm (Cross)", group: "Actions", defaults: ["Space", "Return", "Keypad Enter"]),
        BindAction(key: "bind_square", label: "Quick item (Square)", group: "Actions", defaults: ["E"]),
        BindAction(key: "bind_triangle", label: "Blood Vial (Triangle)", group: "Actions", defaults: ["Q"]),
        BindAction(key: "bind_l3", label: "L3", group: "Actions", defaults: ["Z"]),
        BindAction(key: "bind_dpad_up", label: "Up", group: "D-pad", defaults: ["I"]),
        BindAction(key: "bind_dpad_down", label: "Down", group: "D-pad", defaults: ["K"]),
        BindAction(key: "bind_dpad_left", label: "Left (left-hand weapon)", group: "D-pad", defaults: ["J"]),
        BindAction(key: "bind_dpad_right", label: "Right (right-hand weapon)", group: "D-pad", defaults: ["L"]),
        BindAction(key: "bind_options", label: "Menu (Options)", group: "Menus", defaults: ["O", "F1"]),
        BindAction(key: "bind_touch_left", label: "Gestures (touchpad left)", group: "Menus", defaults: ["Tab"]),
        BindAction(key: "bind_touch_right", label: "Touchpad right side", group: "Menus", defaults: ["Backspace"]),
    ]
    static let groups = ["Movement", "Camera", "Combat", "Actions", "D-pad", "Menus"]

    private var bound: [String: [String]]

    init() { bound = Dictionary(uniqueKeysWithValues: Self.actions.map { ($0.key, $0.defaults) }) }

    init(ini: IniFile) {
        self.init()
        for action in Self.actions {
            if let v = ini[action.key] { bound[action.key] = Self.parse(v) }
        }
    }

    /// Names the game can use, as the game reads them: trimmed, case-insensitive, Cmd and unknown
    /// names dropped (an unknown one stays out of the launcher too), at most `slots`.
    static func parse(_ value: String) -> [String] {
        let names = value.split(separator: ",").compactMap { canonical(String($0).trimmingCharacters(in: .whitespaces)) }
        return Array(names.prefix(slots))
    }

    /// The spelling the game logs and the launcher writes, or nil if the game would ignore it.
    static func canonical(_ name: String) -> String? {
        let all = mouseNames + KeyCodes.names
        guard let match = all.first(where: { $0.caseInsensitiveCompare(name) == .orderedSame }),
              !reserved.contains(match) else { return nil }
        return match
    }

    subscript(action: BindAction) -> [String] {
        get { bound[action.key] ?? action.defaults }
        set { bound[action.key] = Array(newValue.prefix(Self.slots)) }
    }

    /// Puts `name` in slot `slot` of `action` (past the end: appended); nil clears the slot.
    mutating func set(_ action: BindAction, slot: Int, to name: String?) {
        var names = self[action]
        if let name {
            if let same = names.firstIndex(of: name) { names.remove(at: same) } // no duplicate in one action
            if slot < names.count { names[slot] = name } else { names.append(name) }
        } else if slot < names.count {
            names.remove(at: slot)
        }
        self[action] = names
    }

    var isDefault: Bool { Self.actions.allSatisfy { self[$0] == $0.defaults } }

    /// The other actions that use `name`: one key can press several pad inputs, so this is only a
    /// warning.
    func others(using name: String, besides action: BindAction) -> [BindAction] {
        Self.actions.filter { $0 != action && self[$0].contains(name) }
    }

    /// Only the actions changed from the defaults are written (an absent key is the default).
    func apply(to ini: inout IniFile) {
        for action in Self.actions {
            let names = self[action]
            ini[action.key] = names == action.defaults ? nil : names.joined(separator: ",")
        }
    }
}

/// A pad input of the game that controller buttons can press (src/runtime_bindings.c, same ini
/// keys and defaults).
struct PadAction: Identifiable, Hashable {
    let key: String
    let label: String
    let defaults: [String]
    var id: String { key }
}

/// The controller bindings: bbport.ini bind_pad_<input>=Button,Button, buttons named by position
/// as on a PlayStation pad, plus Select, the paddles and Misc. An absent key is the default (the
/// button of the same name), an empty one unbinds.
struct PadBindings: Equatable {
    static let buttons = ["Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "L3", "R3", "Options", "Touchpad", "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Select", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Misc"]

    static let actions: [PadAction] = [
        PadAction(key: "bind_pad_r1", label: "Attack (R1)", defaults: ["R1"]),
        PadAction(key: "bind_pad_r2", label: "Charged attack (R2)", defaults: ["R2"]),
        PadAction(key: "bind_pad_l1", label: "Transform weapon (L1)", defaults: ["L1"]),
        PadAction(key: "bind_pad_l2", label: "Firearm (L2)", defaults: ["L2"]),
        PadAction(key: "bind_pad_circle", label: "Dodge, sprint (Circle)", defaults: ["Circle"]),
        PadAction(key: "bind_pad_r3", label: "Lock on (R3)", defaults: ["R3"]),
        PadAction(key: "bind_pad_cross", label: "Interact, confirm (Cross)", defaults: ["Cross"]),
        PadAction(key: "bind_pad_square", label: "Quick item (Square)", defaults: ["Square"]),
        PadAction(key: "bind_pad_triangle", label: "Blood Vial (Triangle)", defaults: ["Triangle"]),
        PadAction(key: "bind_pad_l3", label: "L3", defaults: ["L3"]),
        PadAction(key: "bind_pad_dpad_up", label: "D-pad up", defaults: ["D-pad Up"]),
        PadAction(key: "bind_pad_dpad_down", label: "D-pad down", defaults: ["D-pad Down"]),
        PadAction(key: "bind_pad_dpad_left", label: "D-pad left (left-hand weapon)", defaults: ["D-pad Left"]),
        PadAction(key: "bind_pad_dpad_right", label: "D-pad right (right-hand weapon)", defaults: ["D-pad Right"]),
        PadAction(key: "bind_pad_options", label: "Menu (Options)", defaults: ["Options"]),
        PadAction(key: "bind_pad_touchpad", label: "Gestures (touchpad)", defaults: ["Touchpad", "Select"]),
    ]

    /// How a button is shown: its position on other pads where the name alone is unclear.
    static func label(_ name: String) -> String {
        switch name {
        case "Select": "Select (Share, Create, View)"
        case "Misc": "Misc (mic, capture, Xbox Share)"
        default: name
        }
    }

    private var bound: [String: [String]]

    init() { bound = Dictionary(uniqueKeysWithValues: Self.actions.map { ($0.key, $0.defaults) }) }

    init(ini: IniFile) {
        self.init()
        for action in Self.actions {
            if let v = ini[action.key] { bound[action.key] = Self.parse(v) }
        }
    }

    /// Names as the game reads them: trimmed, case-insensitive, unknown ones dropped, at most
    /// `Bindings.slots`.
    static func parse(_ value: String) -> [String] {
        let names = value.split(separator: ",").compactMap { part in
            let name = part.trimmingCharacters(in: .whitespaces)
            return buttons.first { $0.caseInsensitiveCompare(name) == .orderedSame }
        }
        return Array(names.prefix(Bindings.slots))
    }

    subscript(action: PadAction) -> [String] {
        get { bound[action.key] ?? action.defaults }
        set { bound[action.key] = Array(newValue.prefix(Bindings.slots)) }
    }

    /// Puts `name` in slot `slot` of `action` (past the end: appended); nil clears the slot.
    mutating func set(_ action: PadAction, slot: Int, to name: String?) {
        var names = self[action]
        if let name {
            if let same = names.firstIndex(of: name) { names.remove(at: same) }
            if slot < names.count { names[slot] = name } else { names.append(name) }
        } else if slot < names.count {
            names.remove(at: slot)
        }
        self[action] = names
    }

    var isDefault: Bool { Self.actions.allSatisfy { self[$0] == $0.defaults } }

    /// The other inputs a button presses (a warning only: one button may press several).
    func others(using name: String, besides action: PadAction) -> [PadAction] {
        Self.actions.filter { $0 != action && self[$0].contains(name) }
    }

    /// Only the inputs changed from the defaults are written.
    func apply(to ini: inout IniFile) {
        for action in Self.actions {
            let names = self[action]
            ini[action.key] = names == action.defaults ? nil : names.joined(separator: ",")
        }
    }
}
