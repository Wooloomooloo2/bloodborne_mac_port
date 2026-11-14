// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// The save backups (launcher/Model/SaveBackup.swift): made automatically before beta launches,
/// new versions and new mods or cheats, or with Back Up Now; restore, show, delete.
struct BackupsView: View {
    @ObservedObject var model: LauncherModel
    @Environment(\.dismiss) private var dismiss
    @State private var selection: SaveBackupEntry.ID?

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Save backups").font(.title2.bold())
            Text("Made automatically before every beta launch, the first launch of a new version and the first with new mods or cheats (the newest \(SaveBackup.keep) of those stay), or with Back Up Now (kept until you delete them).")
                .font(.callout).foregroundStyle(.secondary)
            Table(model.saveBackups, selection: $selection) {
                TableColumn("Date") { Text($0.date.map { $0.formatted(date: .abbreviated, time: .shortened) } ?? $0.name) }
                    .width(min: 140, ideal: 160)
                TableColumn("Why") { Text($0.reason.label) }
                    .width(min: 150, ideal: 180)
                TableColumn("Version") { Text($0.info?.version ?? "") }
                    .width(min: 70, ideal: 110)
            }
            .frame(height: 260)
            if let message = model.backupMessage {
                Text(message).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
            }
            HStack {
                Button("Back Up Now", action: model.backUpNow).disabled(model.running)
                Button("Show in Finder") {
                    if let entry = selected { NSWorkspace.shared.activateFileViewerSelecting([entry.folder]) }
                    else { NSWorkspace.shared.open(model.modFiles.backups) }
                }
                Spacer()
                Button("Delete…") { if let entry = selected { model.deleteBackup(entry) } }
                    .disabled(selected == nil)
                Button("Restore…") { if let entry = selected { model.restoreBackup(entry) } }
                    .disabled(selected == nil || model.running)
                Button("Done") { model.backupMessage = nil; dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 620)
        .onAppear(perform: model.refreshBackups)
    }

    private var selected: SaveBackupEntry? { model.saveBackups.first { $0.id == selection } }
}
