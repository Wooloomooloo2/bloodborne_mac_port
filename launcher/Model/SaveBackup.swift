// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// Why a backup was made (info.json in its folder).
enum SaveBackupReason: String, Codable, Equatable {
    /// Back Up Now.
    case manual
    /// The first launch with a set of mods or cheats not seen before.
    case mods
    /// The first launch of a version not seen before (after an update).
    case version
    /// Every launch of an online play beta.
    case beta
    /// The saves a restore replaced.
    case restore

    /// Automatic backups rotate; the player's own stay until deleted.
    var rotates: Bool { self != .manual }

    var label: String {
        switch self {
        case .manual: "Backed up by you"
        case .mods: "Before new mods or cheats"
        case .version: "Before a new version"
        case .beta: "Before a beta launch"
        case .restore: "Replaced by a restore"
        }
    }
}

struct SaveBackupInfo: Codable, Equatable {
    var reason: SaveBackupReason
    /// The app version that made it.
    var version: String
}

/// One backup: <data>/save-backups/<date>/savedata, with info.json beside it (backups from before
/// 0.5.16 have none: they were made for mods or cheats).
struct SaveBackupEntry: Equatable, Identifiable {
    let folder: URL
    let date: Date?
    let info: SaveBackupInfo?
    var id: String { folder.lastPathComponent }
    var name: String { folder.lastPathComponent }
    var reason: SaveBackupReason { info?.reason ?? .mods }
    var saves: URL { folder.appendingPathComponent("savedata") }
}

enum SaveBackup {
    /// Automatic backups kept (the newest); the player's own are never deleted by rotation.
    static let keep = 10
    static let recordName = "last.json"
    static let versionRecordName = "last-version"
    static let infoName = "info.json"

    static func folderName(_ date: Date) -> String { formatter.string(from: date) }

    private static let formatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy-MM-dd HH.mm.ss"
        return formatter
    }()

    /// A backup folder's name: the date, maybe " 2"... when two fall in one second.
    static func isBackupName(_ name: String) -> Bool {
        name.range(of: #"^\d{4}-\d{2}-\d{2} \d{2}\.\d{2}\.\d{2}( \d+)?$"#, options: .regularExpression) != nil
    }

    /// Rotating backups beyond the newest `keep` (other names, and the player's own, never).
    /// `reasons`: a backup's reason by name (absent: made for mods, before 0.5.16).
    static func toDelete(_ names: [String], reasons: [String: SaveBackupReason] = [:], keep: Int = SaveBackup.keep) -> [String] {
        // (The names sort by date.)
        let rotating = names.filter { isBackupName($0) && (reasons[$0] ?? .mods).rotates }.sorted(by: >)
        return Array(rotating.dropFirst(keep))
    }

    /// Nothing on: no backup. Otherwise one per new set.
    static func needed(_ current: ModSignature, last: ModSignature?) -> Bool {
        !current.isEmpty && current != last
    }

    /// The automatic backup a launch needs, if any (one per launch): a beta always, a version not
    /// launched before, a new set of mods or cheats.
    static func launchReason(mods: ModSignature, lastMods: ModSignature?, version: String, lastVersion: String?,
                             isBeta: Bool) -> SaveBackupReason? {
        if isBeta { return .beta }
        if version != lastVersion { return .version }
        if needed(mods, last: lastMods) { return .mods }
        return nil
    }

    static func lastSignature(backups: URL) -> ModSignature? {
        (try? Data(contentsOf: backups.appendingPathComponent(recordName)))
            .flatMap { try? JSONDecoder().decode(ModSignature.self, from: $0) }
    }

    static func lastVersion(backups: URL) -> String? {
        (try? String(contentsOf: backups.appendingPathComponent(versionRecordName), encoding: .utf8))
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }.flatMap { $0.isEmpty ? nil : $0 }
    }

    /// The launch was backed up for (or needed nothing for) this set and version.
    static func record(backups: URL, signature: ModSignature, version: String) throws {
        try FileManager.default.createDirectory(at: backups, withIntermediateDirectories: true)
        try JSONEncoder().encode(signature).write(to: backups.appendingPathComponent(recordName), options: .atomic)
        try Data((version + "\n").utf8).write(to: backups.appendingPathComponent(versionRecordName), options: .atomic)
    }

    /// The backups, newest first.
    static func list(backups: URL) -> [SaveBackupEntry] {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: backups.path)) ?? []
        return names.filter(isBackupName).sorted(by: >).map { name in
            let folder = backups.appendingPathComponent(name)
            let info = (try? Data(contentsOf: folder.appendingPathComponent(infoName)))
                .flatMap { try? JSONDecoder().decode(SaveBackupInfo.self, from: $0) }
            return SaveBackupEntry(folder: folder, date: formatter.date(from: String(name.prefix(19))), info: info)
        }
    }

    /// Copies `saveFolder` to <backups>/<date>/savedata (when it has saves) with its info, then
    /// rotates. Returns the new backup, nil when there was nothing to copy.
    @discardableResult
    static func backUp(saveFolder: URL, backups: URL, reason: SaveBackupReason, version: String,
                       now: Date = Date()) throws -> URL? {
        let fm = FileManager.default
        guard SaveFolder.hasSaves(saveFolder) else { return nil } // (a new game: nothing yet)
        try fm.createDirectory(at: backups, withIntermediateDirectories: true)
        let existing = (try? fm.contentsOfDirectory(atPath: backups.path)) ?? []
        let folder = backups.appendingPathComponent(ModImport.uniqueName(folderName(now), existing: existing))
        try fm.createDirectory(at: folder, withIntermediateDirectories: true)
        do {
            try fm.copyItem(at: saveFolder, to: folder.appendingPathComponent("savedata"))
            try JSONEncoder().encode(SaveBackupInfo(reason: reason, version: version))
                .write(to: folder.appendingPathComponent(infoName), options: .atomic)
        } catch {
            try? fm.removeItem(at: folder) // no half backup
            throw error
        }
        rotate(backups: backups)
        return folder
    }

    static func rotate(backups: URL) {
        let entries = list(backups: backups)
        let reasons = Dictionary(uniqueKeysWithValues: entries.map { ($0.name, $0.reason) })
        for name in toDelete(entries.map(\.name), reasons: reasons) {
            try? FileManager.default.removeItem(at: backups.appendingPathComponent(name))
        }
    }

    /// Puts `entry`'s saves in place of `saveFolder`'s, after backing those up (reason restore).
    /// The current saves are moved aside first and put back if the copy fails.
    static func restore(_ entry: SaveBackupEntry, saveFolder: URL, backups: URL, version: String,
                        now: Date = Date()) throws {
        let fm = FileManager.default
        guard SaveFolder.hasSaves(entry.saves) else {
            throw CocoaError(.fileReadNoSuchFile, userInfo: [NSFilePathErrorKey: entry.saves.path])
        }
        try backUp(saveFolder: saveFolder, backups: backups, reason: .restore, version: version, now: now)
        let aside = saveFolder.deletingLastPathComponent()
            .appendingPathComponent(".\(saveFolder.lastPathComponent)-restoring-\(getpid())")
        try? fm.removeItem(at: aside)
        let hadFolder = fm.fileExists(atPath: saveFolder.path)
        if hadFolder { try fm.moveItem(at: saveFolder, to: aside) }
        do {
            try fm.copyItem(at: entry.saves, to: saveFolder)
        } catch {
            try? fm.removeItem(at: saveFolder)
            if hadFolder { try? fm.moveItem(at: aside, to: saveFolder) }
            throw error
        }
        try? fm.removeItem(at: aside)
    }

    /// Bytes below `folder`.
    static func size(_ folder: URL) -> Int64 {
        guard let walk = FileManager.default.enumerator(at: folder, includingPropertiesForKeys: [.fileSizeKey]) else { return 0 }
        var total: Int64 = 0
        for case let file as URL in walk {
            total += Int64((try? file.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
        }
        return total
    }
}
