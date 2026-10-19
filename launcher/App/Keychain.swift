// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// The login keychain through `security`, the tool run.sh reads the items with (so the item's
/// access list already names it and no access prompt appears at launch). The secret is an
/// argument of that short-lived process, as scripts/online.py passes it; the launcher keeps and
/// logs nothing of it. BB_KEYCHAIN_TOOL names another command (tests, as in launch_settings.sh).
struct LoginKeychain: SecretStore {
    private let tool = ProcessInfo.processInfo.environment["BB_KEYCHAIN_TOOL"] ?? "security"

    private func security(_ arguments: [String]) -> (status: Int32, output: String) {
        GameRunner.run(["/usr/bin/env", tool] + arguments)
    }

    func has(service: String, account: String) -> Bool {
        // Without -w: the item's attributes, never the secret.
        security(["find-generic-password", "-s", service, "-a", account]).status == 0
    }

    func store(service: String, account: String, secret: String) throws {
        let result = security(["add-generic-password", "-U", "-s", service, "-a", account,
                               "-l", "\(service) (\(account))", "-w", secret])
        if result.status != 0 {
            let detail = result.output.trimmingCharacters(in: .whitespacesAndNewlines)
            throw HuntersDream.Failure(description: detail.isEmpty ? "security add-generic-password failed (status \(result.status))" : detail)
        }
    }

    func remove(service: String, account: String) {
        _ = security(["delete-generic-password", "-s", service, "-a", account])
    }
}
