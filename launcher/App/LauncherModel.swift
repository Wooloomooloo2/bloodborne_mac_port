// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import Combine
import Foundation

/// The Discord sign-in to The Hunter's Dream, as the Online tab shows it.
enum SignInState: Equatable {
    case idle, starting
    case waiting(page: URL, code: String)
    case failed(String)
}

/// The launcher window's state and actions.
@MainActor
final class LauncherModel: ObservableObject {
    let layout: Layout
    let secrets: SecretStore
    let updater = Updater(info: Bundle.main.infoDictionary ?? [:])
    private let env = ProcessInfo.processInfo.environment
    private var confFile = IniFile()
    private var settingsFile = IniFile()
    private var updaterChanges: AnyCancellable?

    @Published var settings = GameSettings()
    @Published var config = LaunchConfig()
    @Published var online = OnlineSettings()
    /// A shadNet password typed in the window: it goes to the keychain at the next save and is
    /// never shown, kept or logged otherwise.
    @Published var newShadnetPassword = ""
    @Published var p2pPortText = String(OnlineSettings.defaultP2PPort)
    @Published private(set) var shadnetPasswordStored = false
    @Published private(set) var signedIn = false
    @Published private(set) var signIn = SignInState.idle
    @Published private(set) var onlineProblem: String?
    private var signInTask: Task<Void, Never>?
    @Published private(set) var dataDir: URL
    @Published private(set) var gameProblem: String?
    @Published private(set) var checkingGame = false
    @Published private(set) var saveProblem: String?
    @Published private(set) var running = false
    @Published private(set) var exitMessage: String?
    /// The last session's crash report (the game stopped with an error), shown as a sheet.
    @Published private(set) var crashReport: CrashReport?
    @Published var showingCrashReport = false

    var onStarted: (() -> Void)?
    var onExit: ((Int32) -> Void)?

    init(layout: Layout, secrets: SecretStore = LoginKeychain()) {
        self.layout = layout
        self.secrets = secrets
        dataDir = layout.dataDir(env: ProcessInfo.processInfo.environment, config: LaunchConfig())
        // The window's update sheet follows the updater.
        updaterChanges = updater.objectWillChange.sink { [weak self] in self?.objectWillChange.send() }
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
        online = OnlineSettings(ini: settingsFile)
        p2pPortText = String(online.p2pPort)
        newShadnetPassword = ""
        refreshSecretState()
        checkGame()
    }

    func save() {
        config.apply(to: &confFile)
        settings.apply(to: &settingsFile)
        if let port = OnlineSettings.port(from: p2pPortText) {
            online.p2pPort = port
        } else {
            p2pPortText = String(online.p2pPort)
        }
        if !newShadnetPassword.isEmpty && !online.shadnetNpid.trimmingCharacters(in: .whitespaces).isEmpty {
            do {
                try online.storeShadnetPassword(newShadnetPassword, secrets: secrets)
                newShadnetPassword = ""
                onlineProblem = nil
            } catch {
                onlineProblem = "The shadNet password could not be stored in the keychain: \(error)"
            }
        }
        online.apply(to: &settingsFile)
        do {
            try confFile.write(to: confURL)
            try settingsFile.write(to: settingsURL)
            if online.hasSecretInFile {
                // As scripts/online.py does: a secret in clear makes the file the owner's alone.
                try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: settingsURL.path)
            }
        } catch {
            exitMessage = "Could not save the settings: \(error.localizedDescription)"
        }
        refreshSecretState()
    }

    /// What the keychain holds for the accounts in the window (after a load, a save, or a
    /// change of the shadNet account name).
    func refreshSecretState() {
        shadnetPasswordStored = online.shadnetPasswordStored(secrets: secrets)
        signedIn = online.signedIn(secrets: secrets)
    }

    /// The community game server (also the account service): BB_COMMUNITY_SERVER from the
    /// environment or launch.conf, else bbport.ini's community_server, else The Hunter's Dream.
    private var communitySetting: String? {
        env["BB_COMMUNITY_SERVER"] ?? confFile["BB_COMMUNITY_SERVER"].map(Shell.unquote) ?? settingsFile["community_server"]
    }

    /// The community server as shown; an address the sign-in refuses (not https) as written.
    var communityServer: String {
        (try? HuntersDream.communityAddress(communitySetting)) ?? communitySetting ?? HuntersDream.defaultCommunityServer
    }

    /// Links a Discord account: the window shows the page and the code until the service
    /// reports the approval; the token then goes to the keychain and the account into bbport.ini.
    func startSignIn() {
        signInTask?.cancel()
        onlineProblem = nil
        let community: String
        do {
            community = try HuntersDream.communityAddress(communitySetting)
        } catch {
            signIn = .failed((error as? HuntersDream.Failure)?.description ?? error.localizedDescription)
            return
        }
        signIn = .starting
        signInTask = Task { [weak self] in
            do {
                let account = try await HuntersDreamClient.signIn(community: community) { [weak self] start in
                    self?.signIn = .waiting(page: start.page, code: start.userCode)
                }
                guard let self, !Task.isCancelled else { return }
                try online.signIn(onlineId: account.onlineId, token: account.token, secrets: secrets)
                signIn = .idle
                save()
            } catch is CancellationError {
                self?.signIn = .idle
            } catch {
                self?.signIn = .failed((error as? HuntersDream.Failure)?.description ?? error.localizedDescription)
            }
        }
    }

    func cancelSignIn() {
        signInTask?.cancel()
        signInTask = nil
        signIn = .idle
    }

    func signOut() {
        cancelSignIn()
        online.signOut(secrets: secrets)
        save()
    }

    func copyToPasteboard(_ text: String) {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
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
        items += ["cap \(settings.frameCap.rawValue) FPS", "\(settings.frameCap.hasFixedSteps ? settings.gameTiming.rawValue : "variable") timing", settings.fullscreen ? "full screen" : "windowed"]
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

    /// One check a day, silent unless it finds an update: when the launcher window shows, or as
    /// the game starts when the window is turned off (main.swift shows the result after the game).
    func checkForUpdatesIfDue() {
        guard config.checkUpdates == true, !running, Updates.isDue(last: config.lastUpdateCheck, now: Date()) else { return }
        config.lastUpdateCheck = Date()
        save()
        Task { await updater.check(includeTestBuilds: config.testBuilds, skipping: config.skippedVersion, manual: false) }
    }

    /// Automatic checks are off until the player agrees: asked once, at the first start with the
    /// updater, before the launcher window or the game (also when the window is turned off).
    func askAboutUpdatesIfNeeded() {
        guard config.checkUpdates == nil else { return }
        let alert = NSAlert()
        alert.messageText = "Check for updates automatically?"
        alert.informativeText = "Bloodborne can look for a new version of this port on its GitHub releases page once a day, when the launcher opens. Nothing is downloaded or installed without asking you. You can change this in the update window (Bloodborne → Check for Updates…), which works either way."
        alert.addButton(withTitle: "Check Automatically")
        alert.addButton(withTitle: "Don't Check")
        NSApp.activate()
        config.checkUpdates = alert.runModal() == .alertFirstButtonReturn
        save()
    }

    /// The app menu's Check for Updates….
    func checkForUpdatesNow() {
        Task { await updater.check(includeTestBuilds: config.testBuilds, skipping: nil, manual: true) }
    }

    func installUpdate() {
        guard let update = updater.available, !running else { return }
        save()
        Task { await updater.install(update, layout: layout, bundle: Bundle.main.bundleURL) }
    }

    func skipUpdate() {
        config.skippedVersion = updater.available?.version.text
        save()
        updater.dismiss()
    }

    /// The update window's checkboxes: saved at once (launch.conf is read again after a game).
    func setUpdatePreference(automatic: Bool? = nil, testBuilds: Bool? = nil) {
        if let automatic { config.checkUpdates = automatic }
        if let testBuilds { config.testBuilds = testBuilds }
        save()
    }

    var updateInstallProblem: String? { Updates.installProblem(layout: layout, bundle: Bundle.main.bundleURL) }

    func effect(_ key: String) -> Bool { settings.effects[key] ?? false }
    func setEffect(_ key: String, _ on: Bool) { settings.effects[key] = on }
}
