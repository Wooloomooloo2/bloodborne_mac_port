// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// Which co-op pool bells and summons go through (bbport.ini `online_pool`, read by
/// scripts/launch_settings.sh).
enum OnlinePool: String, CaseIterable, Identifiable {
    case huntersdream, shadnet
    var id: String { rawValue }
    var label: String { self == .huntersdream ? "The Hunter's Dream" : "shadNet server" }
}

/// A secret the launcher never shows or logs: absent, in the login keychain (the ini says
/// `keychain`, the way scripts/online.py writes it), or written in the file (kept as read).
enum StoredSecret: Equatable {
    case none, keychain, inFile(String)

    init(iniValue: String?) {
        switch iniValue {
        case nil, "": self = .none
        case "keychain": self = .keychain
        case let value?: self = .inFile(value)
        }
    }

    var iniValue: String? {
        switch self {
        case .none: nil
        case .keychain: "keychain"
        case let .inFile(value): value
        }
    }

    var isInFile: Bool {
        if case .inFile = self { return true }
        return false
    }
}

/// Secrets in the login keychain: the items run.sh reads with `security find-generic-password`
/// (service bbport-shadnet / bbport-huntersdream, account = the online id). The launcher only
/// stores, removes and checks for them; it never reads one back.
protocol SecretStore {
    func has(service: String, account: String) -> Bool
    func store(service: String, account: String, secret: String) throws
    func remove(service: String, account: String)
}

/// The launcher's view of the online keys in bbport.ini (scripts/launch_settings.sh turns them
/// into run.sh's environment). Keys it does not offer (community_server, shadnet_webapi) stay
/// in the IniFile.
struct OnlineSettings: Equatable {
    static let defaultShadnetServer = "srv.shadps4.net:31313"
    static let defaultP2PPort = 3658
    static let shadnetService = "bbport-shadnet"
    static let huntersDreamService = "bbport-huntersdream"

    var online = false
    var pool = OnlinePool.huntersdream
    /// Empty: the default server.
    var shadnetServer = ""
    var shadnetNpid = ""
    var shadnetPassword = StoredSecret.none
    var thdOnlineId = ""
    var thdToken = StoredSecret.none
    var upnp = true
    var p2pPort = OnlineSettings.defaultP2PPort

    init() {}

    init(ini: IniFile) {
        // Flags as the shell reads them: exactly 0 or 1, anything else is ignored.
        func flag(_ key: String) -> Bool? {
            guard let v = ini[key], v == "0" || v == "1" else { return nil }
            return v == "1"
        }
        if let v = flag("online") { online = v }
        shadnetServer = ini["shadnet_server"] ?? ""
        shadnetNpid = ini["shadnet_npid"] ?? ""
        shadnetPassword = StoredSecret(iniValue: ini["shadnet_password"])
        thdOnlineId = ini["thd_online_id"] ?? ""
        thdToken = StoredSecret(iniValue: ini["thd_token"])
        if let v = ini["online_pool"], let p = OnlinePool(rawValue: v) {
            pool = p
        } else {
            // launch_settings.sh: a file naming only a shadNet account is from before the pool key.
            pool = !shadnetNpid.isEmpty && thdOnlineId.isEmpty ? .shadnet : .huntersdream
        }
        if let v = flag("upnp") { upnp = v }
        if let v = ini["p2p_port"], let port = Self.port(from: v) { p2pPort = port }
    }

    /// A port as launch_settings.sh accepts it: one to five digits, 0 (any free port) to 65535.
    static func port(from text: String) -> Int? {
        guard !text.isEmpty, text.count <= 5, text.allSatisfy({ $0.isASCII && $0.isNumber }),
              let port = Int(text), port <= 65535 else { return nil }
        return port
    }

    /// Writes the online keys into `ini`; every other line stays. An empty server or account
    /// removes its key (the defaults apply).
    func apply(to ini: inout IniFile) {
        ini["online"] = online ? "1" : "0"
        ini["online_pool"] = pool.rawValue
        let server = shadnetServer.trimmingCharacters(in: .whitespaces)
        ini["shadnet_server"] = server.isEmpty ? nil : server
        let npid = shadnetNpid.trimmingCharacters(in: .whitespaces)
        ini["shadnet_npid"] = npid.isEmpty ? nil : npid
        ini["shadnet_password"] = shadnetPassword.iniValue
        ini["thd_online_id"] = thdOnlineId.isEmpty ? nil : thdOnlineId
        ini["thd_token"] = thdToken.iniValue
        ini["upnp"] = upnp ? "1" : "0"
        ini["p2p_port"] = String(p2pPort)
    }

    /// A password or token written in the file: run.sh wants that file owner-only.
    var hasSecretInFile: Bool { shadnetPassword.isInFile || thdToken.isInFile }

    /// Signed in to The Hunter's Dream: an online id whose token is in the file or the keychain.
    func signedIn(secrets: SecretStore) -> Bool {
        !thdOnlineId.isEmpty && (thdToken.isInFile || secrets.has(service: Self.huntersDreamService, account: thdOnlineId))
    }

    /// The shadNet password for the account name is in the file or the keychain.
    func shadnetPasswordStored(secrets: SecretStore) -> Bool {
        let npid = shadnetNpid.trimmingCharacters(in: .whitespaces)
        return !npid.isEmpty && (shadnetPassword.isInFile || secrets.has(service: Self.shadnetService, account: npid))
    }

    /// Keeps a typed shadNet password in the keychain under the account name (a password in
    /// the file is replaced: the ini then says `keychain`).
    mutating func storeShadnetPassword(_ password: String, secrets: SecretStore) throws {
        let npid = shadnetNpid.trimmingCharacters(in: .whitespaces)
        try secrets.store(service: Self.shadnetService, account: npid, secret: password)
        shadnetPassword = .keychain
    }

    /// The Hunter's Dream account after a Discord sign-in: the token goes to the keychain, the
    /// pool becomes The Hunter's Dream (as scripts/online.py signin --keychain writes it).
    mutating func signIn(onlineId: String, token: String, secrets: SecretStore) throws {
        try secrets.store(service: Self.huntersDreamService, account: onlineId, secret: token)
        thdOnlineId = onlineId
        thdToken = .keychain
        pool = .huntersdream
    }

    /// Forgets the account: the keychain item and both keys.
    mutating func signOut(secrets: SecretStore) {
        if !thdOnlineId.isEmpty {
            secrets.remove(service: Self.huntersDreamService, account: thdOnlineId)
        }
        thdOnlineId = ""
        thdToken = .none
    }
}
