// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne.app's main executable: the launcher window (docs/superpowers/specs/2026-10-16-launcher-design.md).
import AppKit
import SwiftUI

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuItemValidation {
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
            ModsWindow.hide()
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
        // --updated: the copy an update just installed (Updater.install) shows its window.
        // (⌥ is read before the update question, which may be answered after letting go of it.)
        // --benchmark: show the window and run the benchmark at once (scripted comparisons).
        if CommandLine.arguments.contains("--benchmark") {
            showWindow()
            model.startBenchmark(confirm: false)
            return
        }
        let wantsWindow = CommandLine.arguments.contains("--updated") || NSEvent.modifierFlags.contains(.option)
        // Asked before the game too: players who turned the window off would never see it.
        model.askAboutUpdatesIfNeeded()
        if !wantsWindow && model.canSkipWindow {
            direct = true
            // The daily check runs in the background while the game plays (started here, not
            // later: its save() must not rewrite bbport.ini while the game's menu may change it).
            model.checkForUpdatesIfDue()
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
            // With the window turned off, an update the check found shows once, after the session.
            guard model.updater.available != nil else {
                NSApp.terminate(nil)
                return
            }
        } else if status != 0 {
            // The crash report comes first; the footer still links the update.
            model.updater.showing = false
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
            model.checkForUpdatesIfDue()
        } else {
            window?.orderBack(nil)
        }
    }

    @objc func showAbout(_ sender: Any?) { About.show() }

    @objc func showAchievements(_ sender: Any?) {
        model.page = .achievements
        showWindow()
    }

    @objc func checkForUpdates(_ sender: Any?) {
        showWindow()
        model.checkForUpdatesNow()
    }

    func validateMenuItem(_ item: NSMenuItem) -> Bool {
        item.action != #selector(checkForUpdates(_:)) || (model != nil && !model.running && !model.updater.busy)
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        direct = false
        showWindow()
        return false
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        // Quitting the launcher stops a game it started; the benchmark's launches end with the launcher.
        guard model != nil && model.running && model.stop() else { return .terminateNow }
        quitAfterStop = true
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
    appMenu.addItem(withTitle: "About Bloodborne", action: #selector(AppDelegate.showAbout(_:)), keyEquivalent: "").target = delegate
    appMenu.addItem(withTitle: "Check for Updates…", action: #selector(AppDelegate.checkForUpdates(_:)), keyEquivalent: "").target = delegate
    let achievements = appMenu.addItem(withTitle: "Achievements…", action: #selector(AppDelegate.showAchievements(_:)), keyEquivalent: "t")
    achievements.target = delegate
    achievements.keyEquivalentModifierMask = [.command, .shift]
    appMenu.addItem(.separator())
    appMenu.addItem(withTitle: "Hide Bloodborne", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
    appMenu.addItem(.separator())
    appMenu.addItem(withTitle: "Quit Bloodborne", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
    appItem.submenu = appMenu
    app.mainMenu = menu
    app.setActivationPolicy(.regular)
    app.run()
}
