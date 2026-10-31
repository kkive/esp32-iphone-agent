import Foundation

/// The board did not answer in time; the link is still up (a disconnect throws DeviceError instead).
struct BoardTimeout: LocalizedError {
    let command: String
    let seconds: Double
    var errorDescription: String? { String(format: "the board did not answer '%@' within %.0f s", command, seconds) }
}

struct DeviceError: LocalizedError {
    let message: String
    var errorDescription: String? { message }
}

/// Where command lines go: the BLE link, or a fake board in tests.
@MainActor protocol BoardTransport: AnyObject {
    var isConnected: Bool { get }
    func write(_ line: String) throws
    /// Fire-and-forget: write only if the link can take it right now; false = dropped.
    func writeIfIdle(_ line: String) -> Bool
    /// Bytes one write can carry without a response.
    var maxWrite: Int { get }
    /// Drop the link after a timeout; the transport reconnects on its own (like the old TCP close).
    func dropLink()
}

extension BoardTransport {
    func writeIfIdle(_ line: String) -> Bool { (try? write(line)) != nil }
    var maxWrite: Int { 180 }
}

/// What gestures need from the board (CommandChannel, or a fake in tests).
@MainActor protocol HIDDevice: AnyObject {
    var links: Int { get }
    var connected: Bool { get }
    @discardableResult func send(_ command: String) async throws -> String
    func waitConnected(_ timeout: TimeInterval) async -> Bool
}

/// A FIFO async mutex for main-actor code.
@MainActor final class AsyncLock {
    private var busy = false
    private var waiters: [CheckedContinuation<Void, Never>] = []

    func lock() async {
        if !busy { busy = true; return }
        await withCheckedContinuation { waiters.append($0) }
    }

    func unlock() {
        if waiters.isEmpty { busy = false } else { waiters.removeFirst().resume() }
    }
}

/// Sends the board commands one at a time and matches replies by request id. Port of device.py.
@MainActor final class CommandChannel: HIDDevice {
    weak var transport: BoardTransport?
    private(set) var links = 0  // bumped on every (re)connect, so gestures know to release and re-home
    private let lock = AsyncLock()
    /// The board serves two centrals (this app and a Mac harness) and notifies replies to both: start our ids
    /// far from the harness's small ones, and drop replies to ids we did not send.
    private var nextID = Int.random(in: 100_000...800_000)
    private var logTimes: [Double] = []
    /// "app <command>" lines relayed from the Mac harness.
    var onAppCommand: ((String) -> Void)?
    private var pending: [Int: CheckedContinuation<BoardReply, Error>] = [:]
    private var lastInput = 0.0, beat = 0.0
    private var mouseHeld = false
    private let clock: () -> Double
    var onTiming: ((String, Double) -> Void)?  // command name, ms

    init(transport: BoardTransport? = nil, clock: @escaping () -> Double = { ProcessInfo.processInfo.systemUptime }) {
        self.transport = transport
        self.clock = clock
    }

    var connected: Bool { transport?.isConnected ?? false }

    /// The transport (re)connected and subscribed.
    func linkUp() {
        links += 1
        lastInput = clock(); beat = lastInput
        mouseHeld = false
    }

    /// The link dropped: fail everything in flight.
    func linkDown() {
        mouseHeld = false
        let waiting = pending
        pending = [:]
        waiting.values.forEach { $0.resume(throwing: DeviceError(message: "board disconnected")) }
    }

    func handleReply(_ line: String) {
        let text = line.trimmingCharacters(in: .whitespacesAndNewlines)
        if text.hasPrefix("app ") { return onAppCommand?(String(text.dropFirst(4))) ?? () }
        if text.hasPrefix("log ") { return }  // our own log lines, relayed to the harness
        guard let reply = BoardReply.parse(line), let c = pending.removeValue(forKey: reply.id) else { return }
        c.resume(returning: reply)
    }

    func waitConnected(_ timeout: TimeInterval) async -> Bool {
        let deadline = now() + timeout
        while !connected && now() < deadline { try? await Task.sleep(nanoseconds: 200_000_000) }
        return connected
    }

    /// Seconds the board may take: replies need up to ~2 s at p99, moves are paced (10 units per 8 ms),
    /// typing is per character.
    nonisolated static func timeout(_ command: String) -> Double {
        let parts = command.split(separator: " ", maxSplits: 1).map(String.init)
        let name = parts.first ?? "", arg = parts.count > 1 ? parts[1] : ""
        switch name {
        case "move": return 5 + Double(arg.split(separator: " ").compactMap { Int($0) }.map(abs).max() ?? 0) / 10 * 0.012
        case "type": return 5 + 0.1 * Double(arg.count)
        case "ping": return 8  // an idle check: tolerate a hiccup rather than drop a healthy board
        case "snap": return 15  // a whole GOP over BLE
        case "bench": return 5 + (Double(arg) ?? 0) / 10  // KB at >= 80 kbps
        default: return 5
        }
    }

    /// Called once when a command times out (e.g. video starving control traffic): the app turns the
    /// stream off, then the command is retried once.
    var onTimeout: ((String) async -> Void)?

    /// Send one command; returns the text after "ok" (e.g. bench's ms). Throws DeviceError on err/timeout.
    @discardableResult
    func send(_ command: String) async throws -> String {
        let id = nextID
        nextID += 1
        do {
            return try await sendAttempt(command, timeout: nil, id: id)
        } catch let e as BoardTimeout {
            guard command != "ping", !command.hasPrefix("stream"), let onTimeout else { throw e }
            await onTimeout(command)
            DebugLog.write("cmd retry after timeout: \(command.prefix(40))")
            // Keep the request id: firmware caches the last 32 replies, so a lost reply never repeats a gesture.
            return try await sendAttempt(command, timeout: nil, id: id)
        }
    }

    /// A timeout is the board being slow (e.g. a snap while it also streams), not a lost link: it throws
    /// BoardTimeout and keeps the connection; only a real CoreBluetooth disconnect fails as link lost.
    @discardableResult
    func send(_ command: String, timeout custom: Double?) async throws -> String {
        let id = nextID
        nextID += 1
        return try await sendAttempt(command, timeout: custom, id: id)
    }

    private func sendAttempt(_ command: String, timeout custom: Double?, id: Int) async throws -> String {
        await lock.lock()
        defer { lock.unlock() }
        return try await sendLocked(command, timeout: custom, id: id)
    }

    /// Caller holds the command lock, including both halves of an idle pointer pulse.
    private func sendLocked(_ command: String, timeout custom: Double?, id: Int) async throws -> String {
        guard connected, let transport else { throw DeviceError(message: "board not connected") }
        let timeout = custom ?? Self.timeout(command)
        let name = String(command.split(separator: " ").first ?? "")
        let started = clock()
        let reply: BoardReply
        do {
            reply = try await withTaskCancellationHandler {
                try await withCheckedThrowingContinuation { (c: CheckedContinuation<BoardReply, Error>) in
                    pending[id] = c
                    do {
                        try transport.write("\(id) \(command)")
                        // A lost press reply may still mean the button is held. Wait for a release.
                        if name == "press" { mouseHeld = true }
                    } catch {
                        pending[id] = nil
                        c.resume(throwing: error)
                        return
                    }
                    Task { @MainActor [weak self] in
                        try? await Task.sleep(nanoseconds: UInt64(timeout * 1e9))
                        self?.fail(id, DeviceError(message: "timeout"))
                    }
                }
            } onCancel: {
                Task { @MainActor [weak self] in self?.fail(id, CancellationError()) }
            }
        } catch is CancellationError {
            throw CancellationError()
        } catch {
            let ms = (clock() - started) * 1000
            if (error as? DeviceError)?.message == "timeout" {
                if name != "ping" { DebugLog.write(String(format: "cmd #%d %@ → timeout after %.0f ms", id, String(command.prefix(40)), ms)) }
                throw BoardTimeout(command: String(command.prefix(30)), seconds: timeout)
            }
            DebugLog.write(String(format: "cmd #%d %@ → link lost after %.0f ms: %@", id, String(command.prefix(40)), ms,
                                  error.localizedDescription))
            throw DeviceError(message: "board link lost during '\(command.prefix(30))': \(error.localizedDescription)")
        }
        beat = clock()  // transport activity is not necessarily phone input
        if command != "ping" {
            onTiming?(name, (clock() - started) * 1000)
        }
        if name != "ping" {
            DebugLog.write(String(format: "cmd #%d %@ → %@ %@ (%.0f ms)", id, String(command.prefix(40)),
                                  reply.ok ? "ok" : "err", String(reply.text.prefix(60)), (clock() - started) * 1000))
        }
        guard reply.ok else { throw DeviceError(message: "err " + reply.text) }
        if Self.isPhoneInput(name) { lastInput = beat }
        if name == "release" || name == "click" { mouseHeld = false }
        return reply.text
    }

    private static func isPhoneInput(_ name: String) -> Bool {
        ["home", "homescreen", "move", "press", "release", "click", "scroll", "type", "key"].contains(name)
    }

    /// Send a debug-log line to the Mac harness through the board (`log <text>`): fire-and-forget, at most
    /// 10 a second, truncated to one write, dropped when the link is busy.
    @discardableResult
    func relayLog(_ text: String) -> Bool {
        guard connected, let transport else { return false }
        let t = now()
        logTimes = logTimes.filter { t - $0 < 1 }
        guard logTimes.count < 10 else { return false }
        let prefix = "\(nextID) log "
        nextID += 1
        var body = text.replacingOccurrences(of: "\n", with: " ")
        while prefix.utf8.count + body.utf8.count > transport.maxWrite && !body.isEmpty { body.removeLast() }
        guard transport.writeIfIdle(prefix + body) else { return false }
        logTimes.append(t)
        return true
    }

    private func fail(_ id: Int, _ error: Error) {
        pending.removeValue(forKey: id)?.resume(throwing: error)
    }

    /// Poll independently of reply traffic. Non-input commands (snap, stream, configuration) must not
    /// postpone phone input; 10 s leaves headroom before a 30 s auto-lock, even during slow model calls.
    func keepAwake(idle: Double = 10, heartbeat: Double = 2, screenLive: @escaping () -> Bool,
                   taskActive: @escaping () -> Bool) async {
        while !Task.isCancelled {
            do {
                try await Task.sleep(nanoseconds: UInt64(heartbeat * 1e9))
                try await keepAwakeTick(idle: idle, heartbeat: heartbeat, screenLive: screenLive, taskActive: taskActive)
            } catch is CancellationError {
                return
            } catch {
                DebugLog.write("keep-awake: \(error.localizedDescription)")
            }
        }
    }

    /// Recheck after waiting for in-flight commands: a task may have stopped while we were queued.
    /// A pulse holds the FIFO lock for both moves, so it never straddles a tap/drag command.
    func keepAwakeTick(idle: Double = 10, heartbeat: Double = 2, screenLive: () -> Bool,
                       taskActive: () -> Bool) async throws {
        await lock.lock()
        defer { lock.unlock() }
        try Task.checkCancellation()
        guard connected else { return }
        func sendIdle(_ command: String) async throws {
            let id = nextID
            nextID += 1
            _ = try await sendLocked(command, timeout: nil, id: id)
        }
        if taskActive() && screenLive() && !mouseHeld && clock() - lastInput >= idle {
            DebugLog.write(String(format: "keep-awake: active task, input idle %.1f s; pointer pulse", clock() - lastInput))
            try await sendIdle("move 1 0")
            try await sendIdle("move -1 0")
        } else if clock() - beat >= heartbeat {
            try await sendIdle("ping")
        }
    }

    var log: (String) -> Void = { print($0) }
}

func now() -> Double { Date().timeIntervalSince1970 }
