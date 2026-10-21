// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// The Hunter's Dream account service: the Discord device sign-in as scripts/online.py and the
/// Windows port's launcher do it. POST JSON to /auth/device/start gives a code the player
/// approves on the website; /auth/device/poll with the device code answers pending, approved
/// (with the online id and token), slow_down, denied or expired. Every reply carries ResKind
/// (0 = fine) and a Message. The requests and the parsing live here; the loop is the app's.
enum HuntersDream {
    static let defaultCommunityServer = "https://thehuntersdream.com"
    /// Named so on the account page (no hostname: the Mac's name stays private).
    static let deviceName = "bbport on macOS"

    struct Failure: Error, CustomStringConvertible, Equatable {
        let description: String
    }

    struct DeviceStart: Equatable {
        var deviceCode: String
        var userCode: String
        var page: URL
        var interval: TimeInterval
        var expiresIn: TimeInterval
    }

    enum Poll: Equatable {
        case pending, slowDown, denied, expired
        case approved(onlineId: String, token: String)
    }

    /// The game server as scheme://host (online.py community_address): the game picks the ports.
    static func communityAddress(_ value: String?) -> String {
        let value = (value ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        guard !value.isEmpty else { return defaultCommunityServer }
        let scheme: String, rest: String
        if let range = value.range(of: "://", options: .backwards) {
            scheme = String(value[..<range.lowerBound])
            rest = String(value[range.upperBound...])
        } else {
            scheme = ""
            rest = value
        }
        let host = rest.split(separator: "/", omittingEmptySubsequences: false).first.map(String.init) ?? ""
        let bare = host.split(separator: ":", omittingEmptySubsequences: false).first.map(String.init) ?? ""
        return "\(scheme.isEmpty ? "https" : scheme)://\(bare)"
    }

    private static func request(_ url: String, body: [String: String]) -> URLRequest {
        var request = URLRequest(url: URL(string: url)!, timeoutInterval: 15)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try? JSONSerialization.data(withJSONObject: body)
        return request
    }

    static func startRequest(community: String) -> URLRequest {
        request(community + "/auth/device/start", body: ["DeviceName": deviceName])
    }

    static func pollRequest(community: String, deviceCode: String) -> URLRequest {
        request(community + "/auth/device/poll", body: ["DeviceCode": deviceCode])
    }

    /// The reply's fields, or why the service refused (ResKind other than 0) or is not there.
    static func reply(_ data: Data, from url: String) throws -> [String: Any] {
        guard let object = try? JSONSerialization.jsonObject(with: data), let reply = object as? [String: Any],
              let kind = reply["ResKind"] else {
            throw Failure(description: "\(url) has no account service")
        }
        if (kind as? Int) != 0 {
            let message = (reply["Message"] as? String).flatMap { $0.isEmpty ? nil : $0 }
            throw Failure(description: message ?? "refused")
        }
        return reply
    }

    static func deviceStart(_ data: Data, community: String) throws -> DeviceStart {
        let reply = try self.reply(data, from: community + "/auth/device/start")
        guard let device = reply["DeviceCode"] as? String, !device.isEmpty,
              let code = reply["UserCode"] as? String, !code.isEmpty else {
            throw Failure(description: "no code in the reply")
        }
        let page = (reply["VerificationUri"] as? String).flatMap { URL(string: $0) } ?? URL(string: community + "/link")!
        let interval = max(1, (reply["Interval"] as? Double) ?? 5)
        let expires = (reply["ExpiresIn"] as? Double) ?? 600
        return DeviceStart(deviceCode: device, userCode: code, page: page, interval: interval, expiresIn: expires)
    }

    static func poll(_ data: Data, community: String) throws -> Poll {
        let reply = try self.reply(data, from: community + "/auth/device/poll")
        switch reply["Status"] as? String {
        case "approved":
            guard let id = reply["OnlineId"] as? String, !id.isEmpty,
                  let token = reply["Token"] as? String, !token.isEmpty else {
                throw Failure(description: "no token in the reply")
            }
            return .approved(onlineId: id, token: token)
        case "slow_down": return .slowDown
        case "denied": return .denied
        case "expired": return .expired
        default: return .pending
        }
    }
}
