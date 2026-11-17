// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// Hands the focus to the game's window. The window belongs to the GPU host (bbgpu-host), which
// bb-probe starts itself, not through Launch Services, so macOS never activates it: the launcher
// stayed the active app, keys went to it, and the game could not hide the mouse cursor (macOS
// ignores that from an app in the background). Since macOS 14 an app cannot take the focus for
// itself; the active app hands it over (yieldActivation, activate(from:)). A full-screen window
// on another display can still lose it in the Spaces switch: the GPU host takes it back then
// (gpu/shim/bbport_cursor.mm, ActivateAfterWindowMove).
import AppKit
import os

@MainActor
final class GameFocus {
    private var timer: Timer?
    private var handled = Set<pid_t>()
    private var seen = Set<pid_t>()
    private let log = Logger(subsystem: "io.github.wooloomooloo2.bloodborne-mac-port", category: "focus")

    /// Watches for GPU host processes from this app bundle while the game runs (a Restart starts
    /// a new one) and activates each new one, but only while the launcher is the active app:
    /// never takes the focus from an app the player switched to.
    func start() {
        stop()
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.poll() }
        }
    }

    func stop() {
        timer?.invalidate()
        timer = nil
        handled.removeAll()
    }

    private func poll() {
        let bundle = Bundle.main.bundleURL.standardizedFileURL.path + "/"
        for app in NSWorkspace.shared.runningApplications {
            guard let exe = app.executableURL?.standardizedFileURL,
                  exe.lastPathComponent.hasPrefix("bbgpu-host"), exe.path.hasPrefix(bundle),
                  !app.isTerminated, !handled.contains(app.processIdentifier) else { continue }
            let pid = app.processIdentifier
            if seen.insert(pid).inserted {
                // Permission first, before its window exists: opening the window asks macOS to
                // activate the host, and that takes the focus from the launcher even when the
                // host is refused (then no app is active). Yielded, the host's own request wins.
                let active = NSApp.isActive
                if active { NSApp.yieldActivation(to: app) }
                log.notice("host \(pid) found, window \(Self.hasWindow(pid)), launcher active \(active)")
            }
            // Wait for its window: activating an app without one gives no key window.
            guard Self.hasWindow(pid) else { continue }
            handled.insert(pid)
            guard NSApp.isActive else {
                log.notice("host \(pid) has a window; launcher not active, no handoff")
                continue
            }
            NSApp.yieldActivation(to: app)
            let ok = app.activate(from: .current, options: [])
            log.notice("host \(pid) activate -> \(ok); active now \(app.isActive)")
        }
    }

    private static func hasWindow(_ pid: pid_t) -> Bool {
        guard let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements],
                                                    kCGNullWindowID) as? [[String: Any]] else { return false }
        return list.contains { ($0[kCGWindowOwnerPID as String] as? pid_t) == pid
                               && ($0[kCGWindowLayer as String] as? Int) == 0 }
    }
}
