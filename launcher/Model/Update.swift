// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// A release of the port on GitHub with its Bloodborne-macOS-<version>.zip.
struct ReleaseUpdate: Equatable {
    var version: ReleaseVersion
    var tag: String
    var title: String
    var notes: String
    var page: URL
    var zip: URL
    var size: Int64
    /// "sha256:<hex>" when GitHub recorded one.
    var digest: String?
    /// A pre-release on GitHub or a test tag (mac-v0.5.2-test1): offered only with "Include test builds".
    var isTestBuild = false
    /// An online play beta (its zip is Bloodborne-OnlineBeta-macOS-<version>.zip, holding
    /// Bloodborne Beta.app): installed next to the regular app, which offers it only with "Online
    /// play beta"; the beta app updates itself to newer betas only.
    var isBeta = false
}

/// Update checks against the repository's GitHub releases (launcher/App/Updater.swift does the
/// network and the install).
enum Updates {
    static let repository = "Wooloomooloo2/bloodborne_mac_port"
    static let bundleIdentifier = "io.github.wooloomooloo2.bloodborne-mac-port"
    /// The beta app (package_app.sh for -beta versions): its own identity, so it sits next to the
    /// regular app.
    static let betaBundleIdentifier = bundleIdentifier + ".beta"
    static let appName = "Bloodborne.app"
    static let betaAppName = "Bloodborne Beta.app"

    static func isBetaApp(bundleIdentifier id: String?) -> Bool { id == betaBundleIdentifier }
    static func appName(beta: Bool) -> String { beta ? betaAppName : appName }
    static let releasesAPI = URL(string: "https://api.github.com/repos/\(repository)/releases?per_page=30")!
    static let repositoryPage = URL(string: "https://github.com/\(repository)")!
    /// Automatic checks: once a day, when the launcher window shows.
    static let interval: TimeInterval = 24 * 60 * 60
    /// The app's zip in a release. Betas use their own name so updaters before 0.5.16, which know
    /// no beta channel, never offer one.
    static let zipPrefix = "Bloodborne-macOS-"
    static let betaZipPrefix = "Bloodborne-OnlineBeta-macOS-"

    static func isDue(last: Date?, now: Date) -> Bool {
        guard let last else { return true }
        return now.timeIntervalSince(last) >= interval || last > now // (clock set back)
    }

    private struct Payload: Decodable {
        struct Asset: Decodable {
            var name: String
            var size: Int64
            var state: String?
            var digest: String?
            var browserDownloadUrl: URL
        }
        var tagName: String
        var name: String?
        var body: String?
        var draft: Bool?
        var prerelease: Bool?
        var htmlUrl: URL
        var assets: [Asset]
    }

    /// The API's releases newest version first, without drafts, tags that are not
    /// mac-v<numbers>, releases without the app's zip and (unless asked for) test builds: GitHub
    /// pre-releases (each release is published as full or pre-release on purpose) and test tags;
    /// and betas (unless asked for).
    static func releases(json: Data, includeTestBuilds: Bool, includeBetas: Bool = false) throws -> [ReleaseUpdate] {
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        let found = try decoder.decode([Payload].self, from: json).compactMap { r -> ReleaseUpdate? in
            guard r.draft != true, r.tagName.hasPrefix("mac-v"), let version = ReleaseVersion(r.tagName) else { return nil }
            let isTestBuild = r.prerelease == true || !version.isPlain
            let zips = r.assets.filter { $0.name.hasSuffix(".zip") && ($0.state ?? "uploaded") == "uploaded" }
            let beta = zips.first { $0.name.hasPrefix(betaZipPrefix) }
            guard let asset = beta ?? zips.first(where: { $0.name.hasPrefix(zipPrefix) }),
                  beta != nil ? includeBetas : includeTestBuilds || !isTestBuild else { return nil }
            let title = r.name.flatMap { $0.isEmpty ? nil : $0 } ?? r.tagName
            return ReleaseUpdate(version: version, tag: r.tagName, title: title, notes: r.body ?? "",
                                 page: r.htmlUrl, zip: asset.browserDownloadUrl, size: asset.size, digest: asset.digest,
                                 isTestBuild: isTestBuild, isBeta: beta != nil)
        }
        // Newest version first; equal versions keep the API's order (newest published first).
        return found.enumerated().sorted { a, b in
            a.element.version.isNewer(than: b.element.version)
                || (!b.element.version.isNewer(than: a.element.version) && a.offset < b.offset)
        }.map(\.element)
    }

    /// The release to offer over `current` (nil: up to date). `skipping`: a version the player
    /// chose to skip; still offered by a check they ask for. The regular app is offered regular
    /// versions; the beta app (`betaApp`) newer betas only.
    static func newest(json: Data, current: ReleaseVersion?, includeTestBuilds: Bool, betaApp: Bool = false,
                       skipping: String? = nil) throws -> ReleaseUpdate? {
        let candidates = try releases(json: json, includeTestBuilds: includeTestBuilds, includeBetas: betaApp)
        guard let best = candidates.first(where: { $0.isBeta == betaApp }) else { return nil }
        if let current, !best.version.isNewer(than: current) { return nil }
        return best.version.text == skipping ? nil : best
    }

    /// The beta to install next to the regular app: the newest, unless the one installed
    /// (`installed`, its version) is as new.
    static func newestBeta(json: Data, installed: ReleaseVersion?) throws -> ReleaseUpdate? {
        guard let best = try releases(json: json, includeTestBuilds: true, includeBetas: true).first(where: \.isBeta)
        else { return nil }
        if let installed, !best.version.isNewer(than: installed) { return nil }
        return best
    }

    /// nil when this copy of the app can replace itself, else why not.
    static func installProblem(layout: Layout, bundle: URL) -> String? {
        if case .checkout = layout {
            return "This Bloodborne.app runs a checkout of the source code: update it with git pull, then build again (bash build_macos.sh, scripts/macos/make_app.sh)."
        }
        if bundle.path.contains("/AppTranslocation/") {
            return "macOS runs this copy from a temporary read-only location. Move Bloodborne.app to your Applications folder, open it from there and try again."
        }
        let folder = bundle.deletingLastPathComponent()
        if !FileManager.default.isWritableFile(atPath: folder.path) {
            return "\(folder.path) is not writable. Download the new version from the release page and replace Bloodborne.app yourself."
        }
        return nil
    }

    /// nil when an unpacked app (its Info.plist) is this app at the release's version.
    /// The longest the relaunch waits for the old copy to exit before opening the new one anyway.
    static let relaunchWait: TimeInterval = 30

    /// After an update: a detached shell that waits until the old copy (`pid`) has exited, then
    /// opens the new one with --updated (its launcher window shows). Opening it while the old copy
    /// still runs showed both for a moment: `open -n` started a second instance beside the one
    /// still quitting, and a plain `open` would only bring the old one forward. `opener`: the tests'.
    static func relaunchCommand(pid: Int32, bundle: URL, opener: String = "/usr/bin/open",
                                wait: TimeInterval = relaunchWait) -> [String] {
        let tries = Int(wait / 0.1)
        let script = """
            i=0
            while /bin/kill -0 "$1" 2>/dev/null && [ "$i" -lt \(tries) ]; do /bin/sleep 0.1; i=$((i+1)); done
            exec "$3" "$2" --args --updated
            """
        return ["/bin/sh", "-c", script, "bloodborne-relaunch", String(pid), bundle.path, opener]
    }

    static func unpackedProblem(info: [String: Any], expected: ReleaseVersion, beta: Bool = false) -> String? {
        guard info["CFBundleIdentifier"] as? String == (beta ? betaBundleIdentifier : bundleIdentifier) else {
            return "The download is not \(appName(beta: beta)) (bundle identifier \(info["CFBundleIdentifier"] as? String ?? "missing"))."
        }
        let version = info["CFBundleVersion"] as? String ?? "missing"
        return version == expected.text ? nil : "The download contains version \(version), not \(expected.text)."
    }
}
