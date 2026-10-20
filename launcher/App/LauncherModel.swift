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
    private var gameProcess: Process?

    @Published var settings = GameSettings()
    @Published var config = LaunchConfig()
    @Published private(set) var dataDir: URL
    @Published private(set) var gameProblem: String?
    @Published private(set) var checkingGame = false
    @Published private(set) var saveProblem: String?
    @Published private(set) var running = false
    @Published private(set) var stopping = false
    @Published private(set) var exitMessage: String?
    /// The last session's crash report (the game stopped with an error), shown as a sheet.
    @Published private(set) var crashReport: CrashReport?
    @Published var showingCrashReport = false

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
                }
            }
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
            gameProcess = try GameRunner.start(plan, log: logURL) { [weak self] status in self?.finished(status) }
            stopping = false
            running = true
            exitMessage = nil
            onStarted?()
        } catch {
            exitMessage = "The game could not start: \(error.localizedDescription)"
            onExit?(-1)
        }
    }

    /// Stop only this launcher's child. The native renderer watches its parent and exits too.
    func stop() {
        guard running, !stopping, let process = gameProcess, process.isRunning else { return }
        stopping = true
        process.terminate()
    }

    private func finished(_ status: Int32) {
        let wasStopped = stopping
        gameProcess = nil
        running = false
        stopping = false
        load()
        if status == 0 || wasStopped {
            exitMessage = nil
            crashReport = nil
            showingCrashReport = false
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
        onExit?(wasStopped ? 0 : status)
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
}
