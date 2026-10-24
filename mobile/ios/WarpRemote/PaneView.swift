import SwiftUI

/// Warp's accent (#19AAD8).
let accent = Color(red: 0.10, green: 0.67, blue: 0.85)

/// One pane (or one subagent of its Claude Code) while it is on screen: asks the computer about once a second what
/// changed (nothing runs there for it otherwise), and types into it.
@MainActor
final class PaneModel: ObservableObject {
    let session: Session
    /// A subagent's id: its own conversation, read-only. Empty for the pane itself.
    let agentId: String
    private let core: Core
    @Published var kind = ""
    @Published var agent = ""
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
    @Published var agents: [AgentInfo] = []
    @Published var turnStart: UInt64 = 0
    @Published var activity = ""
    /// Bumped when new content arrives at the bottom (the view follows it).
    @Published var tail = 0
    /// The clock live times are drawn with (how long it works, how long a subagent runs): moves with each answer while
    /// something runs.
    @Published var now = nowMs()
    /// A subagent of this pane is being opened: leaving the screen for it doesn't stop following the pane.
    var childOpen = false
    private var rev: UInt64 = 0

    var isSubagent: Bool { !agentId.isEmpty }

    init(session: Session, core: Core, agent: String = "") {
        self.session = session
        self.core = core
        self.agentId = agent
    }

    private func req(_ op: String, _ extra: [String: Any] = [:]) -> [String: Any] {
        var r: [String: Any] = ["op": op, "window": session.window, "pane_id": session.pane_id ?? 0]
        if isSubagent { r["agent"] = agentId }
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
            if !Task.isCancelled { set(\.error, a.error ?? "No answer.") }
            return
        }
        // Only what changed is set: an answer with nothing new redraws nothing (the phone stays idle).
        set(\.error, nil)
        let before = (items.last?.id, items.count, screen.count, screen.last)
        if p.reset ?? false {
            items = []
            screen = []
            set(\.more, p.more ?? false)
        }
        if let new = p.items, !new.isEmpty {
            var list = items
            for it in new {
                if let i = list.firstIndex(where: { $0.id == it.id }) { list[i] = it } else { list.append(it) }
            }
            set(\.items, list)
        }
        if let s = p.screen { set(\.screen, s) }
        if let list = p.agents { set(\.agents, list) }
        set(\.kind, p.kind)
        set(\.agent, p.agent ?? "")
        set(\.status, p.status ?? "")
        set(\.title, p.title ?? "")
        set(\.turnStart, p.turn_start ?? turnStart)
        set(\.activity, p.activity ?? "")
        set(\.prompt, p.prompt)
        set(\.typing, p.typing ?? false)
        set(\.note, p.note ?? "")
        rev = p.rev
        set(\.loaded, true)
        if status == "working" || agents.contains(where: \.running) || items.contains(where: { $0.agent?.running == true }) {
            now = nowMs()
        }
        if before.0 != items.last?.id || before.1 != items.count || before.2 != screen.count || before.3 != screen.last { tail += 1 }
    }

    private func set<T: Equatable>(_ key: ReferenceWritableKeyPath<PaneModel, T>, _ value: T) {
        if self[keyPath: key] != value { self[keyPath: key] = value }
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
    @EnvironmentObject var model: Model
    @StateObject private var m: PaneModel
    @State private var draft = ""
    @State private var showTerminal = false
    @FocusState private var replying: Bool
    private let fixedTitle: String

    init(session: Session, core: Core, agent: String = "", title: String = "") {
        _m = StateObject(wrappedValue: PaneModel(session: session, core: core, agent: agent))
        fixedTitle = title
    }

    private var heading: String {
        if !m.title.isEmpty { return m.title }
        if !fixedTitle.isEmpty { return fixedTitle }
        return m.session.title
    }

    /// Subagents can be opened (a computer that shows them, and not from inside one).
    private var canOpen: Bool { model.version >= 3 && !m.isSubagent }

    var body: some View {
        if m.session.pane_id == nil {
            UpdateWarpView(title: m.session.title)
        } else {
            pane
        }
    }

    private func open(_ a: AgentInfo) {
        guard canOpen, !a.id.isEmpty else { return }
        m.childOpen = true
        model.nav.append(SubagentRoute(session: m.session, agent: a.id, title: a.description ?? a.name))
    }

    private var quickReplies: [QuickReply] {
        guard !m.isSubagent, m.kind == "chat", m.status == "finished", m.prompt == nil, m.typing,
              let last = m.items.last, last.role == "assistant" else { return [] }
        return QuickReply.suggestions(for: last.text)
    }

    private var pane: some View {
        VStack(spacing: 0) {
            if !m.isSubagent {
                AgentChips(agents: m.agents, now: m.now, canOpen: canOpen, open: open)
            }
            ScrollViewReader { proxy in
                ScrollView {
                    // Not lazy: a lazy stack kept re-placing rows whose heights it had only estimated while held to the
                    // end, and never settled (a conversation shows at most a few hundred items, parsed once).
                    VStack(alignment: .leading, spacing: 12) {
                        if m.kind == "chat" && m.more {
                            Text("Pull down for earlier messages")
                                .font(.caption).foregroundStyle(.secondary).frame(maxWidth: .infinity)
                        }
                        if !m.loaded {
                            ProgressView().frame(maxWidth: .infinity).padding(.top, 40)
                        } else if m.kind == "chat" {
                            if m.items.isEmpty {
                                Text(m.isSubagent ? "Nothing yet." : "No messages yet.")
                                    .foregroundStyle(.secondary).frame(maxWidth: .infinity).padding(.top, 40)
                            }
                            ChatRows(items: m.items, now: m.now, canOpen: canOpen, open: open)
                        } else if !m.agent.isEmpty && !showTerminal {
                            AgentWaitingView(agent: m.agent, note: m.note, showTerminal: $showTerminal)
                        } else {
                            ScreenText(lines: m.screen)
                        }
                        Color.clear.frame(height: 1).id("end")
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 12)
                }
                .scrollDismissesKeyboard(.interactively)
                .refreshable { await m.loadOlder() }
                .onChange(of: m.tail) { old, _ in
                    // The first load jumps to the end; what comes later scrolls into view.
                    if old == 0 {
                        proxy.scrollTo("end", anchor: .bottom)
                    } else {
                        withAnimation(.easeOut(duration: 0.2)) { proxy.scrollTo("end", anchor: .bottom) }
                    }
                }
                .onChange(of: replying) { _, now in
                    if now { proxy.scrollTo("end", anchor: .bottom) }
                }
            }
            if let p = m.prompt, !m.isSubagent {
                PromptCard(prompt: p, enabled: m.typing) { i in Task { await m.choose(i) } }
                    .transition(.move(edge: .bottom).combined(with: .opacity))
            } else if !quickReplies.isEmpty {
                QuickReplyBar(replies: quickReplies) { r in Task { _ = await m.send(r.text) } }
            }
            if let e = m.error {
                Text(e).font(.footnote).foregroundStyle(.red).padding(.horizontal).padding(.top, 6)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            if m.isSubagent {
                Text("A subagent's conversation: it can't be typed into from here.")
                    .font(.caption).foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity).padding(.vertical, 10)
                    .background(Color(white: 0.06).ignoresSafeArea(edges: .bottom))
            } else {
                ReplyBar(draft: $draft, typing: m.typing, keysFirst: m.kind != "chat", focused: $replying, send: {
                    let text = draft
                    replying = false  // sent: the keyboard goes, to watch what it does
                    Task { if await m.send(text) { draft = "" } }
                }, key: { k in Task { await m.key(k) } })
            }
        }
        .animation(.easeInOut(duration: 0.2), value: m.prompt)
        .background(Color(white: 0.02).ignoresSafeArea())
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .principal) {
                PaneHeader(title: heading, status: m.status, activity: m.activity, turnStart: m.turnStart, now: m.now,
                           note: m.kind == "chat" ? m.note : "", subagent: m.isSubagent)
            }
        }
        .task { await m.run() }
        .onAppear { m.childOpen = false }
        .onDisappear { if !m.childOpen { Task { await m.stop() } } }
    }
}

/// The title, and what the agent is doing: working (for how long, and on what), waiting for you, finished.
struct PaneHeader: View {
    var title: String
    var status: String
    var activity: String
    var turnStart: UInt64
    var now: UInt64
    var note: String
    var subagent: Bool

    var body: some View {
        VStack(spacing: 2) {
            HStack(spacing: 4) {
                if subagent { Image(systemName: "person.2.fill").font(.caption2).foregroundStyle(.secondary) }
                Text(title).font(.subheadline.weight(.semibold)).lineLimit(1)
            }
            HStack(spacing: 6) {
                StatusChip(status: status, since: status == "working" ? turnStart : 0, now: now)
                let detail = activity.isEmpty ? note : activity
                if !detail.isEmpty {
                    Text(detail).font(.caption2).foregroundStyle(.secondary).lineLimit(1)
                        .accessibilityIdentifier("activity")
                }
            }
        }
        .frame(maxWidth: 300)
    }
}

/// A pane of a Warp older than the phone's (it lists its terminals but can't show one): what to do.
struct UpdateWarpView: View {
    var title: String

    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "arrow.down.circle").font(.system(size: 44)).foregroundStyle(accent)
            Text("Update Warp on your computer").font(.headline)
            Text("This Warp lists its terminals but can't show them on the phone. It updates itself; to update now, right-click its title bar and choose **Check for updates**.")
                .font(.subheadline).foregroundStyle(.secondary).multilineTextAlignment(.center)
        }
        .padding(32)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color(white: 0.02).ignoresSafeArea())
        .navigationTitle(title)
        .navigationBarTitleDisplayMode(.inline)
        .accessibilityIdentifier("update-warp")
    }
}

/// An agent's pane whose conversation isn't there yet: where it is being looked for, and its terminal on request.
struct AgentWaitingView: View {
    var agent: String
    var note: String
    @Binding var showTerminal: Bool

    var body: some View {
        VStack(spacing: 14) {
            ProgressView()
            Text(agent == "codex" ? "Codex" : "Claude Code").font(.headline)
            Text(note.isEmpty ? "Opening the conversation…" : note)
                .font(.subheadline).foregroundStyle(.secondary).multilineTextAlignment(.center)
            Button("Show the terminal") { showTerminal = true }
                .buttonStyle(.bordered)
                .accessibilityIdentifier("show-terminal")
        }
        .frame(maxWidth: .infinity)
        .padding(.top, 60)
        .accessibilityIdentifier("agent-waiting")
    }
}

struct StatusChip: View {
    var status: String
    /// When the turn started (Unix ms), to show for how long it has been working (at `now`).
    var since: UInt64 = 0
    var now: UInt64 = 0

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
                if since > 0 && now > since {
                    Text(shortDuration(now - since))
                        .font(.caption2.monospacedDigit()).foregroundStyle(color.opacity(0.8))
                }
            }
            .padding(.horizontal, 8).padding(.vertical, 2)
            .background(Capsule().fill(color.opacity(0.15)))
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(label)
            .accessibilityIdentifier("status")
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
            ScrollView {
                VStack(spacing: 8) {
                    ForEach(Array(prompt.options.enumerated()), id: \.offset) { i, o in
                        Button { choose(i) } label: {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(o.label).font(.body.weight(.semibold)).multilineTextAlignment(.leading)
                                if let h = o.hint, !h.isEmpty {
                                    Text(h).font(.caption).opacity(0.75).lineLimit(3).multilineTextAlignment(.leading)
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
                }
            }
            .scrollBounceBehavior(.basedOnSize)
            .frame(maxHeight: 330)
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

/// Replies the agent's last message suggests (its numbered options, or yes and no to its question): a tap sends one.
struct QuickReplyBar: View {
    var replies: [QuickReply]
    var send: (QuickReply) -> Void

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
                ForEach(Array(replies.enumerated()), id: \.offset) { i, r in
                    Button { send(r) } label: {
                        Text(r.label).font(.subheadline.weight(.medium)).lineLimit(1)
                            .padding(.horizontal, 14).frame(minHeight: 36)
                            .background(Capsule().fill(i == 0 ? accent.opacity(0.3) : Color.white.opacity(0.09)))
                            .overlay(Capsule().stroke(accent.opacity(i == 0 ? 0.7 : 0.25), lineWidth: 1))
                    }
                    .buttonStyle(.plain)
                    .accessibilityIdentifier("quick-reply-\(i)")
                }
            }
            .padding(.horizontal, 12)
        }
        .padding(.vertical, 8)
        .background(Color(white: 0.05))
    }
}

/// Text box and, on request, the keys a terminal needs, at the bottom.
struct ReplyBar: View {
    @Binding var draft: String
    var typing: Bool
    /// A terminal shown as text: its keys show from the start.
    var keysFirst: Bool
    var focused: FocusState<Bool>.Binding
    var send: () -> Void
    var key: (String) -> Void
    @State private var keysShown: Bool?

    private let keys: [(String, String)] = [("Esc", "esc"), ("Enter", "enter"), ("↑", "up"), ("↓", "down"), ("Tab", "tab"), ("Ctrl+C", "ctrl_c")]

    var body: some View {
        let showKeys = keysShown ?? keysFirst
        VStack(spacing: 8) {
            if showKeys {
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
                    .padding(.horizontal, 12)
                }
                .transition(.move(edge: .bottom).combined(with: .opacity))
            }
            HStack(alignment: .bottom, spacing: 8) {
                Button {
                    withAnimation(.easeInOut(duration: 0.15)) { keysShown = !showKeys }
                } label: {
                    Image(systemName: showKeys ? "keyboard.chevron.compact.down" : "command")
                        .font(.system(size: 17, weight: .medium))
                        .frame(width: 38, height: 38)
                        .background(Circle().fill(Color.white.opacity(showKeys ? 0.15 : 0.08)))
                }
                .buttonStyle(.plain)
                .accessibilityLabel(showKeys ? "Hide keys" : "Show keys")
                .accessibilityIdentifier("keys-toggle")
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
            .padding(.horizontal, 12)
        }
        .disabled(!typing)
        .padding(.vertical, 8)
        .background(Color(white: 0.06).ignoresSafeArea(edges: .bottom))
    }
}

/// Now, in Unix ms.
func nowMs() -> UInt64 { UInt64(Date().timeIntervalSince1970 * 1000) }

/// «12s», «4m 10s», «3h 25m».
func shortDuration(_ ms: UInt64) -> String {
    let s = ms / 1000
    if s < 60 { return "\(s)s" }
    if s < 3600 { return "\(s / 60)m \(s % 60)s" }
    return "\(s / 3600)h \((s % 3600) / 60)m"
}

/// «812», «45.2k», «1.3M».
func shortCount(_ n: UInt64) -> String {
    if n >= 1_000_000 { return String(format: "%.1fM", Double(n) / 1_000_000) }
    if n >= 1000 { return String(format: "%.1fk", Double(n) / 1000) }
    return "\(n)"
}
