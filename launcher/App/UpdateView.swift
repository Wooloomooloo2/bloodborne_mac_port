// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// The footer's version, or the newer release when there is one (opens the update window).
struct UpdateBadge: View {
    @ObservedObject var updater: Updater

    var body: some View {
        if let update = updater.available {
            Button("Version \(update.version.text) is available") { updater.showing = true }
                .buttonStyle(.link)
                .font(.caption)
        } else {
            Text("Version \(updater.currentText)").font(.caption).foregroundStyle(.secondary)
                .textSelection(.enabled)
        }
    }
}

/// The update window: what is new, and download and install (a sheet on the launcher window).
struct UpdateView: View {
    @ObservedObject var model: LauncherModel
    @ObservedObject var updater: Updater

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            content
            Divider()
            Toggle("Check for updates automatically", isOn: Binding(
                get: { model.config.checkUpdates == true }, set: { model.setUpdatePreference(automatic: $0) }))
            Toggle("Include test builds", isOn: Binding(
                get: { model.config.testBuilds }, set: { model.setUpdatePreference(testBuilds: $0) }))
                .help("Also offer pre-releases and test builds: early fixes that may not be finished.")
            if model.isBetaApp {
                Text("This is the online play beta. It updates to newer betas only; the regular Bloodborne app next to it updates itself. To stop testing, turn the beta off in the regular app's update window.")
                    .font(.caption).foregroundStyle(.secondary)
            } else {
                Toggle("Online play beta (0.7)", isOn: Binding(
                    get: { model.onlineBeta }, set: { model.setOnlineBeta($0) }))
                    .disabled(updater.busy)
                    .help("Offer the online play beta, installed as a second app (Bloodborne Beta) next to this one: the game's own co-op and messages through a community server. Unfinished.")
            }
            buttons
        }
        .padding(20)
        .frame(width: 560)
    }

    @ViewBuilder private var content: some View {
        switch updater.status {
        case .idle, .checking:
            HStack(spacing: 10) {
                ProgressView().controlSize(.small)
                Text("Checking for updates…")
            }
        case .upToDate:
            Text("You're up to date").font(.headline)
            Text("Bloodborne \(updater.currentText) is the newest version available.")
                .foregroundStyle(.secondary)
        case let .failed(message):
            Text("Update").font(.headline)
            Text(message).foregroundStyle(.red).textSelection(.enabled)
        case let .installed(message):
            Text("Bloodborne Beta is installed").font(.headline)
            Text(message).foregroundStyle(.secondary).textSelection(.enabled)
        case let .available(update), let .downloading(update, _), let .installing(update):
            let besides = update.isBeta && !updater.isBetaApp
            Text(besides ? "The online play beta is available" : "A new version of Bloodborne is available").font(.headline)
            Text(besides ? "Version \(update.version.text) installs as Bloodborne Beta next to this app; this version (\(updater.currentText)) stays."
                 : "Version \(update.version.text) is available; you have \(updater.currentText).")
                .foregroundStyle(.secondary)
            ScrollView {
                Text(notes(update))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .textSelection(.enabled)
                    .padding(8)
            }
            .frame(height: 220)
            .background(.background.secondary, in: RoundedRectangle(cornerRadius: 6))
            if case let .downloading(_, fraction) = updater.status {
                ProgressView("Downloading…", value: fraction)
            } else if case .installing = updater.status {
                ProgressView("Installing…").progressViewStyle(.linear)
            } else if let problem = model.updateInstallProblem {
                Text(problem).font(.caption).foregroundStyle(.secondary)
            } else if model.running {
                Text("Quit the game to install the update.").font(.caption).foregroundStyle(.secondary)
            } else {
                Text(update.isBeta && !updater.isBetaApp
                     ? "Bloodborne Beta is put next to this app; nothing here changes. Your saves and settings stay where they are."
                     : "The app is replaced and opens again; your saves and settings stay where they are.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    @ViewBuilder private var buttons: some View {
        HStack {
            if let update = updater.available {
                Button("Skip This Version", action: model.skipUpdate).disabled(updater.busy)
                Button("Release Page") { NSWorkspace.shared.open(update.page) }
                Spacer()
                Button("Later") { updater.dismiss() }.keyboardShortcut(.cancelAction).disabled(updater.busy)
                if model.updateInstallProblem == nil {
                    Button(update.isBeta && !updater.isBetaApp ? "Install Beta" : "Install Update",
                           action: model.installUpdate)
                        .keyboardShortcut(.defaultAction)
                        .disabled(updater.busy || model.running)
                }
            } else {
                Spacer()
                Button("OK") { updater.dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
    }

    /// The release notes are Markdown: inline styles and links render, the rest stays as written.
    private func notes(_ update: ReleaseUpdate) -> AttributedString {
        let text = update.notes.isEmpty ? update.title : update.notes
        return (try? AttributedString(markdown: text, options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace)))
            ?? AttributedString(text)
    }
}
