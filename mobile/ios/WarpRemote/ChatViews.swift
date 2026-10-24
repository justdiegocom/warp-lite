import SwiftUI

/// The conversation as a chat: your messages on the right, the agent's answers in bubbles (markdown), its tool calls as
/// compact cards (consecutive ones in one group), subagents as cards that open their own chat, questions and plans as
/// cards of their own.
struct ChatRows: View {
    var items: [ChatItem]
    var now: UInt64
    var canOpen: Bool
    var open: (AgentInfo) -> Void

    private enum Row: Identifiable {
        case item(ChatItem)
        case tools([ChatItem])

        var id: UInt64 {
            switch self {
            case .item(let i): return i.id
            case .tools(let t): return t[0].id
            }
        }
    }

    /// Tool calls of their own kind (a subagent, a question, a plan) stand alone; the others group while consecutive.
    private static func standalone(_ i: ChatItem) -> Bool {
        i.agent != nil || ["Agent", "Task", "AskUserQuestion", "ExitPlanMode"].contains(i.tool ?? "")
    }

    private var rows: [Row] {
        var out: [Row] = []
        var group: [ChatItem] = []
        func flush() {
            if !group.isEmpty { out.append(.tools(group)) }
            group = []
        }
        for i in items {
            if i.role == "tool" && !Self.standalone(i) {
                group.append(i)
            } else {
                flush()
                out.append(.item(i))
            }
        }
        flush()
        return out
    }

    var body: some View {
        ForEach(rows) { row in
            switch row {
            case .tools(let t):
                if t.count == 1 { ToolRow(item: t[0]).padding(.horizontal, 4) } else { ToolGroupCard(items: t) }
            case .item(let i):
                // Redrawn only when the item itself changed.
                ItemRow(item: i, now: now, canOpen: canOpen, open: open).equatable()
            }
        }
    }
}

struct ItemRow: View, Equatable {
    var item: ChatItem
    var now: UInt64
    var canOpen: Bool
    var open: (AgentInfo) -> Void

    /// The clock only matters to a subagent that is running.
    static func == (a: ItemRow, b: ItemRow) -> Bool {
        a.item == b.item && a.canOpen == b.canOpen && (a.item.agent?.running != true || a.now == b.now)
    }

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
            MarkdownView(text: item.text)
                .padding(.horizontal, 14).padding(.vertical, 11)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(RoundedRectangle(cornerRadius: 18).fill(Color(white: 0.105)))
                .accessibilityIdentifier("item-assistant")
        case "tool":
            if let a = item.agent {
                AgentCard(item: item, agent: a, now: now, canOpen: canOpen && !a.id.isEmpty) { open(a) }
            } else if item.tool == "AskUserQuestion" {
                QuestionCard(item: item)
            } else if item.tool == "ExitPlanMode" {
                PlanCard(item: item)
            } else {
                ToolRow(item: item).padding(.horizontal, 4)
            }
        default:
            Text(item.text).font(.caption).foregroundStyle(.secondary).multilineTextAlignment(.center)
                .frame(maxWidth: .infinity)
        }
    }
}

/// What a tool call is: its icon, and how a group of them counts it («Ran 3 commands»).
private struct ToolKind {
    var icon: String
    var verb: String
    var one: String
    var many: String

    init(_ item: ChatItem) {
        let t = item.tool ?? ""
        let text = item.text
        switch t {
        case "Bash", "shell", "container.exec", "local_shell", "exec_command", "shell_command", "exec":
            self.init("terminal", "Ran", "command", "commands")
        case "Edit", "MultiEdit", "Write", "NotebookEdit", "apply_patch":
            self.init("pencil", "Edited", "file", "files")
        case "Read":
            self.init("doc.text", "Read", "file", "files")
        case "Grep", "Glob", "WebSearch":
            self.init("magnifyingglass", "Searched", "time", "times")
        case "WebFetch":
            self.init("globe", "Fetched", "page", "pages")
        case "SendMessage":
            self.init("paperplane", "Sent", "message", "messages")
        case "TodoWrite":
            self.init("checklist", "Updated", "to-do list", "to-do lists")
        case "":
            // An older computer doesn't say which tool: from the line.
            if text.hasPrefix("Ran") { self.init("terminal", "Ran", "command", "commands") }
            else if text.hasPrefix("Edited") || text.hasPrefix("Wrote") { self.init("pencil", "Edited", "file", "files") }
            else if text.hasPrefix("Read") { self.init("doc.text", "Read", "file", "files") }
            else if text.hasPrefix("Searched") || text.hasPrefix("Listed") { self.init("magnifyingglass", "Searched", "time", "times") }
            else if text.hasPrefix("Fetched") { self.init("globe", "Fetched", "page", "pages") }
            else { self.init("wrench.and.screwdriver", "Used", "tool", "tools") }
        default:
            self.init("wrench.and.screwdriver", "Used", "tool", "tools")
        }
    }

    private init(_ icon: String, _ verb: String, _ one: String, _ many: String) {
        self.icon = icon
        self.verb = verb
        self.one = one
        self.many = many
    }
}

/// A short result next to a tool call: «+12 −3» in green and red, «exit 1» in red, the rest grey.
struct BadgeText: View {
    var badge: String
    var failed: Bool

    var body: some View {
        let parts = badge.split(separator: " ")
        if parts.count == 2, parts[0].hasPrefix("+"), parts[1].hasPrefix("−") {
            HStack(spacing: 3) {
                Text(parts[0]).foregroundStyle(.green)
                Text(parts[1]).foregroundStyle(.red)
            }
            .font(.caption.monospacedDigit().weight(.medium))
        } else {
            Text(badge).font(.caption.weight(.medium)).foregroundStyle(failed ? .red : .secondary).lineLimit(1)
        }
    }
}

/// One tool call: its line and short result; a tap shows what it ran and what came back.
struct ToolRow: View {
    var item: ChatItem
    @State private var open = false

    var body: some View {
        let kind = ToolKind(item)
        VStack(alignment: .leading, spacing: 6) {
            Button {
                withAnimation(.easeInOut(duration: 0.15)) { open.toggle() }
            } label: {
                HStack(spacing: 8) {
                    if item.pending == true {
                        ProgressView().controlSize(.mini).frame(width: 18)
                    } else {
                        Image(systemName: item.failed == true ? "exclamationmark.triangle.fill" : kind.icon).font(.footnote).frame(width: 18)
                            .foregroundStyle(item.failed == true ? .red : .secondary)
                    }
                    Text(item.text).font(.subheadline).foregroundStyle(.secondary).lineLimit(1)
                    Spacer(minLength: 4)
                    if let b = item.badge, !b.isEmpty { BadgeText(badge: b, failed: item.failed == true) }
                    if !(item.detail ?? "").isEmpty {
                        Image(systemName: open ? "chevron.up" : "chevron.down").font(.caption2).foregroundStyle(.tertiary)
                    }
                }
                .frame(minHeight: 28)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            if open, let d = item.detail, !d.isEmpty {
                DetailText(text: d)
            }
        }
        .accessibilityIdentifier("item-tool")
    }
}

struct DetailText: View {
    var text: String

    var body: some View {
        Text(text)
            .font(.system(.caption, design: .monospaced))
            .foregroundStyle(.secondary)
            .textSelection(.enabled)
            .padding(10)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 8).fill(Color.white.opacity(0.05)))
    }
}

/// Consecutive tool calls in one card: «Ran 3 commands · Edited 2 files», the one running now, and each of them on tap.
struct ToolGroupCard: View {
    var items: [ChatItem]
    @State private var open = false

    private var summary: String {
        var order: [String] = []
        var counts: [String: (ToolKind, Int)] = [:]
        for i in items {
            let k = ToolKind(i)
            let key = k.verb + k.one
            if counts[key] == nil { order.append(key) }
            counts[key] = (k, (counts[key]?.1 ?? 0) + 1)
        }
        return order.compactMap { key in
            guard let (k, n) = counts[key] else { return nil }
            return "\(k.verb) \(n) \(n == 1 ? k.one : k.many)"
        }.joined(separator: " · ")
    }

    var body: some View {
        let running = items.last(where: { $0.pending == true })
        let failed = items.filter { $0.failed == true }.count
        VStack(alignment: .leading, spacing: 4) {
            Button {
                withAnimation(.easeInOut(duration: 0.15)) { open.toggle() }
            } label: {
                HStack(spacing: 8) {
                    if running != nil {
                        ProgressView().controlSize(.mini).frame(width: 18)
                    } else {
                        Image(systemName: "square.stack.3d.up").font(.footnote).foregroundStyle(.secondary).frame(width: 18)
                    }
                    VStack(alignment: .leading, spacing: 2) {
                        Text(summary).font(.subheadline).foregroundStyle(.secondary).lineLimit(2)
                        if let r = running, !open {
                            Text(r.text).font(.caption).foregroundStyle(.orange).lineLimit(1)
                        }
                    }
                    Spacer(minLength: 4)
                    if failed > 0 {
                        Text("\(failed) failed").font(.caption.weight(.medium)).foregroundStyle(.red)
                    }
                    Image(systemName: open ? "chevron.up" : "chevron.down").font(.caption2).foregroundStyle(.tertiary)
                }
                .frame(minHeight: 30)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            if open {
                VStack(alignment: .leading, spacing: 2) {
                    ForEach(items) { ToolRow(item: $0) }
                }
                .padding(.leading, 12)
            }
        }
        .padding(.horizontal, 10).padding(.vertical, 6)
        .background(RoundedRectangle(cornerRadius: 12).fill(Color.white.opacity(0.035)))
        .accessibilityIdentifier("tool-group")
    }
}

/// The first `n` lines of a text (a long one cut on its own instead of a clipped frame, which a lazy list can't measure).
func firstLines(_ text: String, _ n: Int) -> String {
    let lines = text.split(separator: "\n", omittingEmptySubsequences: false)
    if lines.count <= n && text.count <= 700 { return text }
    let head = lines.prefix(n).joined(separator: "\n")
    return head.count > 700 ? String(head.prefix(700)) + "…" : head + "\n…"
}

/// How long a subagent has been working (running) or took (stopped), its tokens and tool calls.
func agentStats(_ a: AgentInfo, now: UInt64) -> String {
    var parts: [String] = []
    if a.running, let s = a.started, s > 0, now > s {
        parts.append(shortDuration(now - s))
    } else if let d = a.duration_ms, d > 0 {
        parts.append(shortDuration(d))
    } else if let s = a.started, let e = a.ended, s > 0, e > s {
        parts.append(shortDuration(e - s))
    }
    if let t = a.tokens, t > 0 { parts.append("\(shortCount(t)) tokens") }
    if let n = a.tool_uses, n > 0 { parts.append("\(n) tool \(n == 1 ? "use" : "uses")") }
    return parts.joined(separator: " · ")
}

struct AgentStatusPill: View {
    var status: String

    var body: some View {
        let (label, color): (String, Color) = status == "running" ? ("Running", .orange) : status == "failed" ? ("Stopped", .red) : ("Done", .green)
        HStack(spacing: 4) {
            if status == "running" { ProgressView().controlSize(.mini).tint(color) } else { Circle().fill(color).frame(width: 6, height: 6) }
            Text(label).font(.caption2.weight(.semibold)).foregroundStyle(color)
        }
        .padding(.horizontal, 7).padding(.vertical, 2)
        .background(Capsule().fill(color.opacity(0.15)))
    }
}

/// A subagent: what it was asked, its type, how it goes; a tap opens its own conversation. A report it sent when it
/// stopped shows under it.
struct AgentCard: View {
    var item: ChatItem
    var agent: AgentInfo
    var now: UInt64
    var canOpen: Bool
    var open: () -> Void
    @State private var expanded = false

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Button(action: open) {
                HStack(alignment: .top, spacing: 10) {
                    Image(systemName: "person.2.fill").font(.subheadline).foregroundStyle(accent)
                        .frame(width: 30, height: 30).background(Circle().fill(accent.opacity(0.15)))
                    VStack(alignment: .leading, spacing: 3) {
                        Text(item.text.isEmpty ? (agent.description ?? agent.name) : item.text)
                            .font(.subheadline.weight(.semibold)).multilineTextAlignment(.leading).lineLimit(3)
                        HStack(spacing: 6) {
                            Text(agent.name).font(.caption.weight(.medium)).foregroundStyle(.secondary)
                            if agent.background == true { Text("background").font(.caption2).foregroundStyle(.tertiary) }
                        }
                        let stats = agentStats(agent, now: now)
                        if !stats.isEmpty { Text(stats).font(.caption.monospacedDigit()).foregroundStyle(.secondary) }
                    }
                    Spacer(minLength: 4)
                    VStack(alignment: .trailing, spacing: 6) {
                        AgentStatusPill(status: agent.status)
                        if canOpen { Image(systemName: "chevron.right").font(.caption).foregroundStyle(.tertiary) }
                    }
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .disabled(!canOpen)
            .accessibilityIdentifier("agent-card-\(agent.id)")
            if let report = item.detail, !report.isEmpty, item.ts != nil || agent.status != "running" {
                let short = firstLines(report, 6)
                VStack(alignment: .leading, spacing: 4) {
                    MarkdownView(text: expanded ? report : short, compact: true)
                    if short != report {
                        Button(expanded ? "Show less" : "Show more") {
                            withAnimation(.easeInOut(duration: 0.15)) { expanded.toggle() }
                        }
                        .font(.caption.weight(.medium))
                    }
                }
                .padding(10)
                .background(RoundedRectangle(cornerRadius: 10).fill(Color.white.opacity(0.04)))
            }
        }
        .padding(12)
        .background(RoundedRectangle(cornerRadius: 14).fill(Color(white: 0.085)))
        .overlay(RoundedRectangle(cornerRadius: 14).stroke(agent.running ? Color.orange.opacity(0.35) : Color.white.opacity(0.06), lineWidth: 1))
    }
}

/// The subagents at the top: the ones working now, and the ones that stopped in the last half hour; a tap opens one.
struct AgentChips: View {
    var agents: [AgentInfo]
    var now: UInt64
    var canOpen: Bool
    var open: (AgentInfo) -> Void

    private var shown: [AgentInfo] {
        agents.filter { a in
            a.running || (a.ended ?? 0) > now - min(now, 30 * 60 * 1000)
        }.prefix(12).map { $0 }
    }

    var body: some View {
        if !shown.isEmpty {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 8) {
                    ForEach(shown, id: \.key) { a in
                        Button { open(a) } label: {
                            VStack(alignment: .leading, spacing: 2) {
                                HStack(spacing: 5) {
                                    if a.running {
                                        ProgressView().controlSize(.mini).tint(.orange)
                                    } else {
                                        Image(systemName: a.status == "failed" ? "xmark.circle.fill" : "checkmark.circle.fill")
                                            .font(.caption2).foregroundStyle(a.status == "failed" ? .red : .green)
                                    }
                                    Text(a.name).font(.caption.weight(.semibold)).lineLimit(1)
                                    Text(agentStats(a, now: now).components(separatedBy: " · ").first ?? "")
                                        .font(.caption2.monospacedDigit()).foregroundStyle(.secondary)
                                }
                                if let d = a.description, !d.isEmpty {
                                    Text(d).font(.caption2).foregroundStyle(.secondary).lineLimit(1)
                                }
                            }
                            .frame(maxWidth: 190, alignment: .leading)
                            .padding(.horizontal, 10).padding(.vertical, 6)
                            .background(RoundedRectangle(cornerRadius: 12).fill(Color.white.opacity(a.running ? 0.09 : 0.05)))
                            .opacity(a.running ? 1 : 0.75)
                        }
                        .buttonStyle(.plain)
                        .disabled(!canOpen || a.id.isEmpty)
                        .accessibilityIdentifier("agent-chip-\(a.id)")
                    }
                }
                .padding(.horizontal, 12)
            }
            .padding(.vertical, 8)
            .background(Color(white: 0.045))
            .accessibilityIdentifier("agent-chips")
        }
    }
}

/// A question the agent asked with AskUserQuestion: its options with their descriptions, and the answer once given
/// (the buttons to answer come with the question on the screen, at the bottom).
struct QuestionCard: View {
    var item: ChatItem

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Label(item.pending == true ? "Question for you" : "Question", systemImage: "questionmark.bubble.fill")
                .font(.caption.weight(.semibold)).foregroundStyle(.yellow)
            Text(item.text).font(.subheadline.weight(.semibold))
            ForEach(Array((item.options ?? []).enumerated()), id: \.offset) { _, o in
                let chosen = !(item.badge ?? "").isEmpty && (item.badge ?? "").contains(o.label)
                HStack(alignment: .top, spacing: 8) {
                    Image(systemName: chosen ? "checkmark.circle.fill" : "circle").font(.caption)
                        .foregroundStyle(chosen ? .green : .secondary).padding(.top, 2)
                    VStack(alignment: .leading, spacing: 1) {
                        Text(o.label).font(.subheadline)
                        if let h = o.hint, !h.isEmpty { Text(h).font(.caption).foregroundStyle(.secondary) }
                    }
                }
            }
            if let d = item.detail, !d.isEmpty { Text(d).font(.caption).foregroundStyle(.secondary) }
            if let b = item.badge, !b.isEmpty {
                Text("Answered: \(b)").font(.caption.weight(.medium)).foregroundStyle(.green)
            } else if item.pending == true {
                Text("Answer with the buttons below.").font(.caption).foregroundStyle(.secondary)
            }
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 14).fill(Color.yellow.opacity(0.07)))
        .overlay(RoundedRectangle(cornerRadius: 14).stroke(Color.yellow.opacity(0.25), lineWidth: 1))
        .accessibilityIdentifier("item-question")
    }
}

/// A plan the agent proposes (ExitPlanMode), as markdown; approving it is a question on the screen, at the bottom.
struct PlanCard: View {
    var item: ChatItem
    @State private var expanded = false

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Label("Plan", systemImage: "list.bullet.clipboard.fill").font(.caption.weight(.semibold)).foregroundStyle(accent)
                Spacer()
                if let b = item.badge, !b.isEmpty {
                    Text(b.capitalized).font(.caption2.weight(.semibold)).foregroundStyle(b == "approved" ? .green : .red)
                } else if item.pending == true {
                    Text("Waiting for you").font(.caption2.weight(.semibold)).foregroundStyle(.yellow)
                }
            }
            let plan = item.detail ?? ""
            let short = firstLines(plan, 12)
            MarkdownView(text: expanded || item.pending == true ? plan : short, compact: true)
            if item.pending != true && short != plan {
                Button(expanded ? "Show less" : "Show the whole plan") {
                    withAnimation(.easeInOut(duration: 0.15)) { expanded.toggle() }
                }
                .font(.caption.weight(.medium))
            }
        }
        .padding(12)
        .background(RoundedRectangle(cornerRadius: 14).fill(Color(white: 0.085)))
        .overlay(RoundedRectangle(cornerRadius: 14).stroke(accent.opacity(0.3), lineWidth: 1))
        .accessibilityIdentifier("item-plan")
    }
}

/// Markdown as the agents write it: headings, lists (nested, numbered), tables, quotes, code blocks, rules, and inline
/// styles (bold, italics, code, links).
struct MarkdownView: View {
    var text: String
    var compact = false

    enum Block {
        case paragraph(AttributedString)
        case heading(Int, AttributedString)
        case code(String, String)
        case list([(indent: Int, marker: String, text: AttributedString)])
        case quote(AttributedString)
        case table([[AttributedString]])
        case rule
    }

    /// Parsed once per text: a chat is drawn again and again, and parsing markdown is slow.
    private final class Parsed {
        let blocks: [Block]
        init(_ b: [Block]) { blocks = b }
    }

    private static let cache: NSCache<NSString, Parsed> = {
        let c = NSCache<NSString, Parsed>()
        c.countLimit = 400
        return c
    }()

    static func parsed(_ text: String) -> [Block] {
        if let p = cache.object(forKey: text as NSString) { return p.blocks }
        let b = blocks(text)
        cache.setObject(Parsed(b), forKey: text as NSString)
        return b
    }

    static func listItem(_ line: Substring) -> (indent: Int, marker: String, text: String)? {
        let lead = line.prefix(while: { $0 == " " }).count
        let t = line.dropFirst(lead)
        for m in ["- ", "* ", "+ "] where t.hasPrefix(m) {
            return (lead / 2, "•", String(t.dropFirst(2)))
        }
        let digits = t.prefix(while: { $0.isNumber })
        if !digits.isEmpty, digits.count <= 3 {
            let rest = t.dropFirst(digits.count)
            if rest.hasPrefix(". ") || rest.hasPrefix(") ") { return (lead / 2, "\(digits).", String(rest.dropFirst(2))) }
        }
        return nil
    }

    static func tableCells(_ line: String) -> [String] {
        var t = line.trimmingCharacters(in: .whitespaces)
        if t.hasPrefix("|") { t.removeFirst() }
        if t.hasSuffix("|") { t.removeLast() }
        return t.components(separatedBy: "|").map { $0.trimmingCharacters(in: .whitespaces) }
    }

    static func isTableRule(_ line: String) -> Bool {
        let cells = tableCells(line)
        return !cells.isEmpty && cells.allSatisfy { c in !c.isEmpty && c.allSatisfy { "-:".contains($0) } }
    }

    static func blocks(_ text: String) -> [Block] {
        var out: [Block] = []
        let lines = text.split(separator: "\n", omittingEmptySubsequences: false)
        var i = 0
        var para: [Substring] = []
        func flushPara() {
            let p = para.joined(separator: "\n").trimmingCharacters(in: .whitespacesAndNewlines)
            if !p.isEmpty { out.append(.paragraph(inline(p))) }
            para = []
        }
        while i < lines.count {
            let line = lines[i]
            let t = line.trimmingCharacters(in: .whitespaces)
            if t.hasPrefix("```") {
                flushPara()
                let lang = String(t.dropFirst(3)).trimmingCharacters(in: .whitespaces)
                var code: [Substring] = []
                i += 1
                while i < lines.count, !lines[i].trimmingCharacters(in: .whitespaces).hasPrefix("```") {
                    code.append(lines[i])
                    i += 1
                }
                out.append(.code(lang, code.joined(separator: "\n")))
                i += 1
                continue
            }
            if t.isEmpty {
                flushPara()
                i += 1
                continue
            }
            if t.hasPrefix("#") {
                let level = t.prefix(while: { $0 == "#" }).count
                let rest = t.dropFirst(level)
                if level <= 6, rest.hasPrefix(" ") {
                    flushPara()
                    out.append(.heading(level, inline(rest.trimmingCharacters(in: .whitespaces))))
                    i += 1
                    continue
                }
            }
            if t.count >= 3, t.allSatisfy({ $0 == "-" || $0 == "*" || $0 == "_" }), Set(t).count == 1 {
                flushPara()
                out.append(.rule)
                i += 1
                continue
            }
            if t.hasPrefix("|"), i + 1 < lines.count, isTableRule(String(lines[i + 1])) {
                flushPara()
                var rows = [tableCells(t)]
                i += 2
                while i < lines.count, lines[i].trimmingCharacters(in: .whitespaces).hasPrefix("|") {
                    rows.append(tableCells(String(lines[i])))
                    i += 1
                }
                out.append(.table(rows.map { $0.map(inline) }))
                continue
            }
            if t.hasPrefix(">") {
                flushPara()
                var q: [String] = []
                while i < lines.count, lines[i].trimmingCharacters(in: .whitespaces).hasPrefix(">") {
                    q.append(String(lines[i].trimmingCharacters(in: .whitespaces).dropFirst()).trimmingCharacters(in: .whitespaces))
                    i += 1
                }
                out.append(.quote(inline(q.joined(separator: "\n"))))
                continue
            }
            if listItem(line) != nil {
                flushPara()
                var items: [(indent: Int, marker: String, text: String)] = []
                while i < lines.count {
                    if let it = listItem(lines[i]) {
                        items.append(it)
                    } else if !lines[i].trimmingCharacters(in: .whitespaces).isEmpty, lines[i].hasPrefix("  "), !items.isEmpty {
                        items[items.count - 1].text += "\n" + lines[i].trimmingCharacters(in: .whitespaces)  // a wrapped item
                    } else {
                        break
                    }
                    i += 1
                }
                out.append(.list(items.map { (indent: $0.indent, marker: $0.marker, text: inline($0.text)) }))
                continue
            }
            para.append(line)
            i += 1
        }
        flushPara()
        return out
    }

    static func inline(_ s: String) -> AttributedString {
        let opts = AttributedString.MarkdownParsingOptions(interpretedSyntax: .inlineOnlyPreservingWhitespace)
        return (try? AttributedString(markdown: s, options: opts)) ?? AttributedString(s)
    }

    private func headingFont(_ level: Int) -> Font {
        switch level {
        case 1: return compact ? .headline : .title3.weight(.bold)
        case 2: return .headline
        default: return .subheadline.weight(.bold)
        }
    }

    var body: some View {
        let font: Font = compact ? .subheadline : .body
        VStack(alignment: .leading, spacing: 8) {
            ForEach(Array(Self.parsed(text).enumerated()), id: \.offset) { _, b in
                switch b {
                case .paragraph(let p):
                    Text(p).font(font).textSelection(.enabled)
                case .heading(let level, let h):
                    Text(h).font(headingFont(level)).padding(.top, 2)
                case .code(let lang, let code):
                    VStack(alignment: .leading, spacing: 4) {
                        if !lang.isEmpty { Text(lang).font(.caption2).foregroundStyle(.tertiary) }
                        ScrollView(.horizontal, showsIndicators: false) {
                            Text(code).font(.system(.footnote, design: .monospaced)).textSelection(.enabled)
                        }
                    }
                    .padding(10)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(RoundedRectangle(cornerRadius: 8).fill(Color.black.opacity(0.35)))
                case .list(let items):
                    VStack(alignment: .leading, spacing: 4) {
                        ForEach(Array(items.enumerated()), id: \.offset) { _, it in
                            HStack(alignment: .firstTextBaseline, spacing: 6) {
                                Text(it.marker).font(font).foregroundStyle(.secondary).frame(minWidth: 14, alignment: .trailing)
                                Text(it.text).font(font).textSelection(.enabled)
                            }
                            .padding(.leading, CGFloat(min(it.indent, 4)) * 14)
                        }
                    }
                case .quote(let q):
                    HStack(spacing: 8) {
                        Rectangle().fill(Color.white.opacity(0.25)).frame(width: 3)
                        Text(q).font(font).foregroundStyle(.secondary)
                    }
                case .table(let rows):
                    ScrollView(.horizontal, showsIndicators: false) {
                        Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 6) {
                            ForEach(Array(rows.enumerated()), id: \.offset) { r, cells in
                                GridRow {
                                    ForEach(Array(cells.enumerated()), id: \.offset) { _, c in
                                        Text(c).font(r == 0 ? .footnote.weight(.semibold) : .footnote)
                                            .frame(width: 180, alignment: .leading)
                                    }
                                }
                            }
                        }
                        .padding(10)
                    }
                    .background(RoundedRectangle(cornerRadius: 8).fill(Color.white.opacity(0.04)))
                case .rule:
                    Divider()
                }
            }
        }
    }
}

/// A reply the agent's last message suggests: its numbered options, or yes and no to the question it ends with. Only
/// offered (never sent on its own); a tap sends its text.
struct QuickReply: Hashable {
    var label: String
    var text: String

    /// Spanish or English, from the message.
    static func spanish(_ t: String) -> Bool {
        if t.contains("¿") || t.contains("¡") { return true }
        let l = " " + t.lowercased() + " "
        let es = [" que ", " de ", " el ", " la ", " lo ", " los ", " en ", " es ", " para ", " con ", " por ", " una "].reduce(0) { $0 + l.components(separatedBy: $1).count - 1 }
        let en = [" the ", " and ", " to ", " of ", " is ", " you ", " it ", " for ", " with ", " this "].reduce(0) { $0 + l.components(separatedBy: $1).count - 1 }
        return es > en
    }

    /// A numbered line: its number and text (without markdown emphasis).
    static func numbered(_ line: String) -> (Int, String)? {
        var t = line.trimmingCharacters(in: .whitespaces)
        if t.hasPrefix("**") { t = String(t.dropFirst(2)) }
        let digits = t.prefix(while: { $0.isNumber })
        guard !digits.isEmpty, digits.count <= 2, let n = Int(digits) else { return nil }
        var rest = t.dropFirst(digits.count)
        guard rest.hasPrefix(".") || rest.hasPrefix(")") else { return nil }
        rest = rest.dropFirst()
        if rest.hasPrefix("**") { rest = rest.dropFirst(2) }
        let label = rest.replacingOccurrences(of: "**", with: "").replacingOccurrences(of: "`", with: "").trimmingCharacters(in: .whitespaces)
        return label.isEmpty ? nil : (n, label)
    }

    private static let openWords = ["qué", "que ", "cuál", "cual ", "cuáles", "cómo", "como ", "cuándo", "cuando ", "dónde", "donde ", "quién", "cuánto", "cuántos", "por qué",
                                    "what", "which", "how", "when", "where", "who", "why"]

    static func suggestions(for text: String) -> [QuickReply] {
        let lines = text.split(separator: "\n", omittingEmptySubsequences: false).map { $0.trimmingCharacters(in: .whitespaces) }
        var end = lines.count
        while end > 0 && lines[end - 1].isEmpty { end -= 1 }
        guard end > 0 else { return [] }
        let es = spanish(text)
        // Numbered options at the end (a closing line or two after them is fine), asked about.
        var i = end - 1
        var after: [String] = []
        while i >= 0, numbered(lines[i]) == nil, after.count < 2 {
            if !lines[i].isEmpty { after.append(lines[i]) }
            i -= 1
        }
        var opts: [(Int, String)] = []
        while i >= 0 {
            if let o = numbered(lines[i]) {
                opts.insert(o, at: 0)
            } else if opts.isEmpty || !(lines[i].isEmpty || lines[i].hasPrefix("-") || lines[i].hasPrefix("•") || lines[i].hasPrefix("* ")) {
                break  // the line that introduces them (blank lines and sub-bullets between options are skipped)
            }
            i -= 1
        }
        let intro = i >= 0 ? lines[i] : ""
        let asked = (intro + " " + after.joined(separator: " ")).contains("?") || intro.hasSuffix(":")
        if opts.count >= 2, opts.count <= 6, opts.map(\.0) == Array(1...opts.count), asked {
            return opts.map { n, label in
                let short = label.count > 34 ? String(label.prefix(32)) + "…" : label
                return QuickReply(label: "\(n). \(short)", text: "\(n). \(String(label.prefix(300)))")
            }
        }
        // A yes/no question at the very end.
        var last = lines[end - 1].replacingOccurrences(of: "**", with: "").replacingOccurrences(of: "*", with: "")
        guard last.hasSuffix("?") else { return [] }
        if let q = last.range(of: "¿", options: .backwards) {
            last = String(last[q.upperBound...])
        } else if let stop = last.dropLast().lastIndex(where: { ".!:;".contains($0) }) {
            last = String(last[last.index(after: stop)...])
        }
        let q = last.trimmingCharacters(in: .whitespaces).lowercased()
        if openWords.contains(where: { q.hasPrefix($0) }) || q.contains(" or ") || q.contains(" o ") { return [] }
        return es ? [QuickReply(label: "Sí, hazlo", text: "Sí, hazlo"), QuickReply(label: "No", text: "No")]
                  : [QuickReply(label: "Yes, do it", text: "Yes, do it"), QuickReply(label: "No", text: "No")]
    }
}
