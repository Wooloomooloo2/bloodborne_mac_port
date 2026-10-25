// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// The built-in benchmark's two game launches (docs/DESIGN_BENCHMARK.md).
enum BenchmarkPass: String, CaseIterable, Codable {
    case traversal, throughput

    var title: String {
        switch self {
        case .traversal: "Traversal"
        case .throughput: "Throughput"
        }
    }
    var subtitle: String {
        switch self {
        case .traversal: "recorded route through Central Yharnam, 60 FPS cap"
        case .throughput: "standing at the route's start, uncapped, \(Benchmark.throughputSeconds) s"
        }
    }
    var csvName: String { "\(rawValue).csv" }
    var logName: String { "\(rawValue).log" }
}

/// Assets, fixed settings and the per-pass launch.
enum Benchmark {
    static let throughputSeconds = 30
    /// The standard benchmark's settings, as the report states them.
    static let settingsLine = "1920x1080 output, native (upscaler off), default effects, windowed, texture compression on"

    /// nil when the folder has route.pad and savedata/, else why the benchmark can't run.
    static func assetsProblem(_ folder: URL) -> String? {
        let fm = FileManager.default
        var isFolder: ObjCBool = false
        let route = fm.fileExists(atPath: folder.appendingPathComponent("route.pad").path)
        let save = fm.fileExists(atPath: folder.appendingPathComponent("savedata").path, isDirectory: &isFolder) && isFolder.boolValue
        if route && save { return nil }
        return "The benchmark needs its save and recorded route (route.pad and savedata/) in \(folder.path). This build doesn't include them."
    }

    /// The pass's bbport.ini: the standard settings, nothing from the player's.
    static func settings(for pass: BenchmarkPass) -> IniFile {
        var settings = GameSettings()
        settings.output = PixelSize(1920, 1080)
        settings.upscaler = .off
        settings.preset = 0
        settings.showFPS = true
        settings.metalHUD = false
        settings.fullscreen = false
        settings.liveResolution = .off
        settings.textureCompression = true
        settings.effects["skip_intro"] = true
        var ini = IniFile(text: "# Bloodborne benchmark: \(pass.rawValue) pass (written by the launcher)\n")
        settings.apply(to: &ini)
        // 0: no cap (launch_settings.sh only knows 30-120; BB_FPS_LIMIT=0 says the same).
        ini["fps_cap"] = pass == .traversal ? "60" : "0"
        return ini
    }

    /// Fresh copy of the benchmark save and the pass's bbport.ini in `work` (a temporary folder):
    /// the game may write both, and the next pass must start from the same save.
    static func prepare(_ pass: BenchmarkPass, assets: URL, work: URL) throws {
        let fm = FileManager.default
        try fm.createDirectory(at: work, withIntermediateDirectories: true)
        let saves = work.appendingPathComponent("savedata")
        if fm.fileExists(atPath: saves.path) { try fm.removeItem(at: saves) }
        try fm.copyItem(at: assets.appendingPathComponent("savedata"), to: saves)
        try settings(for: pass).write(to: work.appendingPathComponent("benchmark.ini"))
    }

    /// The normal launch plus the design's environment table. Variables a pass doesn't use are
    /// removed, so nothing inherited from the launcher's environment changes the run.
    static func launchPlan(_ pass: BenchmarkPass, layout: Layout, gameDir: URL, dataDir: URL,
                           env: [String: String], assets: URL, work: URL, out: URL) -> LaunchPlan {
        let saves = work.appendingPathComponent("savedata")
        var plan = layout.launchPlan(gameDir: gameDir, saveDir: saves, dataDir: dataDir, env: env)
        var e = plan.environment
        for key in ["BB_BENCH_SECONDS", "BB_PAD_REPLAY", "BB_FPS_LIMIT", "BB_VBLANK_HZ", "BB_PRESENT_MODE", "BB_FPS_AUTO",
                    "BB_PATCHES", "BB_PATCHES_CONFIG", "BB_MODS_DIR", "BB_MODS_CONFIG"] {
            e.removeValue(forKey: key)
        }
        e["BB_BENCHMARK"] = pass.rawValue
        e["BB_BENCH_OUT"] = out.appendingPathComponent(pass.csvName).path
        e["BB_SAVE_DIR"] = saves.path
        e["BB_CONFIG"] = work.appendingPathComponent("benchmark.ini").path
        e["BB_FRAME_STATS"] = "1"
        // Always the plain game: no mods, no patch files (cheats are off: benchmark.ini has none).
        e["BB_MODS_ENABLED"] = "0"
        e["BB_PATCHES_DIR"] = work.appendingPathComponent("no-patches").path
        switch pass {
        case .traversal:
            e["BB_PAD_REPLAY"] = assets.appendingPathComponent("route.pad").path
            e["BB_FPS"] = "60"
        case .throughput:
            e["BB_BENCH_SECONDS"] = String(throughputSeconds)
            e["BB_FPS"] = "uncap"
            e["BB_FPS_LIMIT"] = "0"
            e["BB_VBLANK_HZ"] = "480"
            e["BB_PRESENT_MODE"] = "Immediate"
        }
        plan.environment = e
        return plan
    }

    /// The run's folder name under ~/Library/Logs/Bloodborne/Benchmarks (local time).
    static func runFolderName(_ date: Date) -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd HH.mm.ss"
        return f.string(from: date)
    }

    static func folder(home: URL) -> URL { Logs.folder(home: home).appendingPathComponent("Benchmarks") }
}

// MARK: - The game's CSV

/// One presented frame (the CSV columns, docs/DESIGN_BENCHMARK.md). Times in ms, uploads in MB.
struct BenchmarkFrame: Equatable {
    var t: Double = 0
    var frame: Double
    var game: Double = 0
    var gpu: Double = 0
    var draws = 0
    var compiles = 0
    var compileWait: Double = 0
    var upload: Double = 0
    var gameCPU: Double = 0
    var gpuThread: Double = 0
    var ahead: Double = 0
    var faults = 0
}

enum BenchmarkOutcome: Equatable {
    case ok
    case error(String)
    /// No result line: the game stopped (or was quit) before the pass ended.
    case interrupted

    var text: String {
        switch self {
        case .ok: "ok"
        case let .error(reason): "error: \(reason)"
        case .interrupted: "interrupted (the game stopped before the pass ended)"
        }
    }
}

/// The BB_BENCH_OUT file: `# key=value` comments, a header row, one row per frame. Columns are
/// found by name (unknown ones are ignored, missing ones read 0); rows that don't parse (a line
/// cut off when the game stopped) are skipped.
struct BenchmarkCSV: Equatable {
    var meta: [String: String] = [:]
    var frames: [BenchmarkFrame] = []
    var outcome = BenchmarkOutcome.interrupted

    var started: Double? { meta["started"].flatMap(Double.init) }
    var ended: Double? { meta["ended"].flatMap(Double.init) }

    init(text: String) {
        var columns: [String: Int]?
        for raw in text.replacingOccurrences(of: "\r\n", with: "\n").split(separator: "\n") {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.isEmpty { continue }
            if line.hasPrefix("#") {
                let body = line.dropFirst().trimmingCharacters(in: .whitespaces)
                guard let eq = body.firstIndex(of: "=") else {
                    // "# bloodborne-benchmark 1"
                    if body.hasPrefix("bloodborne-benchmark") {
                        meta["format"] = body.dropFirst("bloodborne-benchmark".count).trimmingCharacters(in: .whitespaces)
                    }
                    continue
                }
                let key = body[..<eq].trimmingCharacters(in: .whitespaces)
                let value = body[body.index(after: eq)...].trimmingCharacters(in: .whitespaces)
                meta[key] = value
                if key == "result" {
                    if value == "ok" {
                        outcome = .ok
                    } else if value.hasPrefix("error") {
                        let reason = value.dropFirst("error".count).drop { $0 == ":" || $0 == " " }
                        outcome = .error(reason.isEmpty ? "unknown" : String(reason))
                    } else {
                        outcome = .error(value)
                    }
                }
                continue
            }
            let fields = line.split(separator: ",", omittingEmptySubsequences: false).map {
                $0.trimmingCharacters(in: .whitespaces)
            }
            guard let index = columns else {
                columns = Dictionary(fields.enumerated().map { ($1, $0) }) { a, _ in a }
                continue
            }
            func number(_ name: String) -> Double? {
                guard let i = index[name] else { return 0 }
                return i < fields.count ? Double(fields[i]) : nil
            }
            guard let t = number("t_ms"), let frame = number("frame_ms"), let game = number("game_ms"),
                  let gpu = number("gpu_ms"), let draws = number("draws"), let compiles = number("compiles"),
                  let wait = number("compile_wait_ms"), let upload = number("upload_mb"),
                  let cpu = number("game_cpu_ms"), let thread = number("gpu_thread_ms"),
                  let ahead = number("ahead_ms"), let faults = number("game_faults"),
                  index["frame_ms"] != nil, fields.count >= index.count else { continue }
            frames.append(BenchmarkFrame(t: t, frame: frame, game: game, gpu: gpu, draws: Int(draws),
                                         compiles: Int(compiles), compileWait: wait, upload: upload,
                                         gameCPU: cpu, gpuThread: thread, ahead: ahead, faults: Int(faults)))
        }
    }
}

// MARK: - System samples (the launcher's 1 Hz sampler)

/// One process at one sample: CPU % (100 = one core; nil on its first sample) and footprint.
struct ProcessSample: Equatable {
    var cpu: Double?
    var footprintMB: Double
}

struct SystemSample: Equatable {
    var pass: BenchmarkPass
    /// Unix time, seconds.
    var time: Double
    var probe: ProcessSample?
    var host: ProcessSample?
    var swapUsedMB: Double
    /// The system's swap-in counter (cumulative since boot).
    var swapIns: UInt64
    /// kern.memorystatus_vm_pressure_level: 1 normal, 2 warning, 4 critical.
    var pressure: Int
    /// ProcessInfo.ThermalState raw value: 0 nominal ... 3 critical.
    var thermal: Int

    static let csvHeader = "pass,time,bb_probe_cpu_pct,bb_probe_footprint_mb,gpu_host_cpu_pct,gpu_host_footprint_mb,swap_used_mb,swap_ins,pressure,thermal"

    var csvRow: String {
        func f(_ v: Double?) -> String { v.map { String(format: "%.1f", $0) } ?? "" }
        return [pass.rawValue, String(format: "%.3f", time), f(probe?.cpu), f(probe?.footprintMB),
                f(host?.cpu), f(host?.footprintMB), f(swapUsedMB), String(swapIns), String(pressure),
                String(thermal)].joined(separator: ",")
    }

    static func pressureName(_ level: Int) -> String {
        switch level {
        case 4...: "critical"
        case 2...: "warning"
        case 1: "normal"
        default: "unknown"
        }
    }

    static func thermalName(_ state: Int) -> String {
        ["nominal", "fair", "serious", "critical"].indices.contains(state)
            ? ["nominal", "fair", "serious", "critical"][state] : "unknown"
    }
}

// MARK: - Analysis

enum HitchCause: String, Codable {
    case compile, streaming, gpu = "GPU", gameCPU = "game CPU", other
}

struct Hitch: Codable, Equatable {
    var timeS: Double
    var frameMs: Double
    var cause: HitchCause
    /// The numbers behind the cause, e.g. "compile wait 80 ms".
    var detail: String
}

/// The CSV's numbers (docs/DESIGN_BENCHMARK.md "Metrics").
struct FrameMetrics: Codable, Equatable {
    var frames: Int
    var seconds: Double
    var averageFPS: Double
    /// Most frames in any 1 s window; nil for a pass shorter than 1 s.
    var maxFPS: Int?
    var low1FPS: Double
    var low01FPS: Double
    var over33: Int
    var over50: Int
    var over100: Int
    var longestMs: Double
    var medianMs: Double
    var hitchThresholdMs: Double
    var hitches: Int
    var hitchMsPerMinute: Double
    var worstHitches: [Hitch]
    var gpuBusyPercent: Double
    var gpuMsPerFrame: Double
    var compiles: Int
    var compileWaitMs: Double

    init?(_ frames: [BenchmarkFrame]) {
        guard !frames.isEmpty else { return nil }
        let times = frames.map(\.frame)
        let total = times.reduce(0, +)
        guard total > 0 else { return nil }
        self.frames = frames.count
        seconds = total / 1000
        averageFPS = Double(frames.count) / seconds

        // Best 1 s window: frames whose t_ms (the end of the frame) lies in (t - 1000, t].
        var best: Int?
        var first = 0
        for (i, f) in frames.enumerated() where f.t >= 1000 {
            while frames[first].t <= f.t - 1000 { first += 1 }
            best = max(best ?? 0, i - first + 1)
        }
        maxFPS = best

        let slowest = times.sorted(by: >)
        func low(_ fraction: Double) -> Double {
            let n = max(1, Int(Double(slowest.count) * fraction))
            return 1000 / (slowest.prefix(n).reduce(0, +) / Double(n))
        }
        low1FPS = low(0.01)
        low01FPS = low(0.001)
        over33 = times.filter { $0 > 100.0 / 3 }.count
        over50 = times.filter { $0 > 50 }.count
        over100 = times.filter { $0 > 100 }.count
        longestMs = slowest[0]
        let ascending = Array(slowest.reversed())
        let mid = ascending.count / 2
        medianMs = ascending.count % 2 == 1 ? ascending[mid] : (ascending[mid - 1] + ascending[mid]) / 2

        hitchThresholdMs = max(2 * medianMs, 25)
        let threshold = hitchThresholdMs, median = medianMs
        let hitchFrames = frames.filter { $0.frame > threshold }
        hitches = hitchFrames.count
        hitchMsPerMinute = hitchFrames.reduce(0) { $0 + $1.frame - median } / (total / 60000)
        let typical = Typical(frames)
        worstHitches = hitchFrames.sorted { $0.frame > $1.frame }.prefix(10).map { Self.hitch($0, typical) }

        gpuBusyPercent = 100 * frames.reduce(0) { $0 + $1.gpu } / total
        gpuMsPerFrame = frames.reduce(0) { $0 + $1.gpu } / Double(frames.count)
        compiles = frames.reduce(0) { $0 + $1.compiles }
        compileWaitMs = frames.reduce(0) { $0 + $1.compileWait }
    }

    /// The run's medians: every frame uploads some data and takes some GPU-tracked faults (a city
    /// walk on an M4 Max: ~14 MB and ~46 a frame), so streaming is judged against them.
    struct Typical {
        var upload = 0.0, faults = 0.0
        init(_ frames: [BenchmarkFrame]) {
            func median(_ v: [Double]) -> Double { v.isEmpty ? 0 : v.sorted()[v.count / 2] }
            upload = median(frames.map(\.upload))
            faults = median(frames.map { Double($0.faults) })
        }
        init() {}
    }

    /// The likely cause, first match wins: compile, streaming (uploads or faults well above the
    /// run's typical frame), GPU, game CPU, other.
    static func hitch(_ f: BenchmarkFrame, _ typical: Typical = Typical()) -> Hitch {
        func ms(_ v: Double) -> String { String(format: "%.0f ms", v) }
        let (cause, detail): (HitchCause, String) =
            if f.compileWait >= 0.3 * f.frame {
                (.compile, "compile wait \(ms(f.compileWait)), \(f.compiles) compiles")
            } else if f.upload >= max(32, 3 * typical.upload) || Double(f.faults) >= max(20, 3 * typical.faults) {
                (.streaming, String(format: "%.0f MB uploaded, %d faults", f.upload, f.faults))
            } else if f.gpu >= 0.6 * f.frame || f.ahead >= 0.5 * f.frame {
                (.gpu, "GPU \(ms(f.gpu)), waited \(ms(f.ahead)) for it")
            } else if f.game >= 0.8 * f.frame {
                (.gameCPU, "game frame \(ms(f.game))")
            } else {
                (.other, "game \(ms(f.game)), GPU \(ms(f.gpu))")
            }
        return Hitch(timeS: f.t / 1000, frameMs: f.frame, cause: cause, detail: detail)
    }
}

struct ProcessStats: Codable, Equatable {
    var name: String
    var meanCPU: Double
    var peakCPU: Double
    var peakFootprintMB: Double
}

/// The sampler's numbers for one pass (the measuring window only when the CSV gives it).
struct SystemMetrics: Codable, Equatable {
    var processes: [ProcessStats]
    var swapStartMB: Double
    var swapEndMB: Double
    var swapPeakMB: Double
    var swapIns: UInt64
    var worstPressure: String
    var worstThermal: String

    init?(_ all: [SystemSample], from start: Double? = nil, to end: Double? = nil) {
        let window = all.filter { s in (start.map { s.time >= $0 } ?? true) && (end.map { s.time <= $0 } ?? true) }
        let samples = window.isEmpty ? all : window
        guard let first = samples.first, let last = samples.last else { return nil }
        func stats(_ name: String, _ pick: (SystemSample) -> ProcessSample?) -> ProcessStats? {
            let seen = samples.compactMap(pick)
            guard !seen.isEmpty else { return nil }
            let cpu = seen.compactMap(\.cpu)
            return ProcessStats(name: name, meanCPU: cpu.isEmpty ? 0 : cpu.reduce(0, +) / Double(cpu.count),
                                peakCPU: cpu.max() ?? 0, peakFootprintMB: seen.map(\.footprintMB).max() ?? 0)
        }
        processes = [stats("bb-probe", \.probe), stats("GPU host", \.host)].compactMap { $0 }
        swapStartMB = first.swapUsedMB
        swapEndMB = last.swapUsedMB
        swapPeakMB = samples.map(\.swapUsedMB).max() ?? 0
        swapIns = last.swapIns >= first.swapIns ? last.swapIns - first.swapIns : 0
        worstPressure = SystemSample.pressureName(samples.map(\.pressure).max() ?? 0)
        worstThermal = SystemSample.thermalName(samples.map(\.thermal).max() ?? -1)
    }
}

// MARK: - Report

/// The Mac the benchmark ran on (gathered by the app: sysctl, IOKit, AppKit).
struct BenchmarkSystemInfo: Codable, Equatable {
    var model: String
    var chip: String
    var performanceCores: Int
    var efficiencyCores: Int
    var gpuCores: Int?
    var memoryGB: Int
    var macOS: String
    var macOSBuild: String
    var appVersion: String
    var appBuild: String
    var commit: String?
    var display: String
    var power: String
    var lowPowerMode: Bool
}

struct PassSummary: Codable, Equatable {
    var pass: BenchmarkPass
    var result: String
    var ok: Bool
    var exitStatus: Int32?
    /// The game's Crash: line when it stopped with an error.
    var problem: String?
    var frames: FrameMetrics?
    var system: SystemMetrics?

    init(pass: BenchmarkPass, csv: BenchmarkCSV, samples: [SystemSample], exitStatus: Int32?, problem: String? = nil) {
        self.pass = pass
        result = csv.outcome.text
        ok = csv.outcome == .ok
        self.exitStatus = exitStatus
        self.problem = problem
        frames = FrameMetrics(csv.frames)
        system = SystemMetrics(samples.filter { $0.pass == pass }, from: csv.started, to: csv.ended)
    }
}

/// summary.json, and the report rendered from it.
struct BenchmarkSummary: Codable, Equatable {
    var format = 1
    var date: String
    var settings = Benchmark.settingsLine
    var system: BenchmarkSystemInfo
    var passes: [PassSummary]

    var complete: Bool { passes.count == BenchmarkPass.allCases.count && passes.allSatisfy(\.ok) }

    var json: Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        return (try? encoder.encode(self)) ?? Data()
    }

    var markdown: String {
        func n(_ v: Double, _ digits: Int = 1) -> String { String(format: "%.\(digits)f", v) }
        func gb(_ mb: Double) -> String { n(mb / 1024, 1) + " GB" }
        let s = system
        var cpu = "\(s.performanceCores)P + \(s.efficiencyCores)E CPU"
        if let g = s.gpuCores { cpu += ", \(g)-core GPU" }
        var version = s.appVersion == s.appBuild ? s.appVersion : "\(s.appVersion) (\(s.appBuild))"
        if let c = s.commit, !c.isEmpty { version += ", commit \(c)" }
        var out = """
        # Bloodborne benchmark, \(date)

        - **Mac:** \(s.model), \(s.chip), \(cpu), \(s.memoryGB) GB
        - **macOS:** \(s.macOS.replacingOccurrences(of: "Version ", with: "")) (\(s.macOSBuild)); app \(version)
        - **Display:** \(s.display); power: \(s.power)\(s.lowPowerMode ? ", Low Power Mode on" : "")
        - **Settings:** \(settings)

        """
        for p in passes {
            out += "\n## \(p.pass.title): \(p.pass.subtitle)\n\n"
            if !p.ok {
                out += "**Result: \(p.result)**"
                if let status = p.exitStatus, status != 0 { out += " (exit status \(status))" }
                out += "\n"
                if let problem = p.problem { out += "\n> \(problem)\n" }
                out += "\n"
            }
            guard let f = p.frames else {
                out += "No frames were measured.\n"
                continue
            }
            out += "| Metric | Value |\n|---|---|\n"
            out += "| Frames | \(f.frames) in \(n(f.seconds)) s |\n"
            out += "| Average FPS | \(n(f.averageFPS)) |\n"
            // The traversal is capped at 60: its best second says nothing.
            if p.pass == .throughput, let best = f.maxFPS { out += "| Max FPS (best 1 s) | \(best) |\n" }
            out += "| 1% / 0.1% low | \(n(f.low1FPS)) / \(n(f.low01FPS)) FPS |\n"
            out += "| Frames over 33 / 50 / 100 ms | \(f.over33) / \(f.over50) / \(f.over100) |\n"
            out += "| Median / longest frame | \(n(f.medianMs)) / \(n(f.longestMs)) ms |\n"
            out += "| Hitches (over \(n(f.hitchThresholdMs)) ms) | \(f.hitches), \(n(f.hitchMsPerMinute, 0)) ms lost per minute |\n"
            out += "| GPU | \(n(f.gpuBusyPercent, 0))% busy, \(n(f.gpuMsPerFrame)) ms per frame |\n"
            out += "| Shader compiles | \(f.compiles), \(n(f.compileWaitMs, 0)) ms waited |\n"
            if let m = p.system {
                for proc in m.processes {
                    out += "| \(proc.name) | CPU \(n(proc.meanCPU, 0))% mean, \(n(proc.peakCPU, 0))% peak; \(gb(proc.peakFootprintMB)) peak memory |\n"
                }
                out += "| Swap | \(gb(m.swapStartMB)) → \(gb(m.swapEndMB)) (peak \(gb(m.swapPeakMB))), \(m.swapIns) swap-ins |\n"
                out += "| Memory pressure / thermal | \(m.worstPressure) / \(m.worstThermal) (worst) |\n"
            }
            if !f.worstHitches.isEmpty {
                out += "\nWorst hitches:\n\n| At | Frame | Likely cause |\n|---|---|---|\n"
                for h in f.worstHitches {
                    out += "| \(n(h.timeS)) s | \(n(h.frameMs)) ms | \(h.cause.rawValue): \(h.detail) |\n"
                }
            }
        }
        return out
    }
}
