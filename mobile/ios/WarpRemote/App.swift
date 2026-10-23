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
    @Published var error: String?
    @Published var busy = false
    private let core = Core()
    private var pinger: Task<Void, Never>?

    func start() async {
        let s = await core.call(["op": "status"])
        paired = s.paired ?? false
        host = s.host ?? ""
        // Tests and screenshots: -pairTicket <code> pairs at launch.
        if !paired, let ticket = UserDefaults.standard.string(forKey: "pairTicket"), !ticket.isEmpty {
            await pair(ticket)
        } else if paired {
            await refresh()
        }
        keepPinging()
    }

    func pair(_ ticket: String) async {
        busy = true
        defer { busy = false }
        let a = await core.call(["op": "pair", "ticket": ticket.trimmingCharacters(in: .whitespacesAndNewlines)])
        guard a.ok else { error = a.error ?? "Couldn't pair."; return }
        error = nil
        paired = true
        host = a.host ?? ""
        path = a.path
        await refresh()
    }

    func refresh() async {
        let a = await core.call(["op": "sessions"])
        if a.ok {
            error = nil
            sessions = a.sessions ?? []
            host = a.host ?? host
            path = a.path
            let p = await core.call(["op": "ping"])
            if p.ok { rtt = p.rtt_ms; path = p.path }
        } else {
            error = a.error
            if a.code == "not_paired" || a.code == "revoked" { path = nil }
        }
    }

    /// While the app is open: a ping every 5 s keeps the connection and shows how it goes (direct or relay).
    private func keepPinging() {
        pinger?.cancel()
        pinger = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(5))
                guard let self, self.paired, self.error == nil else { continue }
                let p = await self.core.call(["op": "ping"])
                if p.ok { self.rtt = p.rtt_ms; self.path = p.path }
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

    func forget() async {
        _ = await core.call(["op": "forget"])
        paired = false
        host = ""
        sessions = []
        path = nil
        rtt = nil
    }
}
