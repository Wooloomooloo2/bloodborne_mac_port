// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct LauncherView: View {
    /// The window's content size (main.swift): with the title bar, 768 of the 13" MacBook Air's
    /// 900 points, under a menu bar and a Dock.
    static let size = CGSize(width: 920, height: 740)

    @ObservedObject var model: LauncherModel

    var body: some View {
        VStack(spacing: 0) {
            // Two tabs so the whole window fits a 13" MacBook Air (1440 x 900 points, Dock shown)
            // without scrolling, even with every optional row showing.
            TabView {
                settingsTab.tabItem { Text("Settings") }
                onlineTab.tabItem { Text("Online") }
            }
            .padding(.horizontal, 12)
            Divider()
            HStack(spacing: 12) {
                Toggle("Show launcher every time", isOn: $model.config.showLauncher)
                Spacer()
                if let message = model.exitMessage {
                    Text(message).foregroundStyle(.red).lineLimit(2)
                    Button("Show Log", action: model.showLog)
                }
                Button(action: model.play) {
                    Text(model.running ? "Running…" : "Play").frame(minWidth: 80)
                }
                .keyboardShortcut(.defaultAction)
                .controlSize(.large)
                .disabled(!model.canPlay)
            }
            .padding()
        }
        .frame(width: LauncherView.size.width, height: LauncherView.size.height)
    }

    /// Display, upscaler, effects and locations: two columns.
    private var settingsTab: some View {
            HStack(alignment: .top, spacing: 0) {
                Form {
                    Section("Display") {
                        Picker("Output resolution", selection: $model.settings.output) {
                            ForEach(model.outputChoices, id: \.self) { Text($0.description).tag($0) }
                        }
                        Picker("Window", selection: $model.settings.fullscreen) {
                            Text("Windowed").tag(false)
                            Text("Full screen").tag(true)
                        }
                        .pickerStyle(.segmented)
                        Picker("Frame rate cap", selection: $model.settings.frameCap) {
                            ForEach(FrameCap.allCases) { Text($0.label).tag($0) }
                        }
                        .pickerStyle(.segmented)
                        if model.settings.frameCap.rawValue > 60 {
                            Text("Above 60 FPS is experimental: the game was made for 30. 120 needs a 120 Hz display.")
                                .font(.caption).foregroundStyle(.secondary)
                        }
                    }
                    Section("Upscaler") {
                        Picker("Upscaler", selection: $model.settings.upscaler) {
                            ForEach(model.upscalerChoices) { Text($0.label).tag($0) }
                        }
                        Picker("Quality", selection: $model.settings.preset) {
                            ForEach(Resolution.presetNames.indices, id: \.self) { Text(Resolution.presetNames[$0]).tag($0) }
                        }
                        .disabled(!model.settings.upscaler.usesPreset)
                        LabeledContent("Renders", value: "\(model.settings.renderSize) → \(model.settings.output)")
                        if model.settings.upscaler == .fsr3 && model.settings.renderer == .metal {
                            Text("FSR 3.1 returns to the Metal renderer in a later 0.5 update; MetalFX runs until then (your choice stays saved).")
                                .font(.caption).foregroundStyle(.secondary)
                        }
                        if model.settings.upscaler == .fsr3 {
                            Toggle("Sharpening", isOn: $model.settings.sharpen)
                            if model.settings.sharpen {
                                Slider(value: $model.settings.sharpness, in: 0...2) { Text("Strength") }
                            }
                        }
                    }
                    Section("Advanced") {
                        if model.layout.hasMetalRenderer {
                            Picker("Renderer", selection: $model.settings.renderer) {
                                ForEach(Renderer.allCases) { Text($0.label).tag($0) }
                            }
                        }
                        Picker("Shader compilation", selection: $model.settings.shaderCompile) {
                            ForEach(ShaderCompile.allCases) { Text($0.label).tag($0) }
                        }
                    }
                }
                .formStyle(.grouped)
                Form {
                    Section("Effects") {
                        ForEach(GameSettings.visualEffects) { effect in
                            Toggle(effect.label, isOn: Binding(get: { model.effect(effect.key) },
                                                               set: { model.setEffect(effect.key, $0) }))
                        }
                        Toggle("Show FPS", isOn: $model.settings.showFPS)
                        Toggle("Metal Performance HUD", isOn: $model.settings.metalHUD)
                            .help("Apple's Metal HUD: FPS, GPU time and frame pacing, top right")
                        Toggle("Skip intro videos", isOn: Binding(get: { model.effect("skip_intro") },
                                                                  set: { model.setEffect("skip_intro", $0) }))
                    }
                    Section("Locations") {
                        FolderRow(title: "Game folder", path: model.config.gameDir?.path ?? "Not chosen",
                                  problem: model.checkingGame ? nil : model.gameProblem,
                                  choose: model.chooseGameFolder, reset: model.resetGameFolder)
                        FolderRow(title: "Save folder", path: model.saveDir.path, problem: model.saveProblem,
                                  choose: model.chooseSaveFolder, reset: model.resetSaveFolder)
                    }
                }
                .formStyle(.grouped)
            }
    }

    /// Online play: the bbport.ini keys scripts/launch_settings.sh reads (docs/ONLINE.md).
    private var onlineTab: some View {
        Form {
            Section("Online play") {
                Toggle("Play online", isOn: $model.online.online)
                Text("Messages, bloodstains and ghosts come from the community server; bells and summons go through the co-op pool below. Off, the game never touches the network. Setup and troubleshooting: docs/ONLINE.md.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Co-op pool") {
                Picker("Pool", selection: $model.online.pool) {
                    ForEach(OnlinePool.allCases) { Text($0.label).tag($0) }
                }
                .pickerStyle(.segmented)
                if model.online.pool == .huntersdream {
                    huntersDreamRows
                } else {
                    shadnetRows
                }
            }
            Section("Connection") {
                Toggle("Ask the router to forward the co-op port (UPnP)", isOn: $model.online.upnp)
                TextField("Co-op UDP port", text: $model.p2pPortText)
                    .onChange(of: model.p2pPortText) { _, text in
                        let digits = String(text.filter { $0.isASCII && $0.isNumber }.prefix(5))
                        if digits != text { model.p2pPortText = digits }
                    }
                Text(portValid ? "3658 is the PlayStation 4's port; 0 picks any free port. A busy port falls back to a free one."
                               : "A port is a number from 0 to 65535.")
                    .font(.caption).foregroundStyle(portValid ? AnyShapeStyle(.secondary) : AnyShapeStyle(.red))
            }
            if let problem = model.onlineProblem {
                Text(problem).foregroundStyle(.red)
            }
        }
        .formStyle(.grouped)
    }

    private var portValid: Bool { OnlineSettings.port(from: model.p2pPortText) != nil }

    @ViewBuilder private var huntersDreamRows: some View {
        if model.signedIn {
            HStack {
                Text("Account")
                Spacer()
                Text("Signed in as \(model.online.thdOnlineId)").foregroundStyle(.secondary)
                Button("Sign Out", action: model.signOut)
            }
            Text("bbhost and Windows-port players are on this pool; it works through any router.")
                .font(.caption).foregroundStyle(.secondary)
        } else {
            switch model.signIn {
            case .idle, .failed:
                HStack {
                    Text("Account")
                    Spacer()
                    Text("Not signed in").foregroundStyle(.secondary)
                    Button("Sign in with Discord…", action: model.startSignIn)
                }
                Text("Free: links a Discord account the way the Windows port's launcher does. bbhost and Windows-port players are on this pool; it works through any router.")
                    .font(.caption).foregroundStyle(.secondary)
                if case let .failed(message) = model.signIn {
                    Text("The sign-in failed: \(message)").font(.caption).foregroundStyle(.red)
                }
            case .starting:
                HStack {
                    ProgressView().controlSize(.small)
                    Text("Asking \(model.communityServer) for a code…")
                    Spacer()
                    Button("Cancel", action: model.cancelSignIn)
                }
            case let .waiting(page, code):
                Text("Open the page, sign in with Discord and enter this code:")
                HStack(spacing: 12) {
                    Text(code).font(.system(.title2, design: .monospaced)).textSelection(.enabled)
                    Button("Copy Code") { model.copyToPasteboard(code) }
                    Spacer()
                    Link(page.absoluteString, destination: page).lineLimit(1).truncationMode(.middle)
                    Button("Open Page") { NSWorkspace.shared.open(page) }
                }
                HStack {
                    ProgressView().controlSize(.small)
                    Text("Waiting for the approval…")
                    Spacer()
                    Button("Cancel", action: model.cancelSignIn)
                }
            }
        }
    }

    @ViewBuilder private var shadnetRows: some View {
        TextField("Server", text: $model.online.shadnetServer, prompt: Text(OnlineSettings.defaultShadnetServer))
        TextField("Account name", text: $model.online.shadnetNpid, prompt: Text("Your shadNet online id"))
            .onChange(of: model.online.shadnetNpid) { model.refreshSecretState() }
        SecureField("Password", text: $model.newShadnetPassword,
                    prompt: Text(model.shadnetPasswordStored ? "Stored in your keychain; type to replace it" : "Kept in your login keychain"))
            .disabled(model.online.shadnetNpid.trimmingCharacters(in: .whitespaces).isEmpty)
        Text("Register at shadnet.shadps4.net. The shadNet protocol is unencrypted, so use a password you use nowhere else; it goes to your login keychain when the settings are saved (Play, or quitting), never into the file or the log.")
            .font(.caption).foregroundStyle(.secondary)
    }
}

private struct FolderRow: View {
    let title: String
    let path: String
    let problem: String?
    let choose: () -> Void
    let reset: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(title)
                Spacer()
                Button("Choose…", action: choose)
                Button("Reset to Default", action: reset)
            }
            Text(path).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                .lineLimit(1).truncationMode(.middle).help(path)
            if let problem {
                Text(problem).font(.caption).foregroundStyle(.red)
            }
        }
    }
}
