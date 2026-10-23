import SwiftUI

struct ContentView: View {
    @EnvironmentObject var model: Model

    var body: some View {
        NavigationStack {
            Group {
                if model.paired { HostView() } else { PairView() }
            }
            .background(Color(white: 0.02).ignoresSafeArea())
        }
        .task { await model.start() }
    }
}

/// Not paired yet: scan the code Warp shows (title bar → Pair phone…) or paste it.
struct PairView: View {
    @EnvironmentObject var model: Model
    @State private var ticket = ""
    @State private var scanning = false

    var body: some View {
        Form {
            Section {
                Text("On your computer, right-click Warp's title bar and choose **Pair phone…**")
                    .foregroundStyle(.secondary)
                Button {
                    scanning = true
                } label: {
                    Label("Scan the code", systemImage: "qrcode.viewfinder")
                }
            }
            Section("Or paste the code") {
                TextField("WARP1…", text: $ticket, axis: .vertical)
                    .font(.system(.footnote, design: .monospaced))
                    .textInputAutocapitalization(.characters)
                    .autocorrectionDisabled()
                    .lineLimit(3...6)
                Button {
                    Task { await model.pair(ticket) }
                } label: {
                    if model.busy { ProgressView() } else { Text("Pair") }
                }
                .disabled(ticket.isEmpty || model.busy)
            }
            if let e = model.error {
                Section { Text(e).foregroundStyle(.red) }
            }
        }
        .scrollContentBackground(.hidden)
        .navigationTitle("Pair with Warp")
        .sheet(isPresented: $scanning) {
            ScannerView { code in
                scanning = false
                ticket = code
                Task { await model.pair(code) }
            }
            .ignoresSafeArea()
        }
    }
}

/// Paired: how the connection goes and every terminal of every Warp window on the computer.
struct HostView: View {
    @EnvironmentObject var model: Model
    @State private var confirmForget = false

    private var windows: [(UInt64, [Session])] {
        var order: [UInt64] = []
        var groups: [UInt64: [Session]] = [:]
        for s in model.sessions {
            if groups[s.window] == nil { order.append(s.window) }
            groups[s.window, default: []].append(s)
        }
        return order.map { ($0, groups[$0]!) }
    }

    var body: some View {
        List {
            Section {
                ConnectionRow(path: model.path, rtt: model.rtt)
                if let e = model.error {
                    Text(e).foregroundStyle(.red).font(.footnote)
                }
            }
            ForEach(Array(windows.enumerated()), id: \.element.0) { index, group in
                Section(windows.count > 1 ? "Window \(index + 1)" : "Terminals") {
                    ForEach(group.1) { s in SessionRow(session: s) }
                }
            }
            if model.sessions.isEmpty && model.error == nil {
                Text("No terminals yet. Pull to refresh.").foregroundStyle(.secondary)
            }
        }
        .scrollContentBackground(.hidden)
        .refreshable { await model.refresh() }
        .navigationTitle(model.host.isEmpty ? "Warp" : model.host)
        .toolbar {
            Menu {
                Button("Forget this computer", role: .destructive) { confirmForget = true }
            } label: {
                Image(systemName: "ellipsis.circle")
            }
        }
        .confirmationDialog("Forget \(model.host)? You'll need a new pairing code from Warp.", isPresented: $confirmForget,
                            titleVisibility: .visible) {
            Button("Forget", role: .destructive) { Task { await model.forget() } }
        }
    }
}

struct ConnectionRow: View {
    var path: NetPath?
    var rtt: Double?

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: icon).foregroundStyle(color).font(.title3)
            VStack(alignment: .leading, spacing: 2) {
                Text(label).font(.headline)
                if let a = path?.addr, !a.isEmpty {
                    Text(a).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                }
            }
            Spacer()
            if let rtt {
                Text(String(format: "%.0f ms", rtt)).font(.system(.body, design: .monospaced)).foregroundStyle(.secondary)
            }
        }
    }

    private var label: String {
        switch path?.kind {
        case "direct": return "Direct connection"
        case "relay": return "Through a relay"
        default: return "Not connected"
        }
    }
    private var icon: String {
        switch path?.kind {
        case "direct": return "bolt.horizontal.circle.fill"
        case "relay": return "arrow.triangle.swap"
        default: return "wifi.slash"
        }
    }
    private var color: Color {
        switch path?.kind {
        case "direct": return .green
        case "relay": return .orange
        default: return .secondary
        }
    }
}

struct SessionRow: View {
    var session: Session

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Circle()
                .fill(session.busy ? Color.green : Color.secondary.opacity(0.4))
                .frame(width: 8, height: 8)
                .padding(.top, 6)
            VStack(alignment: .leading, spacing: 3) {
                HStack {
                    Text(session.title).font(.body.weight(.medium)).lineLimit(1)
                    if let agent = session.agent, !agent.isEmpty {
                        Text(agent == "claude" ? "Claude" : agent == "codex" ? "Codex" : agent)
                            .font(.caption2.weight(.semibold))
                            .padding(.horizontal, 6).padding(.vertical, 2)
                            .background(Capsule().fill(Color.accentColor.opacity(0.25)))
                    }
                }
                Text(session.tab_name == session.title ? folder : "\(session.tab_name) · \(folder)")
                    .font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
        }
        .padding(.vertical, 2)
    }

    private var folder: String {
        let parts = session.cwd.split(whereSeparator: { $0 == "\\" || $0 == "/" })
        return parts.last.map(String.init) ?? session.cwd
    }
}
