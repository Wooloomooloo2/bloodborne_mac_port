// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// The Hunter's Dream device sign-in over the network (the protocol: launcher/Model/HuntersDream.swift).
enum HuntersDreamClient {
    /// Asks for a code, shows it through `onCode`, then polls until the player approved it on
    /// the website. Cancelling the task stops the polling.
    static func signIn(community: String,
                       onCode: @escaping @MainActor (HuntersDream.DeviceStart) -> Void) async throws -> (onlineId: String, token: String) {
        let startData = try await fetch(try HuntersDream.startRequest(community: community))
        let start = try HuntersDream.deviceStart(startData, community: community)
        await onCode(start)
        var interval = start.interval
        let deadline = Date().addingTimeInterval(start.expiresIn)
        var failures = 0
        while Date() < deadline {
            try await Task.sleep(nanoseconds: UInt64(interval * 1_000_000_000))
            let data: Data
            do {
                data = try await fetch(try HuntersDream.pollRequest(community: community, deviceCode: start.deviceCode))
                failures = 0
            } catch let error as HuntersDream.Failure {
                // One poll lost to the network (the server's HTTPS fails now and then) is
                // not the end of the sign-in; a few in a row are.
                failures += 1
                if failures >= 5 { throw error }
                continue
            }
            switch try HuntersDream.poll(data, community: community) {
            case let .approved(onlineId, token): return (onlineId, token)
            case .slowDown: interval = min(interval + 5, HuntersDream.intervalRange.upperBound)
            case .denied: throw HuntersDream.Failure(description: "the sign-in was denied")
            case .expired: throw HuntersDream.Failure(description: "the code expired; try again")
            case .pending: break
            }
        }
        throw HuntersDream.Failure(description: "the code expired; try again")
    }

    private static func fetch(_ request: URLRequest) async throws -> Data {
        do {
            return try await URLSession.shared.data(for: request).0
        } catch let error as URLError where error.code == .cancelled {
            throw CancellationError()
        } catch {
            throw HuntersDream.Failure(description: "\(request.url?.host ?? "the server"): \(error.localizedDescription)")
        }
    }
}
