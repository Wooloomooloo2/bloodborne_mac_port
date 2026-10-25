// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// The benchmark's results: report.md shown as text (headings, lines, tables), with Copy Report
/// and Show in Finder.
struct BenchmarkResultView: View {
    let result: BenchmarkResult
    @ObservedObject var runner: BenchmarkRunner
    @Environment(\.dismiss) private var dismiss
    @State private var copied = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(result.summary.complete ? "Benchmark results" : "Benchmark stopped").font(.title2.bold())
            if !result.summary.complete {
                Text("A pass did not finish; the report shows what was measured. The game's logs are in the results folder.")
                    .font(.callout).foregroundStyle(.secondary)
            }
            ScrollView {
                MarkdownText(markdown: result.report)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(12)
            }
            .background(Color(nsColor: .textBackgroundColor))
            .clipShape(RoundedRectangle(cornerRadius: 6))
            HStack {
                Button("Show in Finder", action: runner.showInFinder)
                Spacer()
                if copied {
                    Text("Copied").foregroundStyle(.secondary)
                }
                Button("Copy Report") {
                    runner.copyReport()
                    copied = true
                }
                Button("Close") { dismiss() }
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 760, height: 600)
    }
}

/// The few Markdown forms BenchmarkSummary.markdown writes: # and ## headings, - lines, > quotes,
/// | tables | and plain lines (with **bold**).
private struct MarkdownText: View {
    let markdown: String

    private enum Block {
        case heading(String, level: Int)
        case text(String, secondary: Bool)
        case table([[String]])
    }

    private var blocks: [Block] {
        var blocks: [Block] = []
        var rows: [[String]] = []
        func endTable() {
            if !rows.isEmpty { blocks.append(.table(rows)) }
            rows = []
        }
        for raw in markdown.split(separator: "\n", omittingEmptySubsequences: false) {
            let line = String(raw)
            if line.hasPrefix("|") {
                let cells = line.split(separator: "|").map { $0.trimmingCharacters(in: .whitespaces) }
                if !cells.allSatisfy({ $0.allSatisfy { $0 == "-" } }) { rows.append(cells) }
                continue
            }
            endTable()
            if line.hasPrefix("## ") {
                blocks.append(.heading(String(line.dropFirst(3)), level: 2))
            } else if line.hasPrefix("# ") {
                blocks.append(.heading(String(line.dropFirst(2)), level: 1))
            } else if line.hasPrefix("> ") {
                blocks.append(.text(String(line.dropFirst(2)), secondary: true))
            } else if !line.isEmpty {
                blocks.append(.text(line.hasPrefix("- ") ? "• " + line.dropFirst(2) : line, secondary: false))
            }
        }
        endTable()
        return blocks
    }

    private func inline(_ s: String) -> Text {
        Text((try? AttributedString(markdown: s)) ?? AttributedString(s))
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            ForEach(Array(blocks.enumerated()), id: \.offset) { _, block in
                switch block {
                case let .heading(text, level):
                    inline(text).font(level == 1 ? .title3.bold() : .headline).padding(.top, level == 1 ? 0 : 10)
                case let .text(text, secondary):
                    inline(text).foregroundStyle(secondary ? .secondary : .primary)
                case let .table(rows):
                    Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 3) {
                        ForEach(Array(rows.enumerated()), id: \.offset) { index, row in
                            GridRow {
                                ForEach(Array(row.enumerated()), id: \.offset) { _, cell in
                                    inline(cell).fontWeight(index == 0 ? .semibold : .regular)
                                }
                            }
                        }
                    }
                }
            }
        }
        .textSelection(.enabled)
        .font(.callout)
    }
}
