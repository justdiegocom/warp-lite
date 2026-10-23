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
}

struct NetPath: Decodable, Equatable {
    var kind: String
    var addr: String
    var rtt_ms: Double
}

/// One terminal pane of a Warp window on the computer.
struct Session: Decodable, Identifiable, Equatable {
    var window: UInt64
    var tab: Int
    var pane: Int
    var tab_name: String
    var title: String
    var cwd: String
    var busy: Bool
    var running: String? = nil
    var agent: String? = nil
    var id: String { "\(window)-\(tab)-\(pane)" }
}
