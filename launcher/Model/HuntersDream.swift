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

    /// Plain http is accepted only for a server on this Mac (online.py LOCAL_HOSTS).
    static let localHosts: Set<String> = ["localhost", "127.0.0.1"]
    /// Between polls, and the most the sign-in waits: the server's numbers within reason.
    static let intervalRange: ClosedRange<TimeInterval> = 1...60
    static let maxExpiresIn: TimeInterval = 1800

    /// The game server as https://host (online.py community_address): the game picks the ports.
    /// Fails for another scheme, or http:// to a host that is not this Mac: the account token
    /// goes there.
    static func communityAddress(_ value: String?) throws -> String {
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
        let checked = scheme.isEmpty ? "https" : scheme.lowercased()
        guard !bare.isEmpty else { throw Failure(description: "no host in the community server address \(value)") }
        guard checked == "https" || (checked == "http" && localHosts.contains(bare.lowercased())) else {
            throw Failure(description: "the community server must be https:// (got \(value); plain http only for localhost)")
        }
        return "\(checked)://\(bare)"
    }

    /// The community server's own page (its scheme, and its host or one under it): the only
    /// page the sign-in opens in the browser.
    static func onCommunityHost(_ url: URL, community: String) -> Bool {
        guard let base = URLComponents(string: community), let baseHost = base.host?.lowercased(),
              let scheme = url.scheme?.lowercased(), let host = url.host?.lowercased() else { return false }
        return scheme == base.scheme?.lowercased() && (host == baseHost || host.hasSuffix("." + baseHost))
    }

    /// An online id as The Hunter's Dream gives them (online.py ONLINE_ID): it goes into
    /// bbport.ini and the keychain item's name.
    static func validOnlineId(_ id: String) -> Bool {
        id.range(of: "^[A-Za-z0-9_-]{3,16}$", options: .regularExpression) != nil
    }

    private static func request(_ url: String, body: [String: String]) throws -> URLRequest {
        guard let address = URL(string: url) else { throw Failure(description: "not an address: \(url)") }
        var request = URLRequest(url: address, timeoutInterval: 15)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try? JSONSerialization.data(withJSONObject: body)
        return request
    }

    static func startRequest(community: String) throws -> URLRequest {
        try request(community + "/auth/device/start", body: ["DeviceName": deviceName])
    }

    static func pollRequest(community: String, deviceCode: String) throws -> URLRequest {
        try request(community + "/auth/device/poll", body: ["DeviceCode": deviceCode])
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
        // A page elsewhere than the community server is not opened: its own /link instead.
        guard let fallback = URL(string: community + "/link") else { throw Failure(description: "not an address: \(community)") }
        let offered = (reply["VerificationUri"] as? String).flatMap { URL(string: $0) }
        let page = offered.flatMap { onCommunityHost($0, community: community) ? $0 : nil } ?? fallback
        let interval = min(max((reply["Interval"] as? Double) ?? 5, intervalRange.lowerBound), intervalRange.upperBound)
        let expires = min(max((reply["ExpiresIn"] as? Double) ?? 600, 0), maxExpiresIn)
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
            guard validOnlineId(id) else { throw Failure(description: "the reply has an invalid online id") }
            guard token.unicodeScalars.allSatisfy({ $0.value > 0x20 && $0.value != 0x7f }) else {
                throw Failure(description: "the reply has an invalid token")
            }
            return .approved(onlineId: id, token: token)
        case "slow_down": return .slowDown
        case "denied": return .denied
        case "expired": return .expired
        default: return .pending
        }
    }
}
