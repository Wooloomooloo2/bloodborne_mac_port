// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct AchievementsView: View {
    @ObservedObject var model: LauncherModel
    @State private var history: [Int: Date] = [:]
    @State private var error: String?
    @State private var search = ""
    @State private var status = 0
    @State private var group = 0
    @State private var reveal = false
    @State private var artwork: [Int: NSImage] = [:]

    private var filtered: [Achievement] {
        Achievement.catalog.filter { trophy in
            let earned = history[trophy.id] != nil
            return (status == 0 || (status == 1) == earned) &&
                (group == 0 || (group == 2) == trophy.dlc) &&
                (search.isEmpty || trophy.name.localizedCaseInsensitiveContains(search) ||
                 ((earned || reveal) && trophy.description.localizedCaseInsensitiveContains(search)))
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .firstTextBaseline) {
                Text("Achievements").font(.title2.bold())
                Spacer()
                Text("\(history.count) / 40 earned").monospacedDigit()
            }
            ProgressView(value: Double(history.count), total: 40)
            Text("Bloodborne \(history.keys.filter { $0 < 34 }.count) / 34 · The Old Hunters \(history.keys.filter { $0 >= 34 }.count) / 6")
                .font(.caption).foregroundStyle(.secondary)
            HStack {
                Toggle("Show in-game unlock banners", isOn: $model.settings.achievementBanners)
                    .disabled(model.running)
                    .help("Applies when you start the game. Use Settings → Achievements in game to change the current session.")
                Spacer()
                Toggle("Reveal locked descriptions", isOn: $reveal)
            }
            Text("Platinum requires the 33 base-game trophies. DLC is optional. In game: F2 or ⌘⇧T.")
                .font(.caption).foregroundStyle(.secondary)
            if let error { Text(error).foregroundStyle(.red).font(.caption) }
            HStack {
                TextField("Search achievements", text: $search)
                    .textFieldStyle(.roundedBorder)
                    .accessibilityLabel("Search achievements")
                Picker("Status", selection: $status) {
                    Text("All").tag(0); Text("Earned").tag(1); Text("Locked").tag(2)
                }.frame(width: 155)
                Picker("Game", selection: $group) {
                    Text("Base game and DLC").tag(0); Text("Bloodborne").tag(1); Text("The Old Hunters").tag(2)
                }.frame(width: 230)
            }
            List(filtered) { trophy in
                HStack(alignment: .top, spacing: 12) {
                    Group {
                        if let image = artwork[trophy.id] {
                            Image(nsImage: image).resizable().scaledToFit()
                        } else {
                            Image(systemName: history[trophy.id] == nil ? "trophy" : "trophy.fill")
                                .font(.title2).foregroundStyle(color(trophy.grade))
                        }
                    }.frame(width: 40, height: 40).accessibilityHidden(true)
                    VStack(alignment: .leading, spacing: 4) {
                        Text(trophy.name).font(.headline)
                        Text("\(trophy.grade.capitalized) · \(trophy.dlc ? "The Old Hunters" : "Bloodborne")")
                            .font(.caption).foregroundStyle(.secondary)
                        if history[trophy.id] != nil || reveal {
                            Text(trophy.description).font(.callout)
                        }
                    }
                    Spacer()
                    if let date = history[trophy.id] {
                        VStack(alignment: .trailing) {
                            Label("Earned", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
                            Text(date, format: .dateTime.year().month().day()).font(.caption)
                        }
                    } else {
                        Text("Locked").foregroundStyle(.secondary)
                    }
                }.padding(.vertical, 7)
            }
            .overlay {
                if filtered.isEmpty { ContentUnavailableView.search(text: search) }
            }
            Text("Progress is read from your user folder and refreshes while this page is open.")
                .font(.caption).foregroundStyle(.secondary)
        }
        .padding(20)
        .task(id: model.config.gameDir) {
            artwork = [:]
            if let game = model.config.gameDir {
                let images = await Task.detached { AchievementArtwork.read(game: game) }.value
                guard !Task.isCancelled else { return }
                artwork = images.compactMapValues { NSImage(data: $0) }
            }
        }
        .task(id: model.trophyURL) {
            let url = model.trophyURL
            while !Task.isCancelled {
                let result = await Task.detached { Result { try AchievementHistory.read(url) } }.value
                guard !Task.isCancelled else { return }
                switch result {
                case .success(let value): history = value; error = nil
                case .failure(let failure): error = "Could not read achievements: \(failure.localizedDescription)"
                }
                do { try await Task.sleep(for: .seconds(2)) } catch { return }
            }
        }
    }

    private func color(_ grade: String) -> Color {
        switch grade {
        case "platinum": .cyan
        case "gold": .yellow
        case "silver": .gray
        default: .brown
        }
    }
}
