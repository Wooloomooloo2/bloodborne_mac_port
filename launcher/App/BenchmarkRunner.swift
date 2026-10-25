// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// The benchmark's launcher side (docs/DESIGN_BENCHMARK.md): runs the passes, samples the system,
// writes the report. The numbers come from launcher/Model/Benchmark.swift.
import AppKit
import Darwin
import Foundation
import IOKit
import IOKit.ps

/// 1 Hz samples of the game's two processes and the system while a pass runs. Reads only: no
/// signals or suspends reach the game.
final class SystemSampler: @unchecked Sendable {
    static let probeName = "bb-probe"
    static let hostNames: Set<String> = ["bbgpu-host", "bbgpu-host-macos27"]

    private let pass: BenchmarkPass
    private let queue = DispatchQueue(label: "bloodborne.benchmark.sampler", qos: .utility)
    private var timer: DispatchSourceTimer?
    private var samples: [SystemSample] = []
    /// Per pid: CPU time and wall clock (both ns) at the previous sample, for CPU %.
    private var previous: [pid_t: (cpu: UInt64, wall: UInt64)] = [:]
    private static let host = mach_host_self()
    private static let timebase: mach_timebase_info_data_t = {
        var info = mach_timebase_info_data_t()
        mach_timebase_info(&info)
        return info
    }()

    init(pass: BenchmarkPass) { self.pass = pass }

    func start() {
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now(), repeating: .seconds(1), leeway: .milliseconds(50))
        timer.setEventHandler { [weak self] in self?.sample() }
        self.timer = timer
        timer.resume()
    }

    /// Stops sampling; the samples taken.
    func stop() -> [SystemSample] {
        queue.sync {
            timer?.cancel()
            timer = nil
            return samples
        }
    }

    /// Mach ticks (rusage times, mach_absolute_time) to ns.
    private static func ns(_ ticks: UInt64) -> UInt64 {
        ticks * UInt64(timebase.numer) / UInt64(timebase.denom)
    }

    private func sample() {
        let pids = Self.pids(named: Self.hostNames.union([Self.probeName]))
        let host = Self.hostNames.compactMap { pids[$0] }.first
        samples.append(SystemSample(pass: pass, time: Date().timeIntervalSince1970,
                                    probe: pids[Self.probeName].flatMap(process), host: host.flatMap(process),
                                    swapUsedMB: Self.swapUsedMB(), swapIns: Self.swapIns(), pressure: Self.pressure(),
                                    thermal: ProcessInfo.processInfo.thermalState.rawValue))
    }

    private func process(_ pid: pid_t) -> ProcessSample? {
        var info = rusage_info_v4()
        let status = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: rusage_info_t?.self, capacity: 1) { proc_pid_rusage(pid, RUSAGE_INFO_V4, $0) }
        }
        guard status == 0 else { return nil }
        let cpu = Self.ns(info.ri_user_time + info.ri_system_time)
        let wall = Self.ns(mach_absolute_time())
        var percent: Double?
        if let last = previous[pid], wall > last.wall, cpu >= last.cpu {
            percent = 100 * Double(cpu - last.cpu) / Double(wall - last.wall)
        }
        previous[pid] = (cpu, wall)
        return ProcessSample(cpu: percent, footprintMB: Double(info.ri_phys_footprint) / 1_048_576)
    }

    /// The first process with each name (only one game runs at a time).
    private static func pids(named names: Set<String>) -> [String: pid_t] {
        let size = proc_listpids(UInt32(PROC_ALL_PIDS), 0, nil, 0)
        guard size > 0 else { return [:] }
        var all = [pid_t](repeating: 0, count: Int(size) / MemoryLayout<pid_t>.size + 64)
        let bytes = proc_listpids(UInt32(PROC_ALL_PIDS), 0, &all, Int32(all.count * MemoryLayout<pid_t>.size))
        var found: [String: pid_t] = [:]
        for pid in all.prefix(Int(bytes) / MemoryLayout<pid_t>.size) where pid > 0 {
            var name = [CChar](repeating: 0, count: 64)
            guard proc_name(pid, &name, UInt32(name.count)) > 0 else { continue }
            let s = String(cString: name)
            if names.contains(s) && found[s] == nil { found[s] = pid }
        }
        return found
    }

    private static func swapUsedMB() -> Double {
        var usage = xsw_usage()
        var size = MemoryLayout<xsw_usage>.size
        guard sysctlbyname("vm.swapusage", &usage, &size, nil, 0) == 0 else { return 0 }
        return Double(usage.xsu_used) / 1_048_576
    }

    private static func swapIns() -> UInt64 {
        var stats = vm_statistics64()
        var count = mach_msg_type_number_t(MemoryLayout<vm_statistics64>.size / MemoryLayout<integer_t>.size)
        let status = withUnsafeMutablePointer(to: &stats) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { host_statistics64(Self.host, HOST_VM_INFO64, $0, &count) }
        }
        return status == KERN_SUCCESS ? stats.swapins : 0
    }

    private static func pressure() -> Int {
        var level: Int32 = 0
        var size = MemoryLayout<Int32>.size
        return sysctlbyname("kern.memorystatus_vm_pressure_level", &level, &size, nil, 0) == 0 ? Int(level) : 0
    }
}

/// The Mac, for the report's header.
enum BenchmarkSystem {
    private static func string(_ name: String) -> String {
        var size = 0
        guard sysctlbyname(name, nil, &size, nil, 0) == 0, size > 0 else { return "unknown" }
        var bytes = [CChar](repeating: 0, count: size)
        guard sysctlbyname(name, &bytes, &size, nil, 0) == 0 else { return "unknown" }
        return String(cString: bytes)
    }

    private static func int(_ name: String) -> Int {
        var value: Int64 = 0
        var size = MemoryLayout<Int64>.size
        guard sysctlbyname(name, &value, &size, nil, 0) == 0 else { return 0 }
        // 4-byte values fill the low half.
        return size == 4 ? Int(Int32(truncatingIfNeeded: value)) : Int(value)
    }

    /// AGXAccelerator's gpu-core-count (Apple GPUs; best effort).
    private static func gpuCores() -> Int? {
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("AGXAccelerator"), &iterator) == KERN_SUCCESS else {
            return nil
        }
        defer { IOObjectRelease(iterator) }
        var service = IOIteratorNext(iterator)
        while service != 0 {
            defer { IOObjectRelease(service); service = IOIteratorNext(iterator) }
            if let value = IORegistryEntryCreateCFProperty(service, "gpu-core-count" as CFString, kCFAllocatorDefault, 0)?
                .takeRetainedValue() as? Int {
                return value
            }
        }
        return nil
    }

    private static func power() -> String {
        guard let snapshot = IOPSCopyPowerSourcesInfo()?.takeRetainedValue(),
              let type = IOPSGetProvidingPowerSourceType(snapshot)?.takeUnretainedValue() as String? else { return "unknown" }
        switch type {
        case kIOPMACPowerKey: return "AC power"
        case kIOPMBatteryPowerKey: return "battery"
        default: return type
        }
    }

    @MainActor
    private static func display() -> String {
        guard let screen = NSScreen.main else { return "unknown" }
        let scale = screen.backingScaleFactor
        let size = "\(Int(screen.frame.width * scale))x\(Int(screen.frame.height * scale))"
        return "\(screen.localizedName), \(size) @ \(screen.maximumFramesPerSecond) Hz"
    }

    @MainActor
    static func current(info: [String: Any]) -> BenchmarkSystemInfo {
        let os = ProcessInfo.processInfo.operatingSystemVersion
        let short = info["CFBundleShortVersionString"] as? String ?? "development build"
        return BenchmarkSystemInfo(
            model: string("hw.model"), chip: string("machdep.cpu.brand_string"),
            performanceCores: int("hw.perflevel0.physicalcpu"), efficiencyCores: int("hw.perflevel1.physicalcpu"),
            gpuCores: gpuCores(), memoryGB: int("hw.memsize") >> 30,
            macOS: "\(os.majorVersion).\(os.minorVersion).\(os.patchVersion)", macOSBuild: string("kern.osversion"),
            appVersion: short, appBuild: info["CFBundleVersion"] as? String ?? short, commit: info["BBCommit"] as? String,
            display: display(), power: power(), lowPowerMode: ProcessInfo.processInfo.isLowPowerModeEnabled)
    }
}

/// A finished (or stopped) benchmark run, for the results sheet.
struct BenchmarkResult {
    let summary: BenchmarkSummary
    let folder: URL
    var report: String { summary.markdown }
    var reportURL: URL { folder.appendingPathComponent("report.md") }
}

/// Runs the passes one after another, each a game launch with its own log and CSV in
/// ~/Library/Logs/Bloodborne/Benchmarks/<date-time>/. A pass without `result=ok` ends the run
/// with what is known.
@MainActor
final class BenchmarkRunner: ObservableObject {
    struct Job {
        let layout: Layout
        let gameDir: URL
        let dataDir: URL
        let env: [String: String]
        let assets: URL
    }

    @Published private(set) var progress: String?
    @Published private(set) var result: BenchmarkResult?
    @Published var showingResult = false
    var isRunning: Bool { job != nil }

    private var job: Job?
    private var folder: URL?
    private var work: URL?
    private var date = ""
    private var system: BenchmarkSystemInfo?
    private var passIndex = 0
    private var passes: [PassSummary] = []
    private var samples: [SystemSample] = []
    private var sampler: SystemSampler?
    /// The running pass's game output (SessionLogWriter, timestamped like a session log).
    private var log: SessionLogWriter?
    private var onStarted: (() -> Void)?
    private var onDone: (() -> Void)?

    /// `started` after the first game launch, `done` after the last pass (results ready).
    func start(_ job: Job, home: URL, started: @escaping () -> Void, done: @escaping () -> Void) throws {
        guard self.job == nil else { return }
        let now = Date()
        date = Benchmark.runFolderName(now)
        let folder = Benchmark.folder(home: home).appendingPathComponent(date)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        self.folder = folder
        work = FileManager.default.temporaryDirectory.appendingPathComponent("Bloodborne benchmark \(getpid())")
        system = BenchmarkSystem.current(info: Bundle.main.infoDictionary ?? [:])
        passes = []
        samples = []
        passIndex = 0
        result = nil
        onStarted = started
        onDone = done
        self.job = job
        do {
            try runPass()
        } catch {
            self.job = nil
            throw error
        }
    }

    private var pass: BenchmarkPass { BenchmarkPass.allCases[passIndex] }

    private func runPass() throws {
        guard let job, let folder, let work else { return }
        let pass = self.pass
        try Benchmark.prepare(pass, assets: job.assets, work: work)
        let plan = Benchmark.launchPlan(pass, layout: job.layout, gameDir: job.gameDir, dataDir: job.dataDir,
                                        env: job.env, assets: job.assets, work: work, out: folder)
        let sampler = SystemSampler(pass: pass)
        let log = try SessionLogWriter(folder: folder.appendingPathComponent(pass.rawValue), appVersion: "")
        self.log = log
        _ = try GameRunner.start(plan, log: log) { [weak self] status in
            self?.passEnded(status)
        }
        sampler.start()
        self.sampler = sampler
        let what = pass == .traversal ? "route" : "uncapped"
        progress = "Benchmark: pass \(passIndex + 1) of \(BenchmarkPass.allCases.count), \(what)…"
        if passIndex == 0 { onStarted?() }
    }

    private func passEnded(_ status: Int32) {
        guard let folder else { return }
        samples += sampler?.stop() ?? []
        sampler = nil
        let pass = self.pass
        if let log {
            // The writer's session folder holds just this pass: its logs go next to the CSV.
            let fm = FileManager.default
            try? fm.moveItem(at: log.text, to: folder.appendingPathComponent(pass.logName))
            try? fm.moveItem(at: log.json, to: folder.appendingPathComponent("\(pass.rawValue).jsonl"))
            try? fm.removeItem(at: log.text.deletingLastPathComponent())
        }
        log = nil
        let csv = BenchmarkCSV(text: (try? String(contentsOf: folder.appendingPathComponent(pass.csvName), encoding: .utf8)) ?? "")
        var problem: String?
        if csv.outcome != .ok {
            // The game's own explanation from the pass's log (the crash report's cause line).
            let log = (try? String(contentsOf: folder.appendingPathComponent(pass.logName), encoding: .utf8)) ?? ""
            problem = CrashReport(status: status, log: log, logName: pass.logName, system: .current(appVersion: ""),
                                  settings: [], home: NSHomeDirectory()).cause
        }
        passes.append(PassSummary(pass: pass, csv: csv, samples: samples, exitStatus: status, problem: problem))
        if csv.outcome == .ok && passIndex + 1 < BenchmarkPass.allCases.count {
            passIndex += 1
            do {
                try runPass()
                return
            } catch {
                passes.append(PassSummary(pass: self.pass, csv: BenchmarkCSV(text: ""), samples: [], exitStatus: nil,
                                          problem: "The pass could not start: \(error.localizedDescription)"))
            }
        }
        finish()
    }

    private func finish() {
        guard let folder, let system else { return }
        let summary = BenchmarkSummary(date: date, system: system, passes: passes)
        let csv = ([SystemSample.csvHeader] + samples.map(\.csvRow)).joined(separator: "\n") + "\n"
        try? csv.write(to: folder.appendingPathComponent("system.csv"), atomically: true, encoding: .utf8)
        try? summary.json.write(to: folder.appendingPathComponent("summary.json"))
        try? summary.markdown.write(to: folder.appendingPathComponent("report.md"), atomically: true, encoding: .utf8)
        if let work { try? FileManager.default.removeItem(at: work) }
        result = BenchmarkResult(summary: summary, folder: folder)
        showingResult = true
        progress = nil
        job = nil
        let done = onDone
        onStarted = nil
        onDone = nil
        done?()
    }

    func copyReport() {
        guard let result else { return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(result.report, forType: .string)
    }

    func showInFinder() {
        guard let result else { return }
        NSWorkspace.shared.activateFileViewerSelecting([result.reportURL])
    }
}
