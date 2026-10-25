// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// The Mods & Cheats window (docs/DESIGN_MOD_MANAGER.md): its own window, the launcher window is
// sized for a 13" Air. Mods and patch files are read by run.sh at launch (scripts/mods.py,
// scripts/patches.py); this window only writes mods.json, patches.json and bbport.ini cheats=.
import AppKit
import SwiftUI
import UniformTypeIdentifiers

/// The window: one, opened from the launcher's footer, refreshed whenever it becomes key.
@MainActor
final class ModsWindow: NSObject, NSWindowDelegate {
    private static var shared: ModsWindow?
    private let window: NSWindow
    private let model: ModsModel

    private init(launcher: LauncherModel) {
        model = ModsModel(launcher: launcher)
        window = NSWindow(contentRect: NSRect(origin: .zero, size: ModsView.size),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        super.init()
        window.title = "Mods & Cheats"
        window.contentMinSize = ModsView.size
        window.contentView = NSHostingView(rootView: ModsView(model: model, launcher: launcher))
        window.isReleasedWhenClosed = false
        window.delegate = self
        window.center()
    }

    static func show(launcher: LauncherModel) {
        let controller = shared ?? ModsWindow(launcher: launcher)
        shared = controller
        controller.window.makeKeyAndOrderFront(nil)
    }

    /// The game started: its window takes over.
    static func hide() { shared?.window.orderOut(nil) }

    func windowDidBecomeKey(_ notification: Notification) { model.refresh() }
}

/// The window's state: the scripts' reports and the actions that write the selection files.
@MainActor
final class ModsModel: ObservableObject {
    let launcher: LauncherModel
    @Published private(set) var mods: [ModEntry] = []
    @Published private(set) var conflicts: [ModConflict] = []
    @Published private(set) var modsProblem: String?
    @Published private(set) var cheats: [CheatInfo] = []
    @Published private(set) var external: [ExternalPatch] = []
    @Published private(set) var patchesProblem: String?
    @Published private(set) var importing = false
    @Published var selection: String?
    /// Results of an older refresh that ends late are dropped (its `then` runs with the newest).
    private var generation = 0
    private var waiting: [() -> Void] = []

    init(launcher: LauncherModel) { self.launcher = launcher }

    private var files: ModFiles { launcher.modFiles }

    private struct ScriptFailure: Error { let message: String }

    /// One script's JSON; any failure as a line for the window.
    nonisolated private static func runScript<T>(_ command: [String], _ decode: (String) throws -> T) -> Result<T, ScriptFailure> {
        let script = URL(fileURLWithPath: command.last { $0.hasSuffix(".py") } ?? "script").lastPathComponent
        let result = GameRunner.runSeparately(command)
        guard result.status == 0 else {
            let reason = result.errors.trimmingCharacters(in: .whitespacesAndNewlines)
            return .failure(ScriptFailure(message: reason.isEmpty ? "\(script) failed (status \(result.status))." : reason))
        }
        do {
            return .success(try decode(result.output))
        } catch {
            return .failure(ScriptFailure(message: "\(script) printed something unexpected: \(error.localizedDescription)"))
        }
    }

    /// Both reports again (in the background); `then` runs once they are shown.
    func refresh(then: (() -> Void)? = nil) {
        generation += 1
        if let then { waiting.append(then) }
        let (current, files, layout) = (generation, files, launcher.layout)
        let modsCommand = layout.pythonCommand(script: "mods.py")
            + ["--report", "--mods-dir", files.mods.path, "--config", files.modsConfig.path]
        let patchesCommand = layout.pythonCommand(script: "patches.py")
            + ["--list", "--patches-dir", files.patches.path, "--patches-config", files.patchesConfig.path]
        Task.detached {
            let report = Self.runScript(modsCommand, ModReport.decode)
            let patches = Self.runScript(patchesCommand, PatchList.decode)
            await MainActor.run {
                guard current == self.generation else { return }
                switch report {
                case let .success(r):
                    self.mods = r.mods
                    self.conflicts = r.conflicts
                    self.modsProblem = nil
                    if let s = self.selection, !r.mods.contains(where: { $0.name == s }) { self.selection = nil }
                case let .failure(f):
                    self.modsProblem = f.message
                }
                switch patches {
                case let .success(p):
                    self.cheats = p.cheats
                    self.external = p.external
                    self.patchesProblem = nil
                case let .failure(f):
                    self.patchesProblem = f.message
                }
                self.launcher.refreshEnabledMods()
                let done = self.waiting
                self.waiting = []
                done.forEach { $0() }
            }
        }
    }

    // MARK: Mods

    /// Every change: mods.json in the list's order, then the report again (conflicts, errors).
    private func write(_ list: [ModEntry], then: (() -> Void)? = nil) {
        mods = list
        do {
            try ModsConfig(mods: list).write(to: files.modsConfig)
        } catch {
            modsProblem = "Could not save \(files.modsConfig.lastPathComponent): \(error.localizedDescription)"
        }
        refresh(then: then)
    }

    func setEnabled(_ name: String, _ on: Bool) {
        write(mods.map { var m = $0; if m.name == name { m.enabled = on }; return m })
    }

    func move(_ name: String, by offset: Int) { write(mods.moving(name, by: offset)) }

    func move(fromOffsets: IndexSet, toOffset: Int) {
        var list = mods
        list.move(fromOffsets: fromOffsets, toOffset: toOffset)
        write(list)
    }

    func canMove(by offset: Int) -> Bool {
        guard let s = selection, let i = mods.firstIndex(where: { $0.name == s }) else { return false }
        return mods.indices.contains(i + offset)
    }

    func folder(_ name: String) -> URL { files.mods.appendingPathComponent(name) }

    func removeSelected() {
        guard let name = selection else { return }
        let alert = NSAlert()
        alert.messageText = "Move “\(name)” to the Trash?"
        alert.informativeText = "The mod's folder goes to the Trash; you can put it back from there."
        alert.addButton(withTitle: "Move to Trash")
        alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        trash(name)
    }

    private func trash(_ name: String) {
        do {
            try FileManager.default.trashItem(at: folder(name), resultingItemURL: nil)
        } catch {
            modsProblem = "Could not move “\(name)” to the Trash: \(error.localizedDescription)"
            return
        }
        selection = nil
        write(mods.filter { $0.name != name })
    }

    func showInFinder() {
        guard let name = selection else { return }
        NSWorkspace.shared.activateFileViewerSelecting([folder(name)])
    }

    func openModsFolder() { open(files.mods) }

    private func open(_ folder: URL) {
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        NSWorkspace.shared.open(folder)
    }

    func addMods() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = true
        panel.allowsMultipleSelection = true
        panel.allowedContentTypes = [.folder, .zip]
        panel.message = "Choose a mod: its folder or a .zip. It is copied into the mods folder."
        panel.prompt = "Add"
        guard panel.runModal() == .OK, !panel.urls.isEmpty else { return }
        let (sources, modsDir) = (panel.urls, files.mods)
        importing = true
        Task.detached {
            var added: [String] = [], problems: [String] = []
            for source in sources {
                do {
                    added.append(try Self.importMod(source, into: modsDir))
                } catch {
                    problems.append("“\(source.lastPathComponent)”: \(error.localizedDescription)")
                }
            }
            let (newMods, failed) = (added, problems)
            await MainActor.run {
                self.importing = false
                // New mods load last (they win), as mods.py does with folders it has not seen.
                let list = self.mods.filter { !newMods.contains($0.name) }
                    + newMods.map { ModEntry(name: $0, enabled: true, files: 0, error: nil) }
                self.write(list) { self.reviewImported(newMods, problems: failed) }
            }
        }
    }

    /// The layout check ran with the report: a folder mods.py cannot load is offered to the Trash.
    private func reviewImported(_ added: [String], problems: [String]) {
        if !problems.isEmpty {
            modsProblem = "Not added: " + problems.joined(separator: "; ")
        }
        for name in added {
            guard let error = mods.first(where: { $0.name == name })?.error else { continue }
            let alert = NSAlert()
            alert.messageText = "“\(name)” can't be loaded as a mod"
            alert.informativeText = error + "\n\nA mod holds the game's folders (dvdroot_ps4, or chr, parts, map... directly). Mods that replace eboot.bin, sce_module or sce_sys are refused."
            alert.addButton(withTitle: "Move to Trash")
            alert.addButton(withTitle: "Keep")
            if alert.runModal() == .alertFirstButtonReturn { trash(name) }
        }
        if let last = added.last, mods.contains(where: { $0.name == last }) { selection = last }
    }

    private struct ImportFailure: LocalizedError {
        let errorDescription: String?
    }

    /// Copies a folder, or unpacks a zip, into mods/<name> (a free name); returns the name.
    nonisolated private static func importMod(_ source: URL, into modsDir: URL) throws -> String {
        let fm = FileManager.default
        try fm.createDirectory(at: modsDir, withIntermediateDirectories: true)
        let src = source.resolvingSymlinksInPath().standardizedFileURL.path
        let dir = modsDir.resolvingSymlinksInPath().standardizedFileURL.path
        if src == dir || src.hasPrefix(dir + "/") || dir.hasPrefix(src + "/") {
            throw ImportFailure(errorDescription: "it is the mods folder or inside it.")
        }
        let name = ModImport.uniqueName(ModImport.baseName(of: source), existing: try fm.contentsOfDirectory(atPath: modsDir.path))
        let target = modsDir.appendingPathComponent(name)
        guard source.pathExtension.lowercased() == "zip" else {
            try fm.copyItem(at: source, to: target)
            return name
        }
        // Unpacked beside the mods folder (same volume), then moved in whole: no half mod in it.
        let temp = (try? fm.url(for: .itemReplacementDirectory, in: .userDomainMask, appropriateFor: modsDir, create: true))
            ?? fm.temporaryDirectory.appendingPathComponent("bb-mod-\(UUID().uuidString)")
        defer { try? fm.removeItem(at: temp) }
        let unpacked = temp.appendingPathComponent(name)
        try fm.createDirectory(at: unpacked, withIntermediateDirectories: true)
        let result = GameRunner.run(["/usr/bin/ditto", "-x", "-k", source.path, unpacked.path])
        guard result.status == 0 else {
            let reason = result.output.trimmingCharacters(in: .whitespacesAndNewlines)
            throw ImportFailure(errorDescription: "the zip could not be unpacked" + (reason.isEmpty ? "." : ": \(reason)"))
        }
        // (Finder's resource forks: they would make a second wrapper folder.)
        try? fm.removeItem(at: unpacked.appendingPathComponent("__MACOSX"))
        try fm.moveItem(at: unpacked, to: target)
        return name
    }

    // MARK: Cheats and patch files

    func isOn(_ cheat: CheatInfo) -> Bool { launcher.settings.cheats.contains(cheat.name) }

    func setCheat(_ cheat: CheatInfo, _ on: Bool) {
        launcher.setCheat(cheat.name, on: on, cheats: cheats)
    }

    func setExternal(_ key: String, _ on: Bool) {
        var selection = PatchSelection(contentsOf: files.patchesConfig)
        selection.set(key, on: on)
        do {
            try selection.write(to: files.patchesConfig)
            external = external.map { var p = $0; if p.key == key { p.enabled = on }; return p }
        } catch {
            patchesProblem = "Could not save \(files.patchesConfig.lastPathComponent): \(error.localizedDescription)"
        }
        refresh()
    }

    func openPatchesFolder() { open(files.patches) }
}

struct ModsView: View {
    static let size = CGSize(width: 720, height: 560)

    @ObservedObject var model: ModsModel
    @ObservedObject var launcher: LauncherModel

    // Not a TabView (LauncherView: #47, a SwiftUI accessibility loop on macOS 26.0.1).
    private enum Page: Hashable { case mods, cheats, patches }
    @State private var page = Page.mods

    var body: some View {
        VStack(spacing: 0) {
            Picker("", selection: $page) {
                Text("Mods").tag(Page.mods)
                Text("Cheats & Tweaks").tag(Page.cheats)
                Text("Patch Files").tag(Page.patches)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .fixedSize()
            .padding(.top, 10)
            Group {
                switch page {
                case .mods: modsPage
                case .cheats: cheatsPage
                case .patches: patchesPage
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .frame(minWidth: ModsView.size.width, minHeight: ModsView.size.height)
    }

    private func problem(_ text: String?) -> some View {
        Group {
            if let text {
                Text(text).font(.caption).foregroundStyle(.red).textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }

    // MARK: Mods

    private var modsPage: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Mods are merged when the game starts, without changing the game files. Mods lower in the list load later and win when two replace the same file. Drag to reorder.")
                .font(.callout).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            problem(model.modsProblem)
            if launcher.online.online {
                Text("Online play is on: mods are not loaded, so everyone in a world plays the same game. They come back when online play is turned off.")
                    .font(.caption).foregroundStyle(.orange)
            }
            List(selection: $model.selection) {
                ForEach(model.mods) { mod in
                    ModRow(mod: mod, enabled: Binding(get: { mod.enabled && !launcher.online.online }, set: { model.setEnabled(mod.name, $0) }),
                           online: launcher.online.online)
                        .tag(mod.name)
                        .contextMenu {
                            Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([model.folder(mod.name)]) }
                        }
                }
                .onMove { model.move(fromOffsets: $0, toOffset: $1) }
            }
            .listStyle(.bordered(alternatesRowBackgrounds: true))
            .overlay {
                if model.mods.isEmpty {
                    Text("No mods yet. Add a mod's folder or .zip, or put mod folders in the mods folder.")
                        .foregroundStyle(.secondary).multilineTextAlignment(.center).padding()
                }
            }
            HStack {
                Button("Add Mod…", action: model.addMods).disabled(model.importing)
                if model.importing { ProgressView().controlSize(.small) }
                Button("Remove…", action: model.removeSelected).disabled(model.selection == nil)
                Button("Show in Finder", action: model.showInFinder).disabled(model.selection == nil)
                Button { if let s = model.selection { model.move(s, by: -1) } } label: { Image(systemName: "chevron.up") }
                    .help("Load earlier").disabled(!model.canMove(by: -1))
                Button { if let s = model.selection { model.move(s, by: 1) } } label: { Image(systemName: "chevron.down") }
                    .help("Load later (wins conflicts)").disabled(!model.canMove(by: 1))
                Spacer()
                Button("Open Mods Folder", action: model.openModsFolder)
            }
            if !model.conflicts.isEmpty {
                Text("Conflicts").font(.headline)
                ScrollView {
                    VStack(alignment: .leading, spacing: 2) {
                        ForEach(model.conflicts) { conflict in
                            Text(conflict.line).font(.caption).textSelection(.enabled)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                    }
                }
                .frame(maxHeight: 110)
            }
        }
        .padding()
    }

    // MARK: Cheats

    private var cheatsPage: some View {
        Form {
            if launcher.running {
                Text("The game is running: these apply at the next start, change them once it has ended.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            if let problem = model.patchesProblem {
                Text(problem).font(.caption).foregroundStyle(.red).textSelection(.enabled)
            }
            if launcher.online.online {
                Text("Online play is on: cheats and gameplay changes are off, so everyone in a world plays the same game. They come back when online play is turned off.")
                    .font(.caption).foregroundStyle(.orange)
            }
            ForEach(Cheats.grouped(model.cheats), id: \.group) { entry in
                Section(entry.group) {
                    if entry.group == "Cheats" {
                        Text("Cheats change the game's code. Your save is backed up before the first launch with a new set of mods or cheats.")
                            .font(.caption).foregroundStyle(.orange)
                    }
                    ForEach(entry.cheats) { cheat in
                        let offline = launcher.online.online && cheat.onlineOff
                        Toggle(isOn: Binding(get: { model.isOn(cheat) && !offline }, set: { model.setCheat(cheat, $0) })) {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(cheat.label)
                                if !cheat.note.isEmpty {
                                    Text(cheat.note).font(.caption).foregroundStyle(.secondary).lineLimit(3)
                                }
                            }
                        }
                        .help(offline ? "Off during online play" : cheat.note)
                        .disabled(launcher.running || offline)
                    }
                }
            }
            Section {
                Text("Applies at the next start. Options that can't be combined turn each other off.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
    }

    // MARK: Patch files

    private var patchesPage: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Patches from other sources' patch files, applied at the next start. Only patches for app version 01.09 (the version this port runs) are listed.")
                .font(.callout).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            problem(model.patchesProblem)
            if launcher.online.online {
                Text("Online play is on: patch files are not applied. They come back when online play is turned off.")
                    .font(.caption).foregroundStyle(.orange)
            }
            if model.external.isEmpty {
                VStack(spacing: 8) {
                    Text("No patch files").font(.headline)
                    Text("Put shadPS4 or GoldHEN patch files (.xml) for Bloodborne, app version 01.09, in the patches folder: their patches show up here to turn on or off. The port's own patches (graphics, frame rate and the cheats) are built in.")
                        .foregroundStyle(.secondary).multilineTextAlignment(.center)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .padding()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                List {
                    ForEach(model.external) { patch in
                        Toggle(isOn: Binding(get: { patch.enabled && !launcher.online.online }, set: { model.setExternal(patch.key, $0) })) {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(patch.name ?? patch.key)
                                Text(patch.author.isEmpty ? patch.file : "\(patch.file) · \(patch.author)")
                                    .font(.caption).foregroundStyle(.secondary)
                                if !patch.note.isEmpty {
                                    Text(patch.note).font(.caption).foregroundStyle(.secondary).lineLimit(3)
                                }
                            }
                        }
                        .help(patch.note)
                        .disabled(launcher.online.online)
                    }
                }
                .listStyle(.bordered(alternatesRowBackgrounds: true))
            }
            HStack {
                Spacer()
                Button("Open Patches Folder", action: model.openPatchesFolder)
            }
        }
        .padding()
    }
}

private struct ModRow: View {
    let mod: ModEntry
    @Binding var enabled: Bool
    /// Online play: mods are off (scripts/mods.py), the switch can't be turned on.
    var online = false

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Toggle("", isOn: $enabled).labelsHidden().disabled(online).help(online ? "Off during online play" : "")
            VStack(alignment: .leading, spacing: 2) {
                Text(mod.name)
                if let error = mod.error {
                    Text(error).font(.caption).foregroundStyle(.red).lineLimit(2).help(error)
                } else {
                    Text(mod.files == 1 ? "1 file" : "\(mod.files) files").font(.caption).foregroundStyle(.secondary)
                }
            }
        }
        .padding(.vertical, 2)
    }
}
