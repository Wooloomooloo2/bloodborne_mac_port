// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// The Saves & Folders page: where the game, saves and backups are, and the save backups
/// (launcher/Model/SaveBackup.swift): made automatically before beta launches, new versions and
/// new mods or cheats, or with Back Up Now; restore, show, delete.
struct SavesView: View {
    @ObservedObject var model: LauncherModel
    @State private var selection: SaveBackupEntry.ID?

    var body: some View {
        HStack(alignment: .top, spacing: 0) {
            Form {
                Section("Folders") {
                    FolderRow(title: "Game folder", path: model.config.gameDir?.path ?? "Not chosen",
                              problem: model.checkingGame ? nil : model.gameProblem,
                              choose: model.chooseGameFolder, reset: model.resetGameFolder)
                    FolderRow(title: "Save folder", path: model.saveDir.path, problem: model.saveProblem,
                              choose: model.chooseSaveFolder, reset: model.resetSaveFolder)
                    FolderRow(title: "Backup folder", path: model.backupDir.path, problem: model.backupProblem,
                              choose: model.chooseBackupFolder, reset: model.resetBackupFolder)
                }
                if model.isBetaApp {
                    Section("Beta saves") {
                        Toggle("Beta uses its own copy of the saves", isOn: Binding(
                            get: { model.betaOwnSaves }, set: { model.setBetaOwnSaves($0) }))
                        Text(model.betaOwnSaves
                             ? "The beta plays its own copy: progress here (co-op too) doesn't reach the regular version, and the beta can't harm its saves."
                             : "Shared with the regular version: progress carries over both ways. Your saves are backed up before every beta launch.")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
            .formStyle(.grouped)
            Form {
                Section("Save backups") {
                    HStack {
                        Button(model.backingUp ? "Backing Up…" : "Back Up Now", action: model.backUpNow)
                            .disabled(model.backingUp)
                        if model.backingUp { ProgressView().controlSize(.small) }
                        Spacer()
                        Button("Show in Finder") {
                            if let entry = selected { NSWorkspace.shared.activateFileViewerSelecting([entry.folder]) }
                            else { NSWorkspace.shared.open(model.backupDir) }
                        }
                    }
                    if let message = model.backupMessage {
                        Text(message).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                    }
                    Table(model.saveBackups, selection: $selection) {
                        TableColumn("Date") { Text($0.date.map { $0.formatted(date: .abbreviated, time: .shortened) } ?? $0.name) }
                            .width(min: 120, ideal: 140)
                        TableColumn("Why") { Text($0.reason.label) }
                            .width(min: 140, ideal: 170)
                        TableColumn("Version") { Text($0.info?.version ?? "") }
                            .width(min: 60, ideal: 90)
                    }
                    .frame(minHeight: 220)
                    HStack {
                        Button("Delete…") { if let entry = selected { model.deleteBackup(entry) } }
                            .disabled(selected == nil)
                        Spacer()
                        Button("Restore…") { if let entry = selected { model.restoreBackup(entry) } }
                            .disabled(selected == nil)
                    }
                    Text("Made automatically before every beta launch, the first launch of a new version and the first with new mods or cheats (the newest \(SaveBackup.keep) of those stay), or with Back Up Now (kept until you delete them).")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            .formStyle(.grouped)
        }
        .onAppear(perform: model.refreshBackups)
    }

    private var selected: SaveBackupEntry? { model.saveBackups.first { $0.id == selection } }
}
