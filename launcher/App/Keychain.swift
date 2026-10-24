// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Security

/// The login keychain. Items are added with the Security framework, so the secret never
/// appears on a command line; their access list trusts the launcher and /usr/bin/security,
/// the tool run.sh reads them with (scripts/launch_settings.sh), so no access prompt appears at
/// launch. Anything that can run /usr/bin/security as this user can therefore read them, as with
/// items `security add-generic-password` makes. Checks and removals go through `security`, which
/// may delete the items it made itself (scripts/online.py's, or an earlier launcher's); the
/// launcher keeps and logs nothing of the secret. BB_KEYCHAIN_TOOL names another command for
/// those (tests, as in launch_settings.sh).
struct LoginKeychain: SecretStore {
    private let tool = ProcessInfo.processInfo.environment["BB_KEYCHAIN_TOOL"] ?? "security"
    static let securityTool = "/usr/bin/security"

    private func security(_ arguments: [String]) -> (status: Int32, output: String) {
        GameRunner.run(["/usr/bin/env", tool] + arguments)
    }

    func has(service: String, account: String) -> Bool {
        // Without -w: the item's attributes, never the secret.
        security(["find-generic-password", "-s", service, "-a", account]).status == 0
    }

    /// Replaces the item: the old one is removed through `security` (an item it made refuses
    /// changes from another program), then added with the access list above.
    func store(service: String, account: String, secret: String) throws {
        remove(service: service, account: account)
        let label = "\(service) (\(account))"
        guard let access = Self.access(label: label) else {
            throw HuntersDream.Failure(description: "the keychain access list could not be made")
        }
        let item: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecAttrLabel as String: label,
            kSecAttrAccess as String: access,
            kSecValueData as String: Data(secret.utf8),
        ]
        let status = SecItemAdd(item as CFDictionary, nil)
        if status != errSecSuccess {
            let detail = SecCopyErrorMessageString(status, nil) as String? ?? "status \(status)"
            throw HuntersDream.Failure(description: "the keychain refused the item: \(detail)")
        }
    }

    func remove(service: String, account: String) {
        _ = security(["delete-generic-password", "-s", service, "-a", account])
    }

    /// The launcher and /usr/bin/security (SecAccess: the access list of a file keychain, which
    /// the login keychain is).
    private static func access(label: String) -> SecAccess? {
        var launcher: SecTrustedApplication?, reader: SecTrustedApplication?
        guard SecTrustedApplicationCreateFromPath(nil, &launcher) == errSecSuccess,
              SecTrustedApplicationCreateFromPath(securityTool, &reader) == errSecSuccess,
              let launcher, let reader else { return nil }
        var access: SecAccess?
        guard SecAccessCreate(label as CFString, [launcher, reader] as CFArray, &access) == errSecSuccess else { return nil }
        return access
    }
}
