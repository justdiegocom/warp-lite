import Foundation
import UIKit

/// The Rust core (mobile/core): synchronous JSON calls, made here on one background queue.
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
    var id: String { "\(window)-\(tab)-\(pane)" }
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
}

struct ChatItem: Decodable, Identifiable, Equatable {
    var id: UInt64
    /// "user", "assistant" (markdown), "tool" (one line, details on tap) or "note".
    var role: String
    var text: String
    var detail: String? = nil
    var failed: Bool? = nil
    var pending: Bool? = nil
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
