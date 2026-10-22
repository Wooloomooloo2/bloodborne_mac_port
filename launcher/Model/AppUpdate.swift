// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// Release information fetched from the GitHub API.
public struct AppUpdate: Equatable, Sendable {
    public let version: String
    public let title: String
    public let downloadURL: URL
    public let releaseURL: URL
    public let notes: String

    public init(version: String, title: String, downloadURL: URL, releaseURL: URL, notes: String) {
        self.version = version
        self.title = title
        self.downloadURL = downloadURL
        self.releaseURL = releaseURL
        self.notes = notes
    }

    /// Compares two semver-like version strings (e.g., "0.5.4.1" > "0.5.3").
    public static func isNewer(remote: String, local: String) -> Bool {
        let cleanRemote = cleanVersion(remote)
        let cleanLocal = cleanVersion(local)
        if cleanLocal.isEmpty || cleanLocal == "development build" || cleanLocal == "unknown" {
            return false
        }
        let rParts = cleanRemote.split(separator: ".").compactMap { Int($0) }
        let lParts = cleanLocal.split(separator: ".").compactMap { Int($0) }
        guard !rParts.isEmpty && !lParts.isEmpty else { return false }
        for i in 0..<max(rParts.count, lParts.count) {
            let r = i < rParts.count ? rParts[i] : 0
            let l = i < lParts.count ? lParts[i] : 0
            if r > l { return true }
            if r < l { return false }
        }
        return false
    }

    public static func cleanVersion(_ v: String) -> String {
        var s = v.trimmingCharacters(in: .whitespacesAndNewlines)
        if s.hasPrefix("mac-v") { s.removeFirst(5) }
        else if s.hasPrefix("v") { s.removeFirst(1) }
        return s
    }
}
