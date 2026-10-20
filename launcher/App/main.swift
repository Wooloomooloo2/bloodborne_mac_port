// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne.app's main executable: the launcher window (docs/superpowers/specs/2026-10-16-launcher-design.md).
import AppKit
import SwiftUI

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private var model: LauncherModel!
    private var window: NSWindow?
    private var direct = false
    private var quitAfterStop = false

    func applicationDidFinishLaunching(_ notification: Notification) {
        guard let layout = Layout.detect(bundleURL: Bundle.main.bundleURL,
                                         info: Bundle.main.infoDictionary ?? [:]) else {
            let alert = NSAlert()
            alert.messageText = "This copy of Bloodborne.app is incomplete"
            alert.informativeText = "Download the app again, or run scripts/macos/make_app.sh in the repository."
            alert.runModal()
            NSApp.terminate(nil)
            return
        }
        model = LauncherModel(layout: layout)
        model.onStarted = { [weak self] in
            guard let self else { return }
            if self.direct {
                // Respect direct launch: only the game's window has a Dock icon.
                self.window?.orderOut(nil)
                NSApp.setActivationPolicy(.accessory)
            } else {
                // Keep the launcher available behind the game, including its Stop button.
                self.showWindow(activate: false)
            }
        }
        model.onExit = { [weak self] status in self?.gameEnded(status) }
        model.load()
        if !NSEvent.modifierFlags.contains(.option) && model.canSkipWindow {
            direct = true
            model.play()
        } else {
            showWindow()
        }
    }

    private func gameEnded(_ status: Int32) {
        if quitAfterStop {
            NSApp.reply(toApplicationShouldTerminate: true)
            return
        }
        if direct && status == 0 {
            NSApp.terminate(nil)
            return
        }
        direct = false
        showWindow()
    }

    private func showWindow(activate: Bool = true) {
        NSApp.setActivationPolicy(.regular)
        if window == nil {
            let window = NSWindow(contentRect: NSRect(origin: .zero, size: LauncherView.size),
                                  styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
            window.title = "Bloodborne"
            window.contentView = NSHostingView(rootView: LauncherView(model: model))
            window.isReleasedWhenClosed = false
            window.center()
            self.window = window
        }
        if activate {
            window?.makeKeyAndOrderFront(nil)
            NSApp.activate()
        } else {
            window?.orderBack(nil)
        }
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        direct = false
        showWindow()
        return false
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard model != nil && model.running else { return .terminateNow }
        quitAfterStop = true
        model.stop()
        return .terminateLater
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        !model.running
    }

    func applicationWillTerminate(_ notification: Notification) {
        if model != nil && !model.running { model.save() }
    }
}

// Top-level code runs on the main thread; AppKit and the delegate are main-actor isolated.
MainActor.assumeIsolated {
    let app = NSApplication.shared
    let delegate = AppDelegate() // app.delegate is weak: this local lives while app.run() runs
    app.delegate = delegate
    let menu = NSMenu()
    let appItem = NSMenuItem()
    menu.addItem(appItem)
    let appMenu = NSMenu()
    appMenu.addItem(withTitle: "Quit Bloodborne", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
    appItem.submenu = appMenu
    app.mainMenu = menu
    app.setActivationPolicy(.regular)
    app.run()
}
