import Foundation
import Network

/// Foreground TCP link used by alpha.1 for authentication, status and RTT diagnostics.
/// BLE remains the active command/video transport until the following milestones.
@MainActor final class WiFiBoardLink: ObservableObject {
    enum State: String { case idle, connecting, authenticating, ready, failed }

    @Published private(set) var state: State = .idle
    @Published private(set) var roundTripMilliseconds: Double?
    @Published private(set) var lastError = ""
    @Published private(set) var status: BoardStatus?

    var onStatus: ((BoardStatus) -> Void)?
    var onVideo: ((AccessUnit) -> Void)?
    var onReady: ((Bool) -> Void)?
    private let queue = DispatchQueue(label: "iphone-agent-wifi-link")
    private var connection: NWConnection?
    private var parser = WiFiFrameParser()
    private var config: NetConfig?
    private var signature = ""
    private var generation = 0
    private var pingSequence: UInt32 = 10
    private var pingStarted: [UInt32: Double] = [:]
    private var retry: Task<Void, Never>?
    private var pinger: Task<Void, Never>?

    func configure(_ value: NetConfig?) {
        guard let value, let ip = value.ip, !ip.isEmpty, let token = value.token, token.count == 32 else {
            stop(); return
        }
        let next = "\(ip):\(value.port ?? 8765):\(token)"
        if next == signature, state == .ready || state == .connecting || state == .authenticating { return }
        signature = next
        config = value
        connect()
    }

    func reconnect() { if config != nil { connect() } }

    func stop() {
        generation += 1
        retry?.cancel(); pinger?.cancel()
        connection?.cancel(); connection = nil
        state = .idle
        onReady?(false)
        parser = WiFiFrameParser()
    }

    private func connect() {
        guard let config, let host = config.ip, let rawPort = NWEndpoint.Port(rawValue: UInt16(config.port ?? 8765)) else { return }
        generation += 1
        let attempt = generation
        retry?.cancel(); pinger?.cancel(); connection?.cancel()
        parser = WiFiFrameParser(); pingStarted = [:]
        state = .connecting; lastError = ""
        onReady?(false)
        let link = NWConnection(host: NWEndpoint.Host(host), port: rawPort, using: .tcp)
        connection = link
        link.stateUpdateHandler = { [weak self, weak link] newState in
            Task { @MainActor in
                guard let self, self.generation == attempt, self.connection === link else { return }
                switch newState {
                case .ready:
                    self.state = .authenticating
                    self.send(.hello, sequence: 1, payload: Data((config.token ?? "").utf8))
                    self.receive(attempt)
                case .failed(let error): self.failed(error.localizedDescription, attempt: attempt)
                case .cancelled: break
                default: break
                }
            }
        }
        link.start(queue: queue)
    }

    private func receive(_ attempt: Int) {
        connection?.receive(minimumIncompleteLength: 1, maximumLength: 65_556) { [weak self] data, _, complete, error in
            Task { @MainActor in
                guard let self, self.generation == attempt else { return }
                if let error { self.failed(error.localizedDescription, attempt: attempt); return }
                if let data, !data.isEmpty {
                    do { for frame in try self.parser.append(data) { self.handle(frame, attempt: attempt) } }
                    catch { self.failed(error.localizedDescription, attempt: attempt); return }
                }
                if complete { self.failed("board closed the Wi-Fi link", attempt: attempt) }
                else { self.receive(attempt) }
            }
        }
    }

    private func handle(_ frame: WiFiFrame, attempt: Int) {
        switch frame.type {
        case .helloReply:
            guard String(decoding: frame.payload, as: UTF8.self) == "ok" else {
                return failed("Wi-Fi authentication failed", attempt: attempt)
            }
            state = .ready
            onReady?(true)
            startPings(attempt)
        case .status:
            if let new = BoardStatus.decode(frame.payload) {
                status = status?.merged(with: new) ?? new
                onStatus?(status!)
            }
        case .videoConfig:
            onVideo?(AccessUnit(data: frame.payload, idr: false, config: true, snap: false,
                                boardMs: frame.boardMilliseconds))
        case .videoAU:
            onVideo?(AccessUnit(data: frame.payload, idr: frame.flags & 1 != 0, config: false, snap: false,
                                boardMs: frame.boardMilliseconds))
        case .pong:
            if let started = pingStarted.removeValue(forKey: frame.sequence) {
                roundTripMilliseconds = (now() - started) * 1000
            }
        case .error:
            lastError = String(decoding: frame.payload, as: UTF8.self)
        default: break
        }
    }

    private func startPings(_ attempt: Int) {
        pinger?.cancel()
        pinger = Task { [weak self] in
            while !Task.isCancelled {
                guard let self, self.generation == attempt, self.state == .ready else { return }
                self.pingSequence &+= 1
                let sequence = self.pingSequence
                self.pingStarted[sequence] = now()
                self.send(.ping, sequence: sequence, payload: Data("app".utf8))
                try? await Task.sleep(for: .seconds(2))
            }
        }
    }

    private func send(_ type: WiFiMessage, sequence: UInt32, payload: Data = Data()) {
        let data = WiFiFrameCodec.encode(type: type, sequence: sequence, payload: payload)
        connection?.send(content: data, completion: .contentProcessed { [weak self] error in
            guard let error else { return }
            Task { @MainActor in self?.failed(error.localizedDescription, attempt: self?.generation ?? -1) }
        })
    }

    private func failed(_ message: String, attempt: Int) {
        guard generation == attempt else { return }
        state = .failed; lastError = message
        onReady?(false)
        connection?.cancel(); connection = nil
        pinger?.cancel()
        retry?.cancel()
        retry = Task { [weak self] in
            try? await Task.sleep(for: .seconds(2))
            guard !Task.isCancelled, let self, self.generation == attempt else { return }
            self.connect()
        }
    }
}

