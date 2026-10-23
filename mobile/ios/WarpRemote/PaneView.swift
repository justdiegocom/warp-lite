import SwiftUI

/// Warp's accent (#19AAD8).
let accent = Color(red: 0.10, green: 0.67, blue: 0.85)

/// One pane while it is on screen: asks the computer about once a second what changed (nothing runs there for it
/// otherwise), and types into it.
@MainActor
final class PaneModel: ObservableObject {
    let session: Session
    private let core: Core
    @Published var kind = ""
    @Published var status = ""
    @Published var title = ""
    @Published var items: [ChatItem] = []
    @Published var screen: [String] = []
    @Published var prompt: Prompt?
    @Published var typing = false
    @Published var note = ""
    @Published var more = false
    @Published var error: String?
    @Published var loaded = false
    /// Bumped when new content arrives at the bottom (the view follows it).
    @Published var tail = 0
    private var rev: UInt64 = 0

    init(session: Session, core: Core) {
        self.session = session
        self.core = core
    }

    private func req(_ op: String, _ extra: [String: Any] = [:]) -> [String: Any] {
        var r: [String: Any] = ["op": op, "window": session.window, "pane_id": session.pane_id ?? 0]
        r.merge(extra) { _, new in new }
        return r
    }

    func run() async {
        while !Task.isCancelled {
            await poll()
            try? await Task.sleep(for: .milliseconds(900))
        }
    }

    func poll() async {
        let a = await core.call(req("watch", ["since": rev]))
        guard a.ok, let p = a.pane else {
            if !Task.isCancelled { error = a.error ?? "No answer." }
            return
        }
        error = nil
        let before = (items.last?.id, items.count, screen)
        if p.reset ?? false {
            items = []
            screen = []
            more = p.more ?? false
        }
        for it in p.items ?? [] {
            if let i = items.firstIndex(where: { $0.id == it.id }) { items[i] = it } else { items.append(it) }
        }
        if let s = p.screen { screen = s }
        kind = p.kind
        status = p.status ?? ""
        title = p.title ?? ""
        if prompt != p.prompt { prompt = p.prompt }
        typing = p.typing ?? false
        note = p.note ?? ""
        rev = p.rev
        loaded = true
        if before.0 != items.last?.id || before.1 != items.count || before.2 != screen { tail += 1 }
    }

    func loadOlder() async {
        guard more, let first = items.first?.id else { return }
        let a = await core.call(req("watch", ["before": first]))
        guard a.ok, let p = a.pane else { return }
        let old = (p.items ?? []).filter { o in !items.contains(where: { $0.id == o.id }) }
        items.insert(contentsOf: old, at: 0)
        more = p.more ?? false
    }

    /// Types text and Enter, as on the computer.
    func send(_ text: String) async -> Bool {
        let a = await core.call(req("input", ["text": text, "keys": ["enter"]]))
        if !a.ok { error = a.error }
        await poll()
        return a.ok
    }

    func key(_ k: String) async {
        let a = await core.call(req("input", ["keys": [k]]))
        if !a.ok { error = a.error }
        await poll()
    }

    func choose(_ option: Int) async {
        guard let p = prompt else { return }
        let a = await core.call(req("input", ["option": option, "prompt": p.id]))
        if !a.ok { error = a.error }
        prompt = nil  // it answers; the next poll shows what follows
        try? await Task.sleep(for: .milliseconds(400))
        await poll()
    }

    func stop() async {
        _ = await core.call(req("unwatch"))
    }
}

struct PaneView: View {
    @StateObject private var m: PaneModel
    @State private var draft = ""
    @FocusState private var replying: Bool

    init(session: Session, core: Core) {
        _m = StateObject(wrappedValue: PaneModel(session: session, core: core))
    }

    private var heading: String {
        if !m.title.isEmpty { return m.title }
        return m.session.title
    }

    var body: some View {
        VStack(spacing: 0) {
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 14) {
                        if m.kind == "chat" && m.more {
                            Text("Pull down for earlier messages")
                                .font(.caption).foregroundStyle(.secondary).frame(maxWidth: .infinity)
                        }
                        if !m.loaded {
                            ProgressView().frame(maxWidth: .infinity).padding(.top, 40)
                        } else if m.kind == "chat" {
                            if m.items.isEmpty {
                                Text("No messages yet.").foregroundStyle(.secondary).frame(maxWidth: .infinity).padding(.top, 40)
                            }
                            ForEach(m.items) { ItemRow(item: $0) }
                        } else {
                            ScreenText(lines: m.screen)
                        }
                        Color.clear.frame(height: 1).id("end")
                    }
                    .padding(.horizontal, 14)
                    .padding(.vertical, 12)
                }
                .scrollDismissesKeyboard(.interactively)
                .refreshable { await m.loadOlder() }
                .onChange(of: m.tail) { _, _ in
                    withAnimation(.easeOut(duration: 0.2)) { proxy.scrollTo("end", anchor: .bottom) }
                }
                .onChange(of: replying) { _, now in
                    if now { proxy.scrollTo("end", anchor: .bottom) }
                }
            }
            if let p = m.prompt {
                PromptCard(prompt: p, enabled: m.typing) { i in Task { await m.choose(i) } }
                    .transition(.move(edge: .bottom).combined(with: .opacity))
            }
            if let e = m.error {
                Text(e).font(.footnote).foregroundStyle(.red).padding(.horizontal).padding(.top, 6)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            ReplyBar(draft: $draft, typing: m.typing, focused: $replying, send: {
                let text = draft
                replying = false  // sent: the keyboard goes, to watch what it does
                Task { if await m.send(text) { draft = "" } }
            }, key: { k in Task { await m.key(k) } })
        }
        .animation(.easeInOut(duration: 0.2), value: m.prompt)
        .background(Color(white: 0.02).ignoresSafeArea())
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .principal) {
                VStack(spacing: 2) {
                    Text(heading).font(.subheadline.weight(.semibold)).lineLimit(1)
                    HStack(spacing: 6) {
                        StatusChip(status: m.status)
                        if !m.note.isEmpty && m.kind == "chat" {
                            Text(m.note).font(.caption2).foregroundStyle(.secondary).lineLimit(1)
                        }
                    }
                }
            }
        }
        .task { await m.run() }
        .onDisappear { Task { await m.stop() } }
    }
}

struct StatusChip: View {
    var status: String

    private var look: (String, Color)? {
        switch status {
        case "working": return ("Working", .orange)
        case "waiting": return ("Waiting for you", .yellow)
        case "finished": return ("Finished", .green)
        case "running": return ("Running", accent)
        case "idle": return ("Idle", .secondary)
        default: return nil
        }
    }

    var body: some View {
        if let (label, color) = look {
            HStack(spacing: 4) {
                if status == "working" {
                    ProgressView().controlSize(.mini).tint(color)
                } else {
                    Circle().fill(color).frame(width: 6, height: 6)
                }
                Text(label).font(.caption2.weight(.semibold)).foregroundStyle(color)
            }
            .padding(.horizontal, 8).padding(.vertical, 2)
            .background(Capsule().fill(color.opacity(0.15)))
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(label)
            .accessibilityIdentifier("status")
        }
    }
}

struct ItemRow: View {
    var item: ChatItem
    @State private var open = false

    var body: some View {
        switch item.role {
        case "user":
            HStack {
                Spacer(minLength: 48)
                Text(item.text)
                    .padding(.horizontal, 14).padding(.vertical, 10)
                    .background(RoundedRectangle(cornerRadius: 18).fill(accent.opacity(0.28)))
                    .textSelection(.enabled)
            }
            .accessibilityIdentifier("item-user")
        case "assistant":
            MarkdownText(text: item.text)
                .frame(maxWidth: .infinity, alignment: .leading)
                .accessibilityIdentifier("item-assistant")
        case "tool":
            VStack(alignment: .leading, spacing: 6) {
                Button {
                    withAnimation(.easeInOut(duration: 0.15)) { open.toggle() }
                } label: {
                    HStack(spacing: 8) {
                        Image(systemName: icon).font(.footnote).frame(width: 18)
                            .foregroundStyle(item.failed == true ? .red : item.pending == true ? .orange : .secondary)
                        Text(item.text).font(.subheadline).foregroundStyle(.secondary).lineLimit(1)
                        Spacer(minLength: 0)
                        if !(item.detail ?? "").isEmpty {
                            Image(systemName: open ? "chevron.up" : "chevron.down").font(.caption2).foregroundStyle(.tertiary)
                        }
                    }
                    .frame(minHeight: 28)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                if open, let d = item.detail, !d.isEmpty {
                    Text(d)
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .textSelection(.enabled)
                        .padding(10)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(RoundedRectangle(cornerRadius: 8).fill(Color.white.opacity(0.05)))
                }
            }
            .accessibilityIdentifier("item-tool")
        default:
            Text(item.text).font(.caption).foregroundStyle(.secondary).frame(maxWidth: .infinity)
        }
    }

    private var icon: String {
        let t = item.text
        if t.hasPrefix("Ran") { return "terminal" }
        if t.hasPrefix("Edited") || t.hasPrefix("Wrote") { return "pencil" }
        if t.hasPrefix("Read") { return "doc.text" }
        if t.hasPrefix("Searched") || t.hasPrefix("Listed") { return "magnifyingglass" }
        if t.hasPrefix("Fetched") { return "globe" }
        if t.hasPrefix("Agent") { return "person.2" }
        if t.hasPrefix("Asked") { return "questionmark.bubble" }
        return "wrench.and.screwdriver"
    }
}

/// Markdown as the agents write it: inline styles, code blocks in monospace, headings bold, lists with bullets.
struct MarkdownText: View {
    var text: String

    private struct Block: Identifiable {
        var id: Int
        var code: Bool
        var text: String
    }

    private var blocks: [Block] {
        var out: [Block] = []
        var cur: [Substring] = []
        var code = false
        func flush() {
            let t = cur.joined(separator: "\n")
            if !t.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty { out.append(Block(id: out.count, code: code, text: t)) }
            cur = []
        }
        for line in text.split(separator: "\n", omittingEmptySubsequences: false) {
            if line.trimmingCharacters(in: .whitespaces).hasPrefix("```") {
                flush()
                code.toggle()
            } else {
                cur.append(line)
            }
        }
        flush()
        return out
    }

    private func styled(_ s: String) -> AttributedString {
        let lines = s.split(separator: "\n", omittingEmptySubsequences: false).map { l -> String in
            let t = l.trimmingCharacters(in: .whitespaces)
            if t.hasPrefix("#") {
                let h = t.drop(while: { $0 == "#" }).trimmingCharacters(in: .whitespaces)
                return "**\(h)**"
            }
            let lead = l.prefix(while: { $0 == " " })
            if t.hasPrefix("- ") || t.hasPrefix("* ") { return "\(lead)• \(t.dropFirst(2))" }
            return String(l)
        }
        let md = lines.joined(separator: "\n")
        let opts = AttributedString.MarkdownParsingOptions(interpretedSyntax: .inlineOnlyPreservingWhitespace)
        return (try? AttributedString(markdown: md, options: opts)) ?? AttributedString(s)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            ForEach(blocks) { b in
                if b.code {
                    ScrollView(.horizontal, showsIndicators: false) {
                        Text(b.text).font(.system(.footnote, design: .monospaced)).padding(10)
                    }
                    .background(RoundedRectangle(cornerRadius: 8).fill(Color.white.opacity(0.06)))
                } else {
                    Text(styled(b.text)).font(.body).textSelection(.enabled)
                }
            }
        }
    }
}

/// A pane that isn't a chat: its text, wrapped to the phone's width. Rules drawn across the terminal (───) would wrap
/// into several lines of dashes on a phone: they become one short rule.
struct ScreenText: View {
    var lines: [String]

    private static let ruleChars = Set("─━═╌┄┈-╭╮╰╯┌┐└┘│ ")

    private var text: String {
        let trimmed = lines.reversed().drop(while: { $0.trimmingCharacters(in: .whitespaces).isEmpty }).reversed()
        return trimmed.map { l -> String in
            let t = l.trimmingCharacters(in: .whitespaces)
            return t.count >= 12 && t.allSatisfy({ Self.ruleChars.contains($0) }) ? String(repeating: "─", count: 24) : l
        }.joined(separator: "\n")
    }

    var body: some View {
        Text(text)
            .font(.system(size: 12, design: .monospaced))
            .foregroundStyle(Color(white: 0.87))
            .textSelection(.enabled)
            .frame(maxWidth: .infinity, alignment: .leading)
            .accessibilityIdentifier("screen")
    }
}

/// The question the pane asks, with its options as big buttons.
struct PromptCard: View {
    var prompt: Prompt
    var enabled: Bool
    var choose: (Int) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            // A short question stands out; a long one (a dialog's paragraph) stays readable without pushing the options away.
            Text(prompt.question)
                .font(prompt.question.count < 90 ? .headline : .subheadline.weight(.medium))
                .lineLimit(6)
                .fixedSize(horizontal: false, vertical: true)
            if let c = prompt.context, !c.isEmpty {
                Text(c.prefix(4).joined(separator: "\n"))
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(.secondary)
                    .lineLimit(5)
                    .padding(8)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(RoundedRectangle(cornerRadius: 8).fill(Color.white.opacity(0.05)))
            }
            ForEach(Array(prompt.options.enumerated()), id: \.offset) { i, o in
                Button { choose(i) } label: {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(o.label).font(.body.weight(.semibold)).multilineTextAlignment(.leading)
                        if let h = o.hint, !h.isEmpty {
                            Text(h).font(.caption).opacity(0.75).lineLimit(2).multilineTextAlignment(.leading)
                        }
                    }
                    .frame(maxWidth: .infinity, minHeight: 46, alignment: .leading)
                    .padding(.horizontal, 14).padding(.vertical, 4)
                    .foregroundStyle(style(o).0)
                    .background(RoundedRectangle(cornerRadius: 12).fill(style(o).1))
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityIdentifier("option-\(i)")
            }
            if !enabled {
                Text("Allow typing for this phone in Warp to answer: title-bar menu → Paired devices.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .disabled(!enabled)
        .opacity(enabled ? 1 : 0.6)
        .padding(14)
        .background(Color(white: 0.09))
        .overlay(alignment: .top) { Rectangle().fill(accent.opacity(0.6)).frame(height: 1) }
    }

    /// The first «Yes» is the main choice; a «No» is red.
    private func style(_ o: PromptOption) -> (Color, Color) {
        let l = o.label.lowercased()
        if l.hasPrefix("no") { return (.red, Color.red.opacity(0.14)) }
        if o == prompt.options.first(where: { $0.label.lowercased().hasPrefix("yes") }) { return (.black, accent) }
        return (.primary, Color.white.opacity(0.08))
    }
}

/// Text box and the keys a terminal needs, at the bottom.
struct ReplyBar: View {
    @Binding var draft: String
    var typing: Bool
    var focused: FocusState<Bool>.Binding
    var send: () -> Void
    var key: (String) -> Void

    private let keys: [(String, String)] = [("Esc", "esc"), ("Enter", "enter"), ("↑", "up"), ("↓", "down"), ("Tab", "tab"), ("Ctrl+C", "ctrl_c")]

    var body: some View {
        VStack(spacing: 8) {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 8) {
                    ForEach(keys, id: \.1) { k in
                        Button { key(k.1) } label: {
                            Text(k.0).font(.system(.subheadline, design: .monospaced).weight(.medium))
                                .frame(minWidth: 44, minHeight: 34)
                                .padding(.horizontal, 6)
                                .background(Capsule().fill(Color.white.opacity(0.09)))
                        }
                        .buttonStyle(.plain)
                        .accessibilityIdentifier("key-\(k.1)")
                    }
                }
                .padding(.horizontal, 14)
            }
            HStack(alignment: .bottom, spacing: 8) {
                TextField(typing ? "Message" : "Typing is off for this phone", text: $draft, axis: .vertical)
                    .lineLimit(1...5)
                    .focused(focused)
                    .padding(.horizontal, 14).padding(.vertical, 10)
                    .background(RoundedRectangle(cornerRadius: 20).fill(Color.white.opacity(0.08)))
                    .accessibilityIdentifier("reply")
                Button(action: send) {
                    Image(systemName: "arrow.up.circle.fill").font(.system(size: 34))
                        .foregroundStyle(draft.isEmpty || !typing ? Color.secondary : accent)
                }
                .disabled(draft.isEmpty || !typing)
                .accessibilityIdentifier("reply-send")
            }
            .padding(.horizontal, 14)
        }
        .disabled(!typing)
        .padding(.vertical, 8)
        .background(Color(white: 0.06).ignoresSafeArea(edges: .bottom))
    }
}
