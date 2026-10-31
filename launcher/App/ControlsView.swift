// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import Carbon.HIToolbox
import SwiftUI

/// The launcher's Controls page: mouse camera settings and the keyboard and mouse bindings
/// (launcher/Model/Bindings.swift; the game reads them at start, src/runtime_bindings.c).
struct ControlsView: View {
    @ObservedObject var model: LauncherModel
    /// The slot waiting for a key or mouse button.
    @State private var capturing: Capture?

    struct Capture: Identifiable {
        let action: BindAction
        let slot: Int
        var id: String { "\(action.key)/\(slot)" }
    }

    var body: some View {
        HStack(alignment: .top, spacing: 0) {
            Form {
                Section("Mouse") {
                    Toggle("Mouse camera", isOn: $model.settings.mouseLook)
                        .help("Click in the game window to turn the camera with the mouse; \u{2318} gives the cursor back. Controllers always work.")
                    NumberSlider(label: "Sensitivity", value: $model.settings.mouseSensitivity, range: 0.1...10, format: "%.1f",
                                 help: "How far the camera turns for a mouse movement.")
                    NumberSlider(label: "Vertical speed", value: $model.settings.mouseVertical, range: 0.25...4, format: "%.2f\u{00D7}",
                                 help: "How fast the camera turns up and down, relative to left and right.")
                    Toggle("Invert vertical", isOn: $model.settings.mouseInvertY)
                    Text("Turning still too slow? Raise Camera speed in the game's own options too. Mouse buttons and the wheel reach the game while it holds the cursor. The in-game menu (\u{2318},) has these sliders too, live.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Section("Keyboard") {
                    Toggle("Camera keys are the d-pad with the mouse camera", isOn: $model.settings.cameraKeysDpad)
                        .disabled(!model.settings.mouseLook)
                        .help("While the mouse turns the camera, the camera keys (the arrow keys by default) scroll menus and switch weapons and items instead.")
                }
                Section("Controller") {
                    Toggle("L3 + R3 opens the in-game menu", isOn: $model.settings.padMenuCombo)
                        .help("Pressing both sticks in opens the settings menu during play. Turn it off if you press them by accident; \u{2318}, still opens it.")
                }
                groups(["Movement", "Camera"])
            }
            .formStyle(.grouped)
            Form {
                groups(["Combat", "Actions", "D-pad", "Menus"])
                Section {
                    HStack {
                        Text("A key can do several things; \u{2318} is kept for Mac shortcuts.")
                            .font(.caption).foregroundStyle(.secondary)
                        Spacer()
                        Button("Reset to Defaults") { model.settings.bindings = Bindings() }
                            .disabled(model.settings.bindings.isDefault)
                    }
                }
            }
            .formStyle(.grouped)
        }
        .sheet(item: $capturing) { capture in
            CaptureSheet(action: capture.action, slot: capture.slot,
                         current: capture.slot < model.settings.bindings[capture.action].count
                            ? model.settings.bindings[capture.action][capture.slot] : nil) { name in
                model.settings.bindings.set(capture.action, slot: capture.slot, to: name)
                capturing = nil
            } cancel: {
                capturing = nil
            }
        }
    }

    private func groups(_ names: [String]) -> some View {
        ForEach(names, id: \.self) { group in
            Section(group) {
                ForEach(Bindings.actions.filter { $0.group == group }) { action in
                    row(action)
                }
            }
        }
    }

    private func row(_ action: BindAction) -> some View {
        let names = model.settings.bindings[action]
        // Two slots always; one more empty one while there is room.
        let shown = min(Bindings.slots, max(2, names.count + (names.count >= 2 ? 1 : 0)))
        let shared = names.flatMap { model.settings.bindings.others(using: $0, besides: action) }
        return LabeledContent {
            HStack(spacing: 4) {
                if !shared.isEmpty {
                    Image(systemName: "exclamationmark.triangle").foregroundStyle(.orange)
                        .help("Also used by: " + shared.map { "\($0.group) \u{2013} \($0.label)" }.joined(separator: ", "))
                }
                ForEach(0..<shown, id: \.self) { slot in
                    let name = slot < names.count ? names[slot] : nil
                    Button { capturing = Capture(action: action, slot: slot) } label: {
                        Text(name.map(KeyLabel.text) ?? (slot == 2 || slot == 3 ? "+" : "\u{2013}"))
                            .frame(minWidth: slot < 2 ? 70 : 16)
                            .foregroundStyle(name == nil ? .secondary : .primary)
                    }
                    .help(name.map { "\($0) \u{2013} click to change" } ?? "Click, then press a key or mouse button")
                }
            }
        } label: {
            Text(action.label)
        }
    }
}

/// A slider with its value, for the mouse settings.
private struct NumberSlider: View {
    let label: String
    @Binding var value: Double
    let range: ClosedRange<Double>
    let format: String
    let help: String

    var body: some View {
        LabeledContent(label) {
            HStack {
                Slider(value: $value, in: range)
                Text(String(format: format, value)).monospacedDigit().frame(width: 44, alignment: .trailing)
            }
        }
        .help(help)
    }
}

/// Waits for one key, mouse button or wheel turn in its box; Clear empties the slot.
private struct CaptureSheet: View {
    let action: BindAction
    let slot: Int
    let current: String?
    let done: (String?) -> Void
    let cancel: () -> Void
    @State private var message: String?
    @State private var monitor: Any?

    var body: some View {
        VStack(spacing: 14) {
            Text("\(action.group): \(action.label)").font(.headline)
            CaptureArea { name in done(name) } refuse: { message = $0 }
                .frame(width: 360, height: 120)
                .overlay {
                    VStack(spacing: 6) {
                        Text("Press a key, click a mouse button or turn the wheel here")
                        if let current { Text("Now: \(KeyLabel.text(current))").foregroundStyle(.secondary) }
                    }
                    .allowsHitTesting(false)
                }
                .background(RoundedRectangle(cornerRadius: 8).fill(.quaternary))
            if let message { Text(message).font(.caption).foregroundStyle(.orange) }
            HStack {
                Button("Clear") { done(nil) }.disabled(current == nil)
                Spacer()
                Button("Cancel", action: cancel)
            }
        }
        .padding(20)
        .frame(width: 400)
        .onAppear {
            // Every key goes to the binding, Esc and Return too (Esc is Circle by default).
            monitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .flagsChanged]) { event in
                switch KeyLabel.name(for: event) {
                case .bound(let name): done(name)
                case .refused(let why): message = why
                case .ignored: break
                }
                return nil
            }
        }
        .onDisappear {
            if let monitor { NSEvent.removeMonitor(monitor) }
            monitor = nil
        }
    }
}

/// The box that takes mouse buttons and the wheel.
private struct CaptureArea: NSViewRepresentable {
    let bound: (String) -> Void
    let refuse: (String) -> Void

    final class View: NSView {
        var bound: (String) -> Void = { _ in }
        private var wheel = 0.0
        override func mouseDown(with event: NSEvent) { bound("Mouse Left") }
        override func rightMouseDown(with event: NSEvent) { bound("Mouse Right") }
        override func otherMouseDown(with event: NSEvent) {
            // NSEvent numbers: 2 middle, 3 back, 4 forward (SDL's X1 and X2).
            switch event.buttonNumber {
            case 2: bound("Mouse Middle")
            case 3: bound("Mouse Back")
            case 4: bound("Mouse Forward")
            default: break
            }
        }
        override func scrollWheel(with event: NSEvent) {
            // Physical direction, as the game counts it (bbport_mouse.cpp), not "natural scrolling".
            let dy = event.isDirectionInvertedFromDevice ? -event.scrollingDeltaY : event.scrollingDeltaY
            wheel += event.hasPreciseScrollingDeltas ? dy / 10 : dy
            if wheel >= 1 { bound("Wheel Up") } else if wheel <= -1 { bound("Wheel Down") }
        }
    }

    func makeNSView(context: Context) -> View { let v = View(); v.bound = bound; return v }
    func updateNSView(_ view: View, context: Context) { view.bound = bound }
}

/// Names for keys: the ini's SDL names and what the launcher shows.
enum KeyLabel {
    enum Result { case bound(String), refused(String), ignored }

    /// A key press (keyDown, or a modifier going down in flagsChanged) as a binding.
    static func name(for event: NSEvent) -> Result {
        if event.type == .flagsChanged {
            let flag: NSEvent.ModifierFlags? = switch Int(event.keyCode) {
            case kVK_Shift, kVK_RightShift: .shift
            case kVK_Option, kVK_RightOption: .option
            case kVK_Control, kVK_RightControl: .control
            case kVK_Command, kVK_RightCommand: .command
            case kVK_CapsLock: nil // each press toggles it: take either
            default: .function
            }
            if let flag, !event.modifierFlags.contains(flag) { return .ignored } // released
        }
        guard let name = KeyCodes.name(keyCode: event.keyCode) else {
            return .refused("The game can't read that key.")
        }
        if Bindings.reserved.contains(name) { return .refused("\u{2318} is kept for Mac shortcuts (and gives the cursor back).") }
        return .bound(name)
    }

    private static let fixed: [String: String] = [
        "Mouse Left": "Left click", "Mouse Right": "Right click", "Mouse Middle": "Middle click",
        "Mouse Back": "Back button", "Mouse Forward": "Forward button",
        "Wheel Up": "Wheel up", "Wheel Down": "Wheel down",
        "Left Shift": "\u{21E7} Left", "Right Shift": "\u{21E7} Right",
        "Left Alt": "\u{2325} Left", "Right Alt": "\u{2325} Right",
        "Left Ctrl": "\u{2303} Left", "Right Ctrl": "\u{2303} Right",
        "Up": "\u{2191}", "Down": "\u{2193}", "Left": "\u{2190}", "Right": "\u{2192}",
        "Backspace": "Delete \u{232B}", "Delete": "Fwd Del \u{2326}", "Escape": "Esc",
        "Insert": "Help", "Keypad Enter": "Enter",
    ]

    /// What is printed on the key in the current keyboard layout (letters, digits, symbols), else
    /// a readable name.
    static func text(_ name: String) -> String {
        if let label = fixed[name] { return label }
        if name.count == 1, let code = KeyCodes.keyCode(name: name), let typed = typed(code) { return typed }
        return name
    }

    private static func typed(_ code: UInt16) -> String? {
        guard let source = TISCopyCurrentKeyboardLayoutInputSource()?.takeRetainedValue(),
              let raw = TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData) else { return nil }
        let data = Unmanaged<CFData>.fromOpaque(raw).takeUnretainedValue() as Data
        return data.withUnsafeBytes { bytes -> String? in
            guard let layout = bytes.baseAddress?.assumingMemoryBound(to: UCKeyboardLayout.self) else { return nil }
            var dead: UInt32 = 0
            var chars = [UniChar](repeating: 0, count: 4)
            var length = 0
            let status = UCKeyTranslate(layout, code, UInt16(kUCKeyActionDisplay), 0, UInt32(LMGetKbdType()),
                                        OptionBits(kUCKeyTranslateNoDeadKeysBit), &dead, chars.count, &length, &chars)
            guard status == noErr, length > 0 else { return nil }
            let s = String(utf16CodeUnits: chars, count: length).uppercased()
            return s.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? nil : s
        }
    }
}
