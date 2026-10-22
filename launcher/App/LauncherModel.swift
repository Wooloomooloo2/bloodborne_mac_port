// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import Foundation

/// The launcher window's state and actions.
@MainActor
final class LauncherModel: ObservableObject {
    let layout: Layout
    private let env = ProcessInfo.processInfo.environment
    private var confFile = IniFile()
    private var settingsFile = IniFile()

    @Published var settings = GameSettings()
    @Published var config = LaunchConfig()
    @Published private(set) var dataDir: URL
    @Published private(set) var gameProblem: String?
    @Published private(set) var checkingGame = false
    @Published private(set) var saveProblem: String?
    @Published private(set) var running = false
    @Published private(set) var exitMessage: String?
    /// The last session's crash report (the game stopped with an error), shown as a sheet.
    @Published private(set) var crashReport: CrashReport?
    @Published var showingCrashReport = false
    @Published var availableUpdate: AppUpdate?
    @Published var isCheckingForUpdates = false
    @Published var isUpdating = false
    @Published var updateStatus: String?

    var onStarted: (() -> Void)?
    var onExit: ((Int32) -> Void)?

    init(layout: Layout) {
        self.layout = layout
        dataDir = layout.dataDir(env: ProcessInfo.processInfo.environment, config: LaunchConfig())
    }

    var confURL: URL { layout.launchConfURL(env: env) }
    var settingsURL: URL { URL(fileURLWithPath: env["BB_CONFIG"] ?? dataDir.appendingPathComponent("bbport.ini").path) }
    var logURL: URL { Logs.folder(home: URL(fileURLWithPath: NSHomeDirectory())).appendingPathComponent("game.log") }
    var saveDir: URL { config.saveDir ?? SaveFolder.defaultURL(dataDir: dataDir) }
    var outputChoices: [PixelSize] {
        Resolution.outputs.contains(settings.output) ? Resolution.outputs : Resolution.outputs + [settings.output]
    }
    var upscalerChoices: [Upscaler] {
        Upscaler.launcherChoices.contains(settings.upscaler) ? Upscaler.launcherChoices
            : Upscaler.launcherChoices + [settings.upscaler]
    }
    var canPlay: Bool { !running && !checkingGame && config.gameDir != nil && gameProblem == nil }
    /// Straight into the game (no window): the toggle is off and a game folder is known.
    var canSkipWindow: Bool {
        !config.showLauncher && config.gameDir.map {
            FileManager.default.fileExists(atPath: $0.appendingPathComponent("eboot.bin").path)
        } == true
    }

    /// Reads launch.conf and bbport.ini (again after each game session: the in-game menu may
    /// have changed bbport.ini).
    func load() {
        confFile = IniFile(contentsOf: confURL)
        config = LaunchConfig(ini: confFile)
        if config.gameDir == nil { config.gameDir = layout.defaultGameDir() }
        dataDir = layout.dataDir(env: env, config: config)
        let exists = FileManager.default.fileExists(atPath: settingsURL.path)
        settingsFile = exists ? IniFile(contentsOf: settingsURL) : IniFile(text: GameSettings.newFileHeader)
        settings = GameSettings(ini: settingsFile)
        checkGame()
        checkForUpdates(quiet: true)
    }

    func save() {
        config.apply(to: &confFile)
        settings.apply(to: &settingsFile)
        do {
            try confFile.write(to: confURL)
            try settingsFile.write(to: settingsURL)
        } catch {
            exitMessage = "Could not save the settings: \(error.localizedDescription)"
        }
    }

    func checkGame() {
        guard let folder = config.gameDir else {
            gameProblem = "Choose the game folder (CUSA03173 with update 1.09, the folder containing eboot.bin)."
            return
        }
        checkingGame = true
        let layout = self.layout
        Task.detached {
            let problem = GameRunner.checkGame(folder, layout: layout)
            await MainActor.run {
                if self.config.gameDir == folder {
                    self.gameProblem = problem
                    self.checkingGame = false
                    if problem == nil {
                        self.applyGameIcon(from: folder)
                    }
                }
            }
        }
    }

    private func applyGameIcon(from folder: URL) {
        let iconURL = folder.appendingPathComponent("sce_sys/icon0.png")
        guard FileManager.default.fileExists(atPath: iconURL.path),
              let iconImage = NSImage(contentsOf: iconURL) else { return }
        NSApplication.shared.applicationIconImage = iconImage
        let bundleURL = Bundle.main.bundleURL
        if bundleURL.pathExtension == "app" {
            NSWorkspace.shared.setIcon(iconImage, forFile: bundleURL.path, options: [])
        }
    }

    private func pickFolder(_ message: String, start: URL?) -> URL? {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.message = message
        panel.directoryURL = start
        return panel.runModal() == .OK ? panel.url : nil
    }

    func chooseGameFolder() {
        guard let url = pickFolder("Choose your Bloodborne game folder (CUSA03173, the one containing eboot.bin).",
                                   start: config.gameDir) else { return }
        config.gameDir = url
        checkGame()
    }

    func resetGameFolder() {
        config.gameDir = layout.defaultGameDir()
        checkGame()
    }

    func chooseSaveFolder() {
        guard let url = pickFolder("Choose the folder for your saves.", start: saveDir) else { return }
        setSaveFolder(url)
    }

    func resetSaveFolder() { setSaveFolder(SaveFolder.defaultURL(dataDir: dataDir)) }

    private func setSaveFolder(_ url: URL) {
        let old = saveDir
        if let problem = SaveFolder.problem(url, create: true) {
            saveProblem = problem
            return
        }
        if SaveFolder.shouldOfferCopy(from: old, to: url) {
            let alert = NSAlert()
            alert.messageText = "Copy your saves to the new folder?"
            alert.informativeText = "Your saves in \(old.path) are copied to \(url.path). The originals stay where they are."
            alert.addButton(withTitle: "Copy Saves")
            alert.addButton(withTitle: "Don't Copy")
            if alert.runModal() == .alertFirstButtonReturn {
                do {
                    try SaveFolder.copySaves(from: old, to: url)
                } catch {
                    saveProblem = "Copying the saves failed: \(error.localizedDescription)"
                    return
                }
            }
        }
        config.saveDir = url.standardizedFileURL.path == SaveFolder.defaultURL(dataDir: dataDir).standardizedFileURL.path ? nil : url
        saveProblem = nil
    }

    func play() {
        guard !running, let game = config.gameDir else { return }
        save()
        if GameRunner.isGameRunning {
            exitMessage = "Bloodborne is already running. Only one copy of the game can run at a time."
            onExit?(-1)
            return
        }
        if !GameRunner.hasRosetta {
            let alert = NSAlert()
            alert.messageText = "Bloodborne needs Rosetta 2"
            alert.informativeText = "The game runs its original PlayStation 4 code through Rosetta 2, Apple's translator for Intel code. Install it now? (macOS asks for an administrator password.)"
            alert.addButton(withTitle: "Install")
            alert.addButton(withTitle: "Cancel")
            guard alert.runModal() == .alertFirstButtonReturn else { onExit?(-1); return }
            guard GameRunner.installRosetta() else {
                exitMessage = "Rosetta 2 was not installed. In Terminal, run: softwareupdate --install-rosetta"
                onExit?(-1)
                return
            }
        }
        if let custom = config.saveDir, let problem = SaveFolder.problem(custom, create: false) {
            let alert = NSAlert()
            alert.messageText = "The save folder is not available"
            alert.informativeText = problem
            alert.addButton(withTitle: "Use Default")
            alert.addButton(withTitle: "Cancel")
            guard alert.runModal() == .alertFirstButtonReturn else { onExit?(-1); return }
            config.saveDir = nil
            save()
        }
        let plan = layout.launchPlan(gameDir: game, saveDir: config.saveDir, dataDir: dataDir, env: env)
        do {
            _ = try GameRunner.start(plan, log: logURL) { [weak self] status in self?.finished(status) }
            running = true
            exitMessage = nil
            onStarted?()
        } catch {
            exitMessage = "The game could not start: \(error.localizedDescription)"
            onExit?(-1)
        }
    }

    private func finished(_ status: Int32) {
        running = false
        load()
        if status == 0 {
            exitMessage = nil
            crashReport = nil
        } else {
            let log = (try? String(contentsOf: logURL, encoding: .utf8)) ?? ""
            let version = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "development build"
            let report = CrashReport(status: status, log: log, logName: logURL.lastPathComponent,
                                     system: .current(appVersion: version), settings: settingsSummary,
                                     home: NSHomeDirectory())
            crashReport = report
            exitMessage = report.summary
            showingCrashReport = true
        }
        onExit?(status)
    }

    /// The settings most likely to matter in a crash report.
    private var settingsSummary: [String] {
        var items = ["output \(settings.output.iniValue)", "upscaler \(settings.upscaler.rawValue)"]
        if settings.upscaler.usesPreset { items.append("quality \(Resolution.presetNames[settings.preset])") }
        items += ["cap \(settings.frameCap.rawValue) FPS", settings.fullscreen ? "full screen" : "windowed"]
        return items
    }

    func showLog() {
        GameRunner.run(["/usr/bin/open", "-R", logURL.path])
    }

    func copyCrashReport() {
        guard let crashReport else { return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(crashReport.report, forType: .string)
    }

    /// Copies the report (too long for a link) and opens a new GitHub issue to paste it into.
    func reportOnGitHub() {
        guard let crashReport else { return }
        copyCrashReport()
        NSWorkspace.shared.open(crashReport.issueURL)
    }

    func effect(_ key: String) -> Bool { settings.effects[key] ?? false }
    func setEffect(_ key: String, _ on: Bool) { settings.effects[key] = on }

    func checkForUpdates(quiet: Bool = true) {
        guard !isCheckingForUpdates else { return }
        isCheckingForUpdates = true
        Task.detached {
            let update = await Self.fetchLatestRelease()
            await MainActor.run {
                self.isCheckingForUpdates = false
                let current = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String
                    ?? Bundle.main.infoDictionary?["CFBundleVersion"] as? String
                    ?? "development build"
                if let update = update, AppUpdate.isNewer(remote: update.version, local: current) {
                    self.availableUpdate = update
                } else if !quiet {
                    let alert = NSAlert()
                    alert.messageText = "Bloodborne is up to date"
                    alert.informativeText = "Version \(current) is currently the newest version."
                    alert.addButton(withTitle: "OK")
                    alert.runModal()
                }
            }
        }
    }

    private static func fetchLatestRelease() async -> AppUpdate? {
        guard let url = URL(string: "https://api.github.com/repos/Wooloomooloo2/bloodborne_mac_port/releases/latest") else {
            return nil
        }
        var request = URLRequest(url: url)
        request.setValue("Bloodborne-Launcher", forHTTPHeaderField: "User-Agent")
        request.timeoutInterval = 10
        do {
            let (data, response) = try await URLSession.shared.data(for: request)
            guard let http = response as? HTTPURLResponse, http.statusCode == 200 else { return nil }
            guard let json = try JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let tag = json["tag_name"] as? String,
                  let htmlStr = json["html_url"] as? String,
                  let releaseURL = URL(string: htmlStr),
                  let assets = json["assets"] as? [[String: Any]] else {
                return nil
            }
            let title = (json["name"] as? String) ?? tag
            let body = (json["body"] as? String) ?? ""
            for asset in assets {
                if let name = asset["name"] as? String,
                   name.hasPrefix("Bloodborne-macOS") && name.hasSuffix(".zip"),
                   let downloadStr = asset["browser_download_url"] as? String,
                   let downloadURL = URL(string: downloadStr) {
                    return AppUpdate(version: tag, title: title, downloadURL: downloadURL, releaseURL: releaseURL, notes: body)
                }
            }
        } catch {
            return nil
        }
        return nil
    }

    func dismissUpdate() {
        availableUpdate = nil
    }

    func performUpdate() {
        guard let update = availableUpdate, !isUpdating else { return }
        let bundleURL = Bundle.main.bundleURL
        if bundleURL.pathExtension != "app" {
            NSWorkspace.shared.open(update.releaseURL)
            return
        }
        isUpdating = true
        updateStatus = "Downloading update…"

        let downloadURL = update.downloadURL
        let appPID = ProcessInfo.processInfo.processIdentifier

        Task.detached {
            do {
                let (tempZipURL, _) = try await URLSession.shared.download(from: downloadURL)
                await MainActor.run {
                    self.updateStatus = "Installing update…"
                }

                let script = """
                PID=\(appPID)
                BUNDLE="\(bundleURL.path)"
                ZIP="\(tempZipURL.path)"
                TMPDIR="/tmp/bb_update_$$\"

                mkdir -p "$TMPDIR"
                unzip -q "$ZIP" -d "$TMPDIR"
                NEW_APP="$TMPDIR/Bloodborne.app"

                if [ -d "$NEW_APP" ]; then
                    while kill -0 $PID 2>/dev/null; do sleep 0.2; done
                    if [ -f "$BUNDLE/Contents/Resources/Bloodborne.icns" ]; then
                        cp "$BUNDLE/Contents/Resources/Bloodborne.icns" "$NEW_APP/Contents/Resources/Bloodborne.icns" 2>/dev/null || true
                    fi
                    rm -rf "$BUNDLE"
                    mv "$NEW_APP" "$BUNDLE"
                    xattr -cr "$BUNDLE"
                    open "$BUNDLE"
                fi
                rm -rf "$TMPDIR" "$ZIP"
                """

                let runner = Process()
                runner.executableURL = URL(fileURLWithPath: "/bin/bash")
                runner.arguments = ["-c", script]
                try runner.run()

                await MainActor.run {
                    NSApplication.shared.terminate(nil)
                }
            } catch {
                await MainActor.run {
                    self.isUpdating = false
                    self.updateStatus = nil
                    self.exitMessage = "Update failed: \(error.localizedDescription)"
                }
            }
        }
    }
}
