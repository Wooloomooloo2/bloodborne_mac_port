// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct Achievement: Identifiable, Sendable {
    let id: Int
    let name: String
    let description: String
    let grade: String
    let dlc: Bool
}

enum AchievementHistory {
    static func logURL(dataDir: URL, env: [String: String]) -> URL {
        let userDir = env["BB_USER_DIR"].flatMap { $0.isEmpty ? nil : URL(fileURLWithPath: $0) }
            ?? dataDir.appendingPathComponent("user")
        return userDir.appendingPathComponent("trophies.log")
    }

    // Same permissive line format as the C runtime and original pyL1nx tracker.
    // Preserve the earliest timestamp; malformed/unknown records never unlock a trophy.
    static func parse(_ text: String) -> [Int: Date] {
        var result: [Int: Date] = [:]
        for line in text.split(separator: "\n") {
            let words = line.split(whereSeparator: \.isWhitespace)
            guard words.count >= 2, let id = Int(words[0]), (0..<40).contains(id),
                  let seconds = Int64(words[1]), seconds > 0, seconds <= 253402300799 else { continue }
            let date = Date(timeIntervalSince1970: Double(seconds))
            result[id] = min(result[id] ?? date, date)
        }
        return result
    }

    static func read(_ url: URL) throws -> [Int: Date] {
        guard FileManager.default.fileExists(atPath: url.path) else { return [:] }
        return parse(try String(contentsOf: url, encoding: .utf8))
    }
}

enum AchievementArtwork {
    // Same bounded, unencrypted TRP directory as our original Cocoa browser.
    static func read(game: URL) -> [Int: Data] {
        let url = game.appendingPathComponent("sce_sys/trophy/trophy00.trp")
        guard let size = try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize,
              (96...64*1024*1024).contains(size), let data = try? Data(contentsOf: url, options: .mappedIfSafe)
        else { return [:] }
        return parse(data)
    }

    static func parse(_ data: Data) -> [Int: Data] {
        guard (96...64*1024*1024).contains(data.count) else { return [:] }
        let bytes = [UInt8](data)
        func be(_ offset: Int, _ count: Int) -> UInt64 {
            bytes[offset..<offset+count].reduce(UInt64(0)) { ($0 << 8) | UInt64($1) }
        }
        let count = be(16,4), stride = be(20,4)
        guard be(0,4) == 0xdca24d00, be(8,8) == UInt64(bytes.count),
              (64...4096).contains(stride), count <= UInt64(bytes.count-96)/stride else { return [:] }
        var images: [Int: Data] = [:]
        for index in 0..<Int(count) {
            let entry = 96 + index*Int(stride)
            let offset = be(entry+32,8), size = be(entry+40,8)
            guard be(entry+48,4) == 0, (24...2*1024*1024).contains(size),
                  offset <= UInt64(bytes.count), size <= UInt64(bytes.count)-offset else { continue }
            let start = Int(offset)
            guard Array(bytes[start..<start+8]) == [137,80,78,71,13,10,26,10],
                  (1...512).contains(be(start+16,4)), (1...512).contains(be(start+20,4)) else { continue }
            let nameBytes = bytes[entry..<entry+32].prefix { $0 != 0 }
            let name = String(decoding: nameBytes, as: UTF8.self)
            for id in 0..<40 where name == String(format: "TROP%03d.PNG", id) {
                images[id] = Data(bytes[start..<start+Int(size)])
            }
        }
        return images
    }
}
