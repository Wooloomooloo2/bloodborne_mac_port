// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import os

/// Writes one game session's output (launcher/Model/SessionLog.swift): each line as it arrives
/// gets its time and severity, in the text log, the JSON log and the system log (unified logging,
/// category = the line's source: Console.app, or
/// `/usr/bin/log stream --info --predicate 'subsystem == "io.github.wooloomooloo2.bloodborne-mac-port"'`).
final class SessionLogWriter: @unchecked Sendable {
    static let subsystem = "io.github.wooloomooloo2.bloodborne-mac-port"

    let text: URL
    let json: URL
    let start: Date
    private let session: SessionLog.Session
    private let textHandle: FileHandle
    private let jsonHandle: FileHandle
    /// One per source (the system log's category: Console filters by it). On `queue`.
    private var loggers: [String: Logger] = [:]
    private let home = NSHomeDirectory()
    // Everything below on `queue`.
    private let queue = DispatchQueue(label: "io.github.wooloomooloo2.bloodborne-mac-port.log")
    private var pending: [UInt8] = []
    private var previous: LogEntry?
    private var closed = false
    private let endOfOutput = DispatchSemaphore(value: 0)
    /// Held until finish(): Process lets go of it when the game's process ends, and a released
    /// pipe closes its read end, so a process still writing (the GPU host exiting) would get SIGPIPE.
    private var pipe: Pipe?

    init(folder: URL, appVersion: String, start: Date = Date()) throws {
        (text, json) = try SessionLog.begin(in: folder, start: start)
        self.start = start
        session = SessionLog.Session(id: text.deletingPathExtension().lastPathComponent, appVersion: appVersion)
        textHandle = try FileHandle(forWritingTo: text)
        jsonHandle = try FileHandle(forWritingTo: json)
    }

    /// The launcher's own lines (session start, exit status).
    func note(_ message: String, severity: LogSeverity = .notice) {
        let entry = LogEntry(time: Date(), severity: severity, source: "launcher", message: "launcher: " + message)
        queue.async { self.write([entry]) }
    }

    /// Reads `pipe` until its end (every process holding its write end has exited).
    func read(_ pipe: Pipe) {
        queue.sync { self.pipe = pipe }
        pipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard let self else { return }
            if data.isEmpty {
                handle.readabilityHandler = nil
                self.endOfOutput.signal()
            } else {
                self.queue.async { self.consume(data) }
            }
        }
    }

    /// After the game's process ended: waits a little for the rest of its output (a child may
    /// still hold the pipe), writes the last partial line and the exit status, closes the files.
    func finish(status: Int32, waitingUpTo timeout: TimeInterval = 2) {
        _ = endOfOutput.wait(timeout: .now() + timeout)
        queue.sync {
            if !pending.isEmpty { flushPending(partial: true) }
            let minutes = Int(Date().timeIntervalSince(start)) / 60, seconds = Int(Date().timeIntervalSince(start)) % 60
            let how = status == 0 ? "ended normally" : "stopped with status \(status)"
            write([LogEntry(time: Date(), severity: status == 0 ? .notice : .error, source: "launcher",
                            message: "launcher: the game \(how) after \(minutes) min \(seconds) s")])
            closed = true
            try? textHandle.close()
            try? jsonHandle.close()
            pipe?.fileHandleForReading.readabilityHandler = nil
            pipe = nil
        }
    }

    private func consume(_ data: Data) {
        pending.append(contentsOf: data)
        flushPending(partial: false)
    }

    /// Writes the complete lines in `pending` (and, at the end, the unterminated rest).
    private func flushPending(partial: Bool) {
        let now = Date()
        var entries: [LogEntry] = []
        var start = 0
        for (i, byte) in pending.enumerated() where byte == 0x0A {
            entries.append(entry(pending[start..<i], time: now))
            start = i + 1
        }
        if partial && start < pending.count {
            entries.append(entry(pending[start...], time: now))
            start = pending.count
        }
        pending.removeFirst(start)
        write(entries)
    }

    private func entry(_ bytes: ArraySlice<UInt8>, time: Date) -> LogEntry {
        var line = String(decoding: bytes, as: UTF8.self)
        if line.hasSuffix("\r") { line.removeLast() }
        let (severity, source) = SessionLog.classify(line, previous: previous)
        let entry = LogEntry(time: time, severity: severity, source: source, message: line)
        previous = entry
        return entry
    }

    private func write(_ entries: [LogEntry]) {
        guard !closed, !entries.isEmpty else { return }
        var textOut = "", jsonOut = ""
        var stamped: (time: Date, local: String, utc: String)? // lines that arrived together share it
        for entry in entries {
            if stamped?.time != entry.time {
                stamped = (entry.time, SessionLog.localTime(entry.time), SessionLog.utcTime(entry.time))
            }
            textOut += SessionLog.textLine(entry, time: stamped!.local) + "\n"
            jsonOut += SessionLog.jsonLine(entry, session: session, time: stamped!.utc) + "\n"
            // The system log gets the home folder as ~ (like crash reports), readable in Console.
            let message = CrashReport.redact(entry.message, home: home)
            let logger = loggers[entry.source] ?? Logger(subsystem: Self.subsystem, category: entry.source)
            loggers[entry.source] = logger
            logger.log(level: Self.osLevel(entry.severity), "\(message, privacy: .public)")
        }
        textHandle.write(Data(textOut.utf8))
        jsonHandle.write(Data(jsonOut.utf8))
    }

    private static func osLevel(_ severity: LogSeverity) -> OSLogType {
        switch severity {
        case .critical: .fault
        case .error: .error
        case .warning, .notice: .default
        case .info: .info
        case .debug: .debug
        }
    }
}
