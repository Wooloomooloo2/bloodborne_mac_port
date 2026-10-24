// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// The save folder: BB_SAVE_DIR for the game (src/runtime_savedata.c), default <data>/user/savedata.
enum SaveFolder {
    static func defaultURL(dataDir: URL) -> URL {
        dataDir.appendingPathComponent("user/savedata")
    }

    /// Any regular file below the folder.
    static func hasSaves(_ url: URL) -> Bool {
        guard let walk = FileManager.default.enumerator(at: url, includingPropertiesForKeys: [.isRegularFileKey]) else {
            return false
        }
        for case let file as URL in walk {
            if (try? file.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true { return true }
        }
        return false
    }

    /// Moving from a folder with saves to another without any: offer to copy them.
    static func shouldOfferCopy(from old: URL, to new: URL) -> Bool {
        old.standardizedFileURL.path != new.standardizedFileURL.path && hasSaves(old) && !hasSaves(new)
    }

    /// Copies the save tree; never overwrites a file in `new`, never touches `old`.
    static func copySaves(from old: URL, to new: URL) throws {
        let fm = FileManager.default
        try fm.createDirectory(at: new, withIntermediateDirectories: true)
        for item in try fm.contentsOfDirectory(at: old, includingPropertiesForKeys: [.isDirectoryKey]) {
            let target = new.appendingPathComponent(item.lastPathComponent)
            let isDirectory = (try? item.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true
            if isDirectory && fm.fileExists(atPath: target.path) {
                try copySaves(from: item, to: target)
            } else if !fm.fileExists(atPath: target.path) {
                try fm.copyItem(at: item, to: target)
            }
        }
    }

    /// nil when the game can write saves there, else why not. `create`: make the folder (when the
    /// player chose it); otherwise a missing folder (an unplugged drive) is a problem.
    static func problem(_ url: URL, create: Bool) -> String? {
        let fm = FileManager.default
        var isDirectory: ObjCBool = false
        if !fm.fileExists(atPath: url.path, isDirectory: &isDirectory) {
            guard create else { return "The save folder \(url.path) is missing (is its drive connected?)." }
            do {
                try fm.createDirectory(at: url, withIntermediateDirectories: true)
            } catch {
                return "Cannot create \(url.path): \(error.localizedDescription)"
            }
        } else if !isDirectory.boolValue {
            return "\(url.path) is a file, not a folder."
        }
        return fm.isWritableFile(atPath: url.path) ? nil : "\(url.path) is not writable."
    }
}
