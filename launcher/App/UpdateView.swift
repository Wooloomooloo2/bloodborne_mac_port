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
        case let .available(update), let .downloading(update, _), let .installing(update):
            Text("A new version of Bloodborne is available").font(.headline)
            Text("Version \(update.version.text) is available; you have \(updater.currentText).")
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
                Text("The app is replaced and opens again; your saves and settings stay where they are.")
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
                    Button("Install Update", action: model.installUpdate)
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
