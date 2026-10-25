// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// The Mods & Cheats window's model: the reports of scripts/mods.py --report and scripts/patches.py
// --list, the files run.sh reads at launch (mods.json, patches.json, bbport.ini cheats=), and
// the save backup taken before a launch with a new set of mods or cheats.
import Foundation

/// Where run.sh looks (its BB_MODS_DIR, BB_MODS_CONFIG, BB_PATCHES_DIR, BB_PATCHES_CONFIG
/// overrides too), and the launcher's own save backups.
struct ModFiles: Equatable {
    var mods: URL
    var modsConfig: URL
    var patches: URL
    var patchesConfig: URL
    var backups: URL

    init(dataDir: URL, env: [String: String] = [:]) {
        func path(_ key: String, _ fallback: String) -> URL {
            if let v = env[key], !v.isEmpty { return URL(fileURLWithPath: v) }
            return dataDir.appendingPathComponent(fallback)
        }
        mods = path("BB_MODS_DIR", "mods")
        modsConfig = path("BB_MODS_CONFIG", "mods.json")
        patches = path("BB_PATCHES_DIR", "patches")
        patchesConfig = path("BB_PATCHES_CONFIG", "patches.json")
        backups = dataDir.appendingPathComponent("save-backups")
    }
}

// MARK: - mods.py --report

struct ModEntry: Codable, Equatable, Identifiable {
    var name: String
    var enabled: Bool
    var files: Int
    /// Why the folder cannot load (no game folders in it, a refused eboot.bin, ...).
    var error: String?
    var id: String { name }
}

struct ModConflict: Codable, Equatable, Identifiable {
    var path: String
    var mods: [String]
    var winner: String
    var id: String { path }
    /// "dvdroot_ps4/chr/c1000.chrbnd.dcx — A, B: B wins"
    var line: String { "\(path) — \(mods.joined(separator: ", ")): \(winner) wins" }
}

/// Every mod folder in load order, and the files more than one enabled mod replaces.
struct ModReport: Codable, Equatable {
    var mods: [ModEntry]
    var conflicts: [ModConflict]

    static func decode(_ json: String) throws -> ModReport {
        try JSONDecoder().decode(ModReport.self, from: Data(json.utf8))
    }
}

/// mods.json: `order` (load order, later mods win) and `disabled` (folder names).
struct ModsConfig: Codable, Equatable {
    var order: [String] = []
    var disabled: [String] = []

    init(order: [String] = [], disabled: [String] = []) {
        self.order = order
        self.disabled = disabled
    }

    /// The list as the window shows it: its order, the unticked ones disabled.
    init(mods: [ModEntry]) {
        order = mods.map(\.name)
        disabled = mods.filter { !$0.enabled }.map(\.name)
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        order = try c.decodeIfPresent([String].self, forKey: .order) ?? []
        disabled = try c.decodeIfPresent([String].self, forKey: .disabled) ?? []
    }

    /// A missing or unreadable file: everything on, alphabetical (mods.py's default).
    init(contentsOf url: URL) {
        self = (try? Data(contentsOf: url)).flatMap { try? JSONDecoder().decode(ModsConfig.self, from: $0) } ?? ModsConfig()
    }

    var json: Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .withoutEscapingSlashes]
        return ((try? encoder.encode(self)) ?? Data("{}".utf8)) + Data("\n".utf8)
    }

    func write(to url: URL) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try json.write(to: url, options: .atomic)
    }

    /// The mods the next launch loads, without running Python (for the main window's summary and
    /// the save backup): the folders of `modsDir` (not hidden ones, as mods.py) not disabled.
    static func enabledMods(modsDir: URL, config: URL) -> [String] {
        let fm = FileManager.default
        guard let items = try? fm.contentsOfDirectory(at: modsDir, includingPropertiesForKeys: [.isDirectoryKey]) else { return [] }
        let disabled = Set(ModsConfig(contentsOf: config).disabled)
        return items.filter { !$0.lastPathComponent.hasPrefix(".") && (try? $0.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true }
            .map(\.lastPathComponent).filter { !disabled.contains($0) }.sorted()
    }
}

extension Array where Element == ModEntry {
    /// One place up (`by: -1`) or down (`by: 1`); unchanged at the ends.
    func moving(_ name: String, by offset: Int) -> [ModEntry] {
        guard let i = firstIndex(where: { $0.name == name }), indices.contains(i + offset) else { return self }
        var list = self
        list.swapAt(i, i + offset)
        return list
    }
}

// MARK: - patches.py --list

struct CheatInfo: Codable, Equatable, Identifiable {
    /// The patch name (bbport.ini cheats=), not the label.
    var name: String
    var group: String
    var label: String
    /// Two cheats with the same exclusive group cannot be on together.
    var exclusive: String?
    var note: String
    /// Off during online play (patches.py ONLINE_OFF): cheats, gameplay changes.
    var onlineOff = false
    var id: String { name }

    private enum CodingKeys: String, CodingKey {
        case name, group, label, exclusive, note
        case onlineOff = "online_off"
    }

    init(name: String, group: String, label: String, exclusive: String? = nil, note: String, onlineOff: Bool = false) {
        (self.name, self.group, self.label, self.exclusive, self.note, self.onlineOff) = (name, group, label, exclusive, note, onlineOff)
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = try c.decode(String.self, forKey: .name)
        group = try c.decode(String.self, forKey: .group)
        label = try c.decode(String.self, forKey: .label)
        exclusive = try c.decodeIfPresent(String.self, forKey: .exclusive)
        note = try c.decode(String.self, forKey: .note)
        onlineOff = try c.decodeIfPresent(Bool.self, forKey: .onlineOff) ?? false
    }
}

struct ExternalPatch: Codable, Equatable, Identifiable {
    /// "<file name>/<patch name>" (patches.json).
    var key: String
    var file: String
    var name: String?
    var author: String
    var note: String
    var enabled: Bool
    var id: String { key }
}

struct PatchList: Codable, Equatable {
    var cheats: [CheatInfo]
    var external: [ExternalPatch]

    static func decode(_ json: String) throws -> PatchList {
        try JSONDecoder().decode(PatchList.self, from: Data(json.utf8))
    }
}

/// bbport.ini cheats=: patch names joined with ";" (scripts/patches.py cheat_patches).
enum Cheats {
    /// The window's groups, in this order (any other group after them).
    static let groupOrder = ["Cheats", "Gameplay", "Camera", "Interface"]

    static func parse(_ value: String?) -> [String] {
        var seen = Set<String>()
        return (value ?? "").split(separator: ";").map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty && seen.insert($0).inserted }
    }

    /// nil (no key) when none is on.
    static func format(_ names: [String]) -> String? {
        names.isEmpty ? nil : names.joined(separator: ";")
    }

    /// `selected` with `name` on or off; turning one on turns off the others of its exclusive group.
    static func toggle(_ name: String, on: Bool, in selected: [String], cheats: [CheatInfo]) -> [String] {
        var list = selected.filter { $0 != name }
        guard on else { return list }
        if let group = cheats.first(where: { $0.name == name })?.exclusive {
            let rivals = Set(cheats.filter { $0.exclusive == group }.map(\.name))
            list.removeAll { rivals.contains($0) }
        }
        return list + [name]
    }

    static func grouped(_ cheats: [CheatInfo]) -> [(group: String, cheats: [CheatInfo])] {
        let groups = groupOrder + cheats.map(\.group).filter { !groupOrder.contains($0) }.reduce(into: [String]()) {
            if !$0.contains($1) { $0.append($1) }
        }
        return groups.compactMap { group in
            let members = cheats.filter { $0.group == group }
            return members.isEmpty ? nil : (group, members)
        }
    }

    /// The main window's line next to its Mods & Cheats… button.
    /// `online`: online play is on, so mods (mods.py) and cheats (patches.py ONLINE_OFF) are off.
    static func summary(mods: Int, cheats: Int, online: Bool = false) -> String {
        if mods == 0 && cheats == 0 { return "No mods or cheats" }
        var parts: [String] = []
        if mods > 0 { parts.append(mods == 1 ? "1 mod" : "\(mods) mods") }
        if cheats > 0 { parts.append(cheats == 1 ? "1 cheat" : "\(cheats) cheats") }
        var off: String {
            switch (mods > 0, cheats > 0) {
            case (true, true): " (mods and cheats off online)"
            case (true, false): " (mods off online)"
            default: " (cheats off online)"
            }
        }
        return parts.joined(separator: " · ") + " on" + (online ? off : "")
    }
}

/// patches.json: external patches turned on or off by the player (otherwise each file's
/// isEnabled decides).
struct PatchSelection: Codable, Equatable {
    var enabled: [String] = []
    var disabled: [String] = []

    init(enabled: [String] = [], disabled: [String] = []) {
        self.enabled = enabled
        self.disabled = disabled
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        enabled = try c.decodeIfPresent([String].self, forKey: .enabled) ?? []
        disabled = try c.decodeIfPresent([String].self, forKey: .disabled) ?? []
    }

    init(contentsOf url: URL) {
        self = (try? Data(contentsOf: url)).flatMap { try? JSONDecoder().decode(PatchSelection.self, from: $0) } ?? PatchSelection()
    }

    /// Keys of files that are gone stay (the file may come back).
    mutating func set(_ key: String, on: Bool) {
        enabled.removeAll { $0 == key }
        disabled.removeAll { $0 == key }
        if on { enabled.append(key) } else { disabled.append(key) }
        enabled.sort()
        disabled.sort()
    }

    var json: Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .withoutEscapingSlashes]
        return ((try? encoder.encode(self)) ?? Data("{}".utf8)) + Data("\n".utf8)
    }

    func write(to url: URL) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try json.write(to: url, options: .atomic)
    }
}

// MARK: - Adding mods

enum ModImport {
    /// The folder name for a dropped folder or zip "X" (X.zip -> X).
    static func baseName(of source: URL) -> String {
        let name = source.pathExtension.lowercased() == "zip" ? source.deletingPathExtension().lastPathComponent
            : source.lastPathComponent
        let trimmed = name.trimmingCharacters(in: .whitespaces)
        // mods.py skips hidden folders.
        return trimmed.isEmpty || trimmed.hasPrefix(".") ? "Mod" : trimmed
    }

    /// `base`, else "base 2", "base 3"... (case-insensitive: APFS usually is).
    static func uniqueName(_ base: String, existing: [String]) -> String {
        let taken = Set(existing.map { $0.lowercased() })
        if !taken.contains(base.lowercased()) { return base }
        var n = 2
        while taken.contains("\(base) \(n)".lowercased()) { n += 1 }
        return "\(base) \(n)"
    }
}

// MARK: - Save backups

/// The active mods and cheats (sorted): a launch with a set not seen before backs up the save.
struct ModSignature: Codable, Equatable {
    var mods: [String]
    var cheats: [String]

    init(mods: [String], cheats: [String]) {
        self.mods = mods.sorted()
        self.cheats = cheats.sorted()
    }

    var isEmpty: Bool { mods.isEmpty && cheats.isEmpty }
}

enum SaveBackup {
    static let keep = 5
    static let recordName = "last.json"

    static func folderName(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy-MM-dd HH.mm.ss"
        return formatter.string(from: date)
    }

    /// A backup folder's name: the date, maybe " 2"... when two fall in one second.
    static func isBackupName(_ name: String) -> Bool {
        name.range(of: #"^\d{4}-\d{2}-\d{2} \d{2}\.\d{2}\.\d{2}( \d+)?$"#, options: .regularExpression) != nil
    }

    /// Backups beyond the newest `keep` (other names are never deleted).
    static func toDelete(_ names: [String], keep: Int = SaveBackup.keep) -> [String] {
        // (The names sort by date.)
        let backups = names.filter(isBackupName).sorted(by: >)
        return Array(backups.dropFirst(keep))
    }

    /// Nothing on: no backup. Otherwise one per new set.
    static func needed(_ current: ModSignature, last: ModSignature?) -> Bool {
        !current.isEmpty && current != last
    }

    static func lastSignature(backups: URL) -> ModSignature? {
        (try? Data(contentsOf: backups.appendingPathComponent(recordName)))
            .flatMap { try? JSONDecoder().decode(ModSignature.self, from: $0) }
    }

    /// Copies `saveFolder` to <backups>/<date>/savedata (when it has saves), records `signature`
    /// and deletes the oldest backups. Returns the new backup, nil when there was nothing to copy.
    @discardableResult
    static func backUp(saveFolder: URL, backups: URL, signature: ModSignature, now: Date = Date()) throws -> URL? {
        let fm = FileManager.default
        var made: URL?
        if SaveFolder.hasSaves(saveFolder) {
            try fm.createDirectory(at: backups, withIntermediateDirectories: true)
            let existing = (try? fm.contentsOfDirectory(atPath: backups.path)) ?? []
            let folder = backups.appendingPathComponent(ModImport.uniqueName(folderName(now), existing: existing))
            try fm.createDirectory(at: folder, withIntermediateDirectories: true)
            do {
                try fm.copyItem(at: saveFolder, to: folder.appendingPathComponent("savedata"))
            } catch {
                try? fm.removeItem(at: folder) // no half backup that would count as one of the five
                throw error
            }
            made = folder
        }
        // (A new game has no save yet: the first one is made with this set.)
        try fm.createDirectory(at: backups, withIntermediateDirectories: true)
        try JSONEncoder().encode(signature).write(to: backups.appendingPathComponent(recordName), options: .atomic)
        for name in toDelete((try? fm.contentsOfDirectory(atPath: backups.path)) ?? []) {
            try? fm.removeItem(at: backups.appendingPathComponent(name))
        }
        return made
    }
}
