import Foundation
import UIKit

/// The Rust core (mobile/core): synchronous JSON calls, made here on one background queue (Forget on its own).
final class Core {
    private let handle: OpaquePointer?
    private let queue = DispatchQueue(label: "warp.core")

    init() {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let dir = base.appendingPathComponent("warp-remote", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        handle = wr_core_open(dir.path, UIDevice.current.name)
    }

    func call(_ request: [String: Any]) async -> Answer {
        await call(request, on: queue)
    }

    /// Forgets the paired computer at once: not behind the queue, where a call may be waiting up to 30 s for a computer
    /// that is gone (the core cancels it).
    func forget() async -> Answer {
        await call(["op": "forget"], on: .global(qos: .userInitiated))
    }

    private func call(_ request: [String: Any], on queue: DispatchQueue) async -> Answer {
        await withCheckedContinuation { cont in
            queue.async {
                guard let handle = self.handle,
                      let body = try? JSONSerialization.data(withJSONObject: request),
                      let text = String(data: body, encoding: .utf8),
                      let raw = wr_core_call(handle, text) else {
                    cont.resume(returning: Answer(ok: false, error: "The app's network core didn't start."))
                    return
                }
                let out = String(cString: raw)
                wr_string_free(raw)
                let answer = (try? JSONDecoder().decode(Answer.self, from: Data(out.utf8)))
                    ?? Answer(ok: false, error: "Unexpected answer.")
                cont.resume(returning: answer)
            }
        }
    }
}

/// Everything the core answers (each call fills in its part).
struct Answer: Decodable {
    var ok: Bool
    var code: String? = nil
    var error: String? = nil
    var paired: Bool? = nil
    var host: String? = nil
    var connected: Bool? = nil
    var path: NetPath? = nil
    var sessions: [Session]? = nil
    var rtt_ms: Double? = nil
    var pane: PaneState? = nil
    /// The protocol both sides speak (3: subagents can be opened).
    var version: Int? = nil
}

struct NetPath: Decodable, Equatable {
    var kind: String
    var addr: String
    var rtt_ms: Double
}

/// One terminal pane of a Warp window on the computer.
struct Session: Decodable, Identifiable, Hashable {
    var window: UInt64
    var tab: Int
    var pane: Int
    var tab_name: String
    var title: String
    var cwd: String
    var busy: Bool
    var running: String? = nil
    var agent: String? = nil
    /// The pane's id in its window (nil from an older Warp, which can't show a pane's content).
    var pane_id: UInt32? = nil
    /// Finishes and questions not seen yet on the computer (Warp's unread badge).
    var unread: Int? = nil
    /// An agent's pane: "working", "waiting" (a question waits for an answer) or "finished".
    var status: String? = nil
    var id: String { "\(window)-\(tab)-\(pane)" }
}

/// A subagent of a pane's Claude Code, opened from its card or its chip (read-only).
struct SubagentRoute: Hashable {
    var session: Session
    var agent: String
    var title: String
}

/// What a pane shows (warp-remote's PaneView): a chat from Claude Code's or Codex's transcript, or the screen as text.
struct PaneState: Decodable {
    var rev: UInt64
    var kind: String
    var agent: String? = nil
    var status: String? = nil
    var title: String? = nil
    var items: [ChatItem]? = nil
    var reset: Bool? = nil
    var more: Bool? = nil
    var screen: [String]? = nil
    var prompt: Prompt? = nil
    var typing: Bool? = nil
    var note: String? = nil
    /// The subagents (running ones first), when they changed.
    var agents: [AgentInfo]? = nil
    /// When the turn going on (or the last one) started (Unix ms).
    var turn_start: UInt64? = nil
    /// What the agent says it is doing («Baking… (3m 12s · ↓ 4.2k tokens)»), or the tool call running.
    var activity: String? = nil
}

struct ChatItem: Decodable, Identifiable, Equatable {
    var id: UInt64
    /// "user", "assistant" (markdown), "tool" (one line, details on tap) or "note".
    var role: String
    var text: String
    var detail: String? = nil
    var failed: Bool? = nil
    var pending: Bool? = nil
    /// Unix ms.
    var ts: UInt64? = nil
    /// The tool of a tool call ("Bash", "Edit", "Agent", "AskUserQuestion", "ExitPlanMode"…).
    var tool: String? = nil
    /// What it came to, short («+12 −3», «exit 1»).
    var badge: String? = nil
    /// The subagent it started (or a report of).
    var agent: AgentInfo? = nil
    /// An AskUserQuestion's options.
    var options: [PromptOption]? = nil
}

/// A subagent: Claude Code's Agent tool, in the foreground or the background, with its own conversation.
struct AgentInfo: Decodable, Equatable {
    /// Empty until the computer knows it.
    var id: String
    var name: String
    var description: String? = nil
    /// "running", "done" or "failed".
    var status: String
    var started: UInt64? = nil
    var ended: UInt64? = nil
    var duration_ms: UInt64? = nil
    var tokens: UInt64? = nil
    var tool_uses: UInt64? = nil
    var background: Bool? = nil

    var running: Bool { status == "running" }
    /// Stable within a list even before the id is known.
    var key: String { id.isEmpty ? "\(name)-\(started ?? 0)-\(description ?? "")" : id }
}

/// A question on the pane's screen with its options (Claude Code's permission prompts and menus, Codex's approvals).
struct Prompt: Decodable, Equatable {
    var id: String
    var question: String
    var context: [String]? = nil
    var options: [PromptOption]
    var selected: Int? = nil
}

struct PromptOption: Decodable, Equatable {
    var label: String
    var hint: String? = nil
}
