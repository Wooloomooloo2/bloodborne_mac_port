// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import CryptoKit
import Foundation

/// Checks the GitHub releases (launcher/Model/Update.swift), downloads the new app and replaces
/// this one with it: unpacked next to the installed app (same volume), checked (identifier,
/// version, signature), swapped in, relaunched.
@MainActor
final class Updater: ObservableObject {
    enum Status: Equatable {
        case idle
        case checking
        case upToDate
        case available(ReleaseUpdate)
        case downloading(ReleaseUpdate, Double)
        case installing(ReleaseUpdate)
        case failed(String)
    }

    @Published private(set) var status: Status = .idle
    /// The update window (LauncherView's sheet).
    @Published var showing = false

    let current: ReleaseVersion?
    /// The version as Info.plist has it ("0.5.2", "0.5.2-5-g0b50589", "dev").
    let currentText: String

    init(info: [String: Any]) {
        currentText = info["CFBundleVersion"] as? String ?? info["CFBundleShortVersionString"] as? String ?? "dev"
        current = ReleaseVersion(currentText)
    }

    var available: ReleaseUpdate? {
        switch status {
        case let .available(u), let .downloading(u, _), let .installing(u): u
        default: nil
        }
    }

    var busy: Bool {
        switch status {
        case .checking, .downloading, .installing: true
        default: false
        }
    }

    /// `manual`: the player asked (menu): skipped versions count, and the window shows the result
    /// whatever it is. Automatic checks stay silent unless there is something to install.
    func check(includeTestBuilds: Bool, skipping: String?, manual: Bool) async {
        guard !busy else { return }
        status = .checking
        if manual { showing = true }
        do {
            var request = URLRequest(url: Updates.releasesAPI, timeoutInterval: 20)
            request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
            request.setValue("Bloodborne-macOS/\(currentText)", forHTTPHeaderField: "User-Agent")
            let (data, response) = try await URLSession.shared.data(for: request)
            if let http = response as? HTTPURLResponse, http.statusCode != 200 {
                throw UpdateError("GitHub answered \(http.statusCode)\(http.statusCode == 403 ? " (rate limit: try again later)" : "").")
            }
            let found = try Updates.newest(json: data, current: current, includeTestBuilds: includeTestBuilds,
                                           skipping: manual ? nil : skipping)
            if let found {
                status = .available(found)
                showing = true
            } else {
                status = .upToDate
            }
        } catch {
            status = manual ? .failed("Could not check for updates: \(error.localizedDescription)") : .idle
        }
    }

    /// Downloads and installs `update`, then relaunches the new app. Returns only on failure.
    func install(_ update: ReleaseUpdate, layout: Layout, bundle: URL) async {
        if let problem = Updates.installProblem(layout: layout, bundle: bundle) {
            status = .failed(problem)
            return
        }
        do {
            try await replace(bundle, with: update)
        } catch {
            status = .failed("The update was not installed: \(error.localizedDescription)")
            return
        }
        // The new copy shows its launcher window (--updated) rather than going straight to the game.
        GameRunner.run(["/usr/bin/open", "-n", bundle.path, "--args", "--updated"])
        NSApp.terminate(nil)
    }

    /// Downloads `update`, checks it and puts it in place of the app at `bundle`.
    func replace(_ bundle: URL, with update: ReleaseUpdate) async throws {
        let fm = FileManager.default
        // Unpacked beside the app: replaceItemAt swaps within one volume.
        let staging = bundle.deletingLastPathComponent().appendingPathComponent(".Bloodborne-update-\(getpid())")
        defer { try? fm.removeItem(at: staging) }
        status = .downloading(update, 0)
        let zip = try await FileDownload.run(update.zip) { [weak self] fraction in
            self?.status = .downloading(update, fraction)
        }
        defer { try? fm.removeItem(at: zip) }
        status = .installing(update)
        try await Task.detached { try Self.verify(zip, update) }.value
        try? fm.removeItem(at: staging)
        try fm.createDirectory(at: staging, withIntermediateDirectories: true)
        let newApp = staging.appendingPathComponent("Bloodborne.app")
        try await Task.detached {
            try Self.tool(["/usr/bin/ditto", "-x", "-k", zip.path, staging.path], "Unpacking the update failed")
            guard let info = NSDictionary(contentsOf: newApp.appendingPathComponent("Contents/Info.plist")) as? [String: Any] else {
                throw UpdateError("The download contains no Bloodborne.app.")
            }
            if let problem = Updates.unpackedProblem(info: info, expected: update.version) { throw UpdateError(problem) }
            try Self.tool(["/usr/bin/codesign", "--verify", "--deep", "--strict", newApp.path],
                          "The downloaded app's signature is broken")
            // (URLSession downloads are not quarantined; clear it anyway so the app opens.)
            GameRunner.run(["/usr/bin/xattr", "-dr", "com.apple.quarantine", newApp.path])
        }.value
        _ = try fm.replaceItemAt(bundle, withItemAt: newApp)
    }

    nonisolated private static func verify(_ zip: URL, _ update: ReleaseUpdate) throws {
        let size = (try? zip.resourceValues(forKeys: [.fileSizeKey]).fileSize).map(Int64.init) ?? -1
        guard update.size <= 0 || size == update.size else {
            throw UpdateError("The download is incomplete (\(size) of \(update.size) bytes).")
        }
        guard let digest = update.digest, digest.hasPrefix("sha256:") else { return }
        let handle = try FileHandle(forReadingFrom: zip)
        defer { try? handle.close() }
        var hash = SHA256()
        while let chunk = try handle.read(upToCount: 1 << 20), !chunk.isEmpty { hash.update(data: chunk) }
        let hex = hash.finalize().map { String(format: "%02x", $0) }.joined()
        guard hex == digest.dropFirst("sha256:".count).lowercased() else {
            throw UpdateError("The download's checksum does not match the release.")
        }
    }

    nonisolated private static func tool(_ command: [String], _ what: String) throws {
        let result = GameRunner.run(command)
        guard result.status == 0 else {
            throw UpdateError("\(what): \(result.output.trimmingCharacters(in: .whitespacesAndNewlines))")
        }
    }

    func dismiss() {
        showing = false
        if case .failed = status { status = .idle }
        if case .upToDate = status { status = .idle }
    }
}

struct UpdateError: LocalizedError {
    let errorDescription: String?
    init(_ message: String) { errorDescription = message }
}

/// One download with progress (the async URLSession calls report none for downloads).
private final class FileDownload: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {
    private var continuation: CheckedContinuation<URL, Error>?
    private let progress: @MainActor (Double) -> Void
    private var lastReported = -1.0

    private init(progress: @escaping @MainActor (Double) -> Void) { self.progress = progress }

    /// The file, moved to a temporary location the caller removes.
    static func run(_ url: URL, progress: @escaping @MainActor (Double) -> Void) async throws -> URL {
        let download = FileDownload(progress: progress)
        let session = URLSession(configuration: .ephemeral, delegate: download, delegateQueue: nil)
        defer { session.finishTasksAndInvalidate() }
        return try await withCheckedThrowingContinuation { continuation in
            download.continuation = continuation
            session.downloadTask(with: url).resume()
        }
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData _: Int64,
                    totalBytesWritten written: Int64, totalBytesExpectedToWrite total: Int64) {
        guard total > 0 else { return }
        let fraction = Double(written) / Double(total)
        guard fraction - lastReported >= 0.01 || fraction >= 1 else { return }
        lastReported = fraction
        let progress = self.progress
        Task { @MainActor in progress(fraction) }
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        // The file at `location` is deleted when this returns.
        if let http = downloadTask.response as? HTTPURLResponse, http.statusCode != 200 {
            finish(.failure(UpdateError("The download failed (HTTP \(http.statusCode)).")))
            return
        }
        let kept = FileManager.default.temporaryDirectory.appendingPathComponent("Bloodborne-update-\(UUID().uuidString).zip")
        do {
            try FileManager.default.moveItem(at: location, to: kept)
            finish(.success(kept))
        } catch {
            finish(.failure(error))
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        if let error { finish(.failure(error)) }
    }

    private func finish(_ result: Result<URL, Error>) {
        continuation?.resume(with: result)
        continuation = nil
    }
}
