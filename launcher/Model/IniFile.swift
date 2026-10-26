// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// A `key=value` file (bbport.ini, launch.conf). Comments, blank lines, unknown keys and their
/// order survive a read and write; only the keys that are set change. Parsing follows the game's
/// own reader (gpu/shim/bbport_settings.cpp Load): the key is what precedes the first `=`.
struct IniFile: Equatable {
    enum Line: Equatable {
        case entry(key: String, value: String)
        case other(String)
    }

    private(set) var lines: [Line] = []

    init(text: String = "") {
        // (Swift treats "\r\n" as one Character: normalize before splitting.)
        let unix = text.replacingOccurrences(of: "\r\n", with: "\n")
        for raw in unix.split(separator: "\n", omittingEmptySubsequences: false) {
            let line = String(raw)
            if !line.hasPrefix("#"), let eq = line.firstIndex(of: "=") {
                let key = line[..<eq].trimmingCharacters(in: .whitespaces)
                let value = line[line.index(after: eq)...].trimmingCharacters(in: .whitespaces)
                if !key.isEmpty {
                    lines.append(.entry(key: key, value: value))
                    continue
                }
            }
            lines.append(.other(line))
        }
        // The final newline leaves one empty piece: not a line of the file.
        if case .other("") = lines.last {
            lines.removeLast()
        }
    }

    init(contentsOf url: URL) {
        self.init(text: (try? String(contentsOf: url, encoding: .utf8)) ?? "")
    }

    private static func isEntry(_ line: Line, _ key: String) -> Bool {
        if case let .entry(k, _) = line { return k == key }
        return false
    }

    subscript(key: String) -> String? {
        get {
            for line in lines.reversed() {
                if case let .entry(k, v) = line, k == key { return v }
            }
            return nil
        }
        set {
            guard let value = newValue else {
                lines.removeAll { Self.isEntry($0, key) }
                return
            }
            if let i = lines.lastIndex(where: { Self.isEntry($0, key) }) {
                lines[i] = .entry(key: key, value: value)
            } else {
                lines.append(.entry(key: key, value: value))
            }
        }
    }

    var text: String {
        lines.map { line -> String in
            switch line {
            case let .entry(key, value): return "\(key)=\(value)"
            case let .other(text): return text
            }
        }.joined(separator: "\n") + "\n"
    }

    /// An atomic write makes a new file: the old one's permissions are kept (an owner-only
    /// bbport.ini holding a password stays owner-only).
    func write(to url: URL) throws {
        let fm = FileManager.default
        try fm.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        let permissions = (try? fm.attributesOfItem(atPath: url.path))?[.posixPermissions]
        try text.write(to: url, atomically: true, encoding: .utf8)
        if let permissions {
            try fm.setAttributes([.posixPermissions: permissions], ofItemAtPath: url.path)
        }
    }
}
