import SwiftUI

@main
struct WarpRemoteApp: App {
    @StateObject private var model = Model()
    @Environment(\.scenePhase) private var phase

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(model)
                .preferredColorScheme(.dark)
                .tint(Color(red: 0.10, green: 0.67, blue: 0.85))
        }
        .onChange(of: phase) { _, now in
            Task { await model.phase(active: now == .active) }
        }
    }
}

/// The paired computer and its terminals, as the screens show them.
@MainActor
final class Model: ObservableObject {
    @Published var paired = false
    @Published var host = ""
    @Published var path: NetPath?
    @Published var rtt: Double?
    @Published var sessions: [Session] = []
    /// The protocol the computer speaks (3 and up: a subagent can be opened).
    @Published var version = 0
    /// What is open: a terminal, and maybe one of its subagents.
    @Published var nav = NavigationPath()
    /// The list of terminals is on screen: it follows them (badges, what each agent does).
    var listVisible = false
    @Published var error: String?
    /// The core's code for `error` ("unavailable", "not_paired", "revoked"…).
    @Published var errorCode: String?
    @Published var busy = false
    /// Refreshes in flight: with no connection, the list says Connecting….
    @Published private(set) var attempts = 0
    /// Tries in a row that didn't reach the computer; from the second, the list says it can't reach it.
    @Published private(set) var failures = 0
    let core = Core()
    private var pinger: Task<Void, Never>?
    /// Bumped by Forget: what a call made before answers later is dropped.
    private var epoch = 0

    var connecting: Bool { attempts > 0 }
    var unreachable: Bool { failures >= 2 }
    /// The computer doesn't know this phone any more (removed in Warp): only a new code helps.
    var needsPairing: Bool { errorCode == "not_paired" || errorCode == "revoked" }

    func start() async {
        // Tests: -rememberTicket <code> [-rememberName <name>] remembers a computer without reaching it (one that is gone).
        let d = UserDefaults.standard
        if let ticket = d.string(forKey: "rememberTicket"), !ticket.isEmpty {
            _ = await core.call(["op": "remember", "ticket": ticket, "name": d.string(forKey: "rememberName") ?? "Computer"])
        }
        let s = await core.call(["op": "status"])
        paired = s.paired ?? false
        host = s.host ?? ""
        // Tests and screenshots: -pairTicket <code> pairs at launch.
        if !paired, let ticket = d.string(forKey: "pairTicket"), !ticket.isEmpty {
            await pair(ticket)
        } else if paired {
            await refresh()
        }
        keepPinging()
    }

    func pair(_ ticket: String) async {
        busy = true
        defer { busy = false }
        let mine = epoch
        let a = await core.call(["op": "pair", "ticket": ticket.trimmingCharacters(in: .whitespacesAndNewlines)])
        guard mine == epoch else { return }
        guard a.ok else { error = a.error ?? "Couldn't pair."; return }
        error = nil
        errorCode = nil
        failures = 0
        paired = true
        host = a.host ?? ""
        path = a.path
        await refresh()
    }

    /// The terminals, connecting first if needed (one refresh at a time: a computer that is gone takes 30 s to say so).
    func refresh() async {
        guard paired, attempts == 0 else { return }
        let mine = epoch
        attempts += 1
        defer { if mine == epoch { attempts -= 1 } }
        let a = await core.call(["op": "sessions"])
        guard mine == epoch else { return }
        if a.ok {
            error = nil
            errorCode = nil
            failures = 0
            sessions = a.sessions ?? []
            version = a.version ?? version
            host = a.host ?? host
            path = a.path
            let p = await core.call(["op": "ping"])
            guard mine == epoch else { return }
            if p.ok { rtt = p.rtt_ms; path = p.path }
        } else {
            error = a.error
            errorCode = a.code
            path = nil
            rtt = nil
            if !needsPairing { failures += 1 }
        }
    }

    /// While the app is open: a ping every 5 s keeps the connection and shows how it goes (direct or relay), and while
    /// the list is on screen the terminals are asked again instead. When the computer doesn't answer, it tries again
    /// until the list says it can't reach it (then Try again, or pull to refresh).
    private func keepPinging() {
        pinger?.cancel()
        pinger = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(5))
                guard let self, self.paired, !self.connecting, !self.unreachable, !self.needsPairing else { continue }
                if self.error != nil {
                    await self.refresh()
                    continue
                }
                // The list on screen follows the terminals (badges, what each agent does); otherwise a ping.
                if self.listVisible {
                    await self.refresh()
                    continue
                }
                let mine = self.epoch
                let p = await self.core.call(["op": "ping"])
                guard mine == self.epoch, !Task.isCancelled else { continue }
                if p.ok { self.rtt = p.rtt_ms; self.path = p.path } else { await self.refresh() }
            }
        }
    }

    func phase(active: Bool) async {
        if active {
            if paired { await refresh() }
            keepPinging()
        } else {
            pinger?.cancel()
            _ = await core.call(["op": "disconnect"])
        }
    }

    /// Forgets the computer at once, whatever the connection is doing: the pairing screen shows now, and the core drops
    /// a connect still waiting for it.
    func forget() async {
        epoch += 1
        pinger?.cancel()
        paired = false
        host = ""
        sessions = []
        nav = NavigationPath()
        path = nil
        rtt = nil
        error = nil
        errorCode = nil
        attempts = 0
        failures = 0
        _ = await core.forget()
        keepPinging()
    }
}
