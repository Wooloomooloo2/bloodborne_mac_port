// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct LauncherView: View {
    /// The window's content size (main.swift).
    static let size = CGSize(width: 920, height: 680)

    @ObservedObject var model: LauncherModel

    var body: some View {
        VStack(spacing: 0) {
            // Two tabs of two columns each so the whole window fits a 13" MacBook Air (1440 x 900
            // points, Dock shown) without scrolling, even with every optional row showing.
            TabView {
                general.tabItem { Text("General") }
                advanced.tabItem { Text("Advanced") }
            }
            .disabled(model.running)
            Divider()
            HStack(spacing: 12) {
                Toggle("Show launcher every time", isOn: $model.config.showLauncher)
                    .disabled(model.running)
                Spacer()
                if let message = model.exitMessage {
                    Text(message).foregroundStyle(.red).lineLimit(2)
                    if model.crashReport != nil {
                        Button("Crash Report…") { model.showingCrashReport = true }
                    } else {
                        Button("Show Log", action: model.showLog)
                    }
                }
                if model.running {
                    Text(model.stopping ? "Stopping…" : "Game running")
                        .foregroundStyle(.secondary)
                    Button(action: model.stop) {
                        Label("Stop", systemImage: "stop.fill").frame(minWidth: 80)
                    }
                    .controlSize(.large)
                    .disabled(model.stopping)
                    .help("Close the running game. Use the game's Exit Game menu first to finish saving.")
                } else {
                    Button(action: model.play) {
                        Text("Play").frame(minWidth: 80)
                    }
                    .keyboardShortcut(.defaultAction)
                    .controlSize(.large)
                    .disabled(!model.canPlay)
                }
            }
            .padding()
        }
        .frame(width: LauncherView.size.width, height: LauncherView.size.height)
        .sheet(isPresented: $model.showingCrashReport) {
            if let report = model.crashReport {
                CrashReportView(report: report, model: model)
            }
        }
    }

    private var general: some View {
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
                    if model.settings.upscaler != .off {
                        Toggle("Sharpening", isOn: $model.settings.sharpen)
                        if model.settings.sharpen {
                            Slider(value: $model.settings.sharpness, in: 0...2) { Text("Strength") }
                        }
                    }
                }
            }
            .formStyle(.grouped)
            Form {
                Section("Effects") {
                    ForEach(GameSettings.visualEffects) { effect in
                        Toggle(effect.label, isOn: effectBinding(effect.key))
                    }
                    Toggle("Show FPS", isOn: $model.settings.showFPS)
                    Toggle("Metal Performance HUD", isOn: $model.settings.metalHUD)
                        .help("Apple's Metal HUD: FPS, GPU time and frame pacing, top right")
                    Toggle("Skip intro videos", isOn: effectBinding("skip_intro"))
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

    /// The in-game menu's Graphics and Advanced options (gpu/shim/bbport_overlay.cpp), same wording.
    private var advanced: some View {
        let upscaler = model.settings.upscaler
        let temporal = upscaler != .off && upscaler != .taa // FSR and MetalFX use the reactive mask
        return HStack(alignment: .top, spacing: 0) {
            Form {
                Section("Detail") {
                    Picker("Model detail", selection: $model.settings.modelDetail) {
                        ForEach(ModelDetail.allCases) { Text($0.label).tag($0) }
                    }
                    Text("Distance at which characters and objects switch to simpler models.")
                        .font(.caption).foregroundStyle(.secondary)
                    Picker("Live resolution changes", selection: $model.settings.liveResolution) {
                        ForEach(LiveResolution.allCases) { Text($0.label).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    Text("On: output resolution and quality change in the game without a restart, but post-processing stays at 1080p (slower). Off: everything renders at the chosen size. Above 1080p it is always on.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Section("Shader compilation") {
                    Picker("New shaders", selection: $model.settings.shaderCompile) {
                        ForEach(ShaderCompile.allCases) { Text($0.label).tag($0) }
                    }
                    Text("Shaders the game hasn't used on this Mac before compile in the background. Shaders seen before load at start either way.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Section("Startup and debugging") {
                    ForEach(GameSettings.debugEffects) { effect in
                        Toggle(effect.label, isOn: effectBinding(effect.key))
                    }
                    Text("Free camera: hold Cross and press L3 (keyboard: Space + Z). Debug menu: left touchpad / Tab; needs DbgFont14h.ccm and DbgFont14h.tpf in dvdroot_ps4/font from Nexus mod #253.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            .formStyle(.grouped)
            Form {
                Section("Upscaler") {
                    if upscaler == .off {
                        Text("Choose an upscaler on the General tab to adjust these options.")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    Group {
                        Toggle("Subpixel jitter", isOn: $model.settings.jitter)
                            .help("Offsets each frame by a fraction of a pixel so the upscaler gathers more detail over time.")
                        Toggle("Character motion vectors", isOn: $model.settings.objectMotion)
                            .help("Precise vectors for animated objects: clothing and weapons break up less in motion.")
                    }
                    .disabled(upscaler == .off)
                }
                Section("Reactive mask") {
                    if !temporal {
                        Text(upscaler == .taa ? "TAA doesn't use a reactive mask. Choose FSR or MetalFX to adjust it."
                                              : "Choose FSR or MetalFX on the General tab to adjust these options.")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    Group {
                        Toggle("Enable mask", isOn: $model.settings.reactive)
                        Text("Marks transparent effects (particles, haze) so the upscaler relies less on previous frames. Less trailing behind effects, but shimmering returns beneath them.")
                            .font(.caption).foregroundStyle(.secondary)
                        Group {
                            Slider(value: $model.settings.reactiveScale, in: 0...4) { Text("Scale") }
                            Slider(value: $model.settings.reactiveThreshold, in: 0...1) { Text("Threshold") }
                            Slider(value: $model.settings.reactiveMax, in: 0...1) { Text("Maximum") }
                        }
                        .disabled(!model.settings.reactive)
                    }
                    .disabled(!temporal)
                }
            }
            .formStyle(.grouped)
        }
    }

    private func effectBinding(_ key: String) -> Binding<Bool> {
        Binding(get: { model.effect(key) }, set: { model.setEffect(key, $0) })
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

/// The game stopped with an error: what happened, and a report ready to paste into an issue.
private struct CrashReportView: View {
    let report: CrashReport
    @ObservedObject var model: LauncherModel
    @Environment(\.dismiss) private var dismiss
    @State private var copied = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Bloodborne stopped").font(.title2.bold())
            Text(report.summary)
            Text("Please report it so it can be fixed: Report on GitHub copies this report and opens a new issue to paste it into. The report has no personal files; your home folder is shown as ~.")
                .font(.callout).foregroundStyle(.secondary)
            ScrollView {
                Text(report.report)
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(8)
            }
            .background(Color(nsColor: .textBackgroundColor))
            .clipShape(RoundedRectangle(cornerRadius: 6))
            HStack {
                Button("Show Log in Finder", action: model.showLog)
                Spacer()
                if copied {
                    Text("Copied").foregroundStyle(.secondary)
                }
                Button("Copy Report") {
                    model.copyCrashReport()
                    copied = true
                }
                Button("Report on GitHub") {
                    model.reportOnGitHub()
                    copied = true
                }
                Button("Close") { dismiss() }
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 760, height: 560)
    }
}
