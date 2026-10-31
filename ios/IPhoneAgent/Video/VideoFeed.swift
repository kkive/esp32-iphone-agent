import Foundation
import UIKit

/// What the agent and the pointer need from the screen (VideoFeed, or a fake in tests). Port of screen.py.
@MainActor protocol ScreenSource: AnyObject {
    var live: Bool { get }
    var frame: Data? { get }
    var updated: Double { get }
    var capturedAt: Double { get }
    var aspect: Double { get }
    /// After a gesture that ended at `since`: wait until the screen shows its result (and fetch it, in snap mode).
    func afterAction(since: Double) async
}

/// How a run gets its frames: the live stream (`stream on`, the default: a frame in under a second), or one
/// snap after each action (`stream off`; waits for the board's whole GOP, often 20-30 s).
enum FrameMode: String {
    case stream, snap
    static let key = "frameModeV2"  // V2: drops the old saved "snap" left by earlier automatic fallbacks
    /// A command timeout switches the current run to snap; not saved, so the next run streams again.
    static var fallback: FrameMode?
    static var current: FrameMode {
        get { fallback ?? FrameMode(rawValue: UserDefaults.standard.string(forKey: key) ?? "") ?? .stream }
        set { UserDefaults.standard.set(newValue.rawValue, forKey: key); fallback = nil }
    }
}

extension ScreenSource {
    func afterAction(since: Double) async { await settled() }

    /// Wait until the screen stops changing (AirPlay only sends frames on change). Mid-animation frames are big and
    /// can reach us late over BLE, so a gap is only "quiet" once it also covers the last frame's transport lag.
    func settled(quiet: Double = 0.5, least: Double = 0.3, most: Double = 4) async {
        let started = now()
        try? await Task.sleep(nanoseconds: UInt64(least * 1e9))
        while now() - updated < quiet + lag && now() - started < most {
            try? await Task.sleep(nanoseconds: 100_000_000)
        }
    }

    /// How late the shown frame arrived after the board got it (0 when unknown), capped.
    private var lag: Double { capturedAt > 0 ? min(max(updated - capturedAt, 0), 1) : 0 }

    /// The first frame captured after `since` (received, when capture times are unknown), or nil if none in time.
    func fresh(since: Double, timeout: Double = 0.8) async -> Data? {
        let deadline = now() + timeout
        while (capturedAt > 0 ? capturedAt : updated) <= since {
            if now() > deadline { return nil }
            try? await Task.sleep(nanoseconds: 20_000_000)
        }
        return frame
    }
}

/// The phone's screen, mirrored over AirPlay to the board and forwarded over BLE, as JPEG frames.
@MainActor final class VideoFeed: ObservableObject, ScreenSource {
    @Published private(set) var frame: Data?
    @Published private(set) var size = CGSize.zero
    @Published private(set) var note = ""  // last decoder problem, shown in Diagnostics
    private(set) var updated = 0.0
    /// When the board received the shown frame, in local time (≈; see `boardClock`).
    private(set) var capturedAt = 0.0
    /// A decode error or a lost chunk since the last good frame: the shown frame is stale until the next IDR.
    private(set) var broken = false
    private var clockOffset: Double?  // local s − board s, the smallest seen (≈ the link latency)
    var mirroring = false  // board status says ap == streaming
    @Published private(set) var waitingForKeyframe = false
    @Published private(set) var decoderRecovering = false
    private var frameRevision: UInt64 = 0
    private var recoveryFrameRevision: UInt64 = 0
    var metrics: Metrics?
    weak var device: HIDDevice?

    private var assembler = VideoAssembler()
    private let decoder = H264Decoder()
    private var wifiAvailable = false

    init() {
        decoder.onFrame = { [weak self] f in DispatchQueue.main.async { self?.publish(f) } }
        decoder.onRecoveryChanged = { [weak self] recovering in
            DispatchQueue.main.async { self?.decoderRecoveryChanged(recovering) }
        }
        decoder.onError = { [weak self] msg in
            DispatchQueue.main.async {
                let bg = UIApplication.shared.applicationState == .background
                self?.note = bg ? "\(msg) [in background]" : msg
                self?.metrics?.decodeError(background: bg)
                self?.broken = true
            }
        }
    }

    var live: Bool {
        mirroring && !waitingForKeyframe && !decoderRecovering && !broken && frame != nil
            && frameRevision >= recoveryFrameRevision
            && (device?.connected ?? false)
    }

    func decoderRecoveryChanged(_ recovering: Bool) {
        decoderRecovering = recovering
        if recovering { recoveryFrameRevision = frameRevision + 1 }
        DebugLog.write("video: decoder recovery " + (recovering ? "started" : "finished"))
    }

    func updateKeyframeStatus(_ waiting: Bool?) {
        guard let waiting else { return }
        if waiting && !waitingForKeyframe {
            DebugLog.write("video: board is waiting for a keyframe; discard stale screen")
            clearFrame()
        }
        waitingForKeyframe = waiting
    }

    /// Drop the last image when mirroring stops, so a later connection never presents an old screen as live.
    func clearFrame() { frame = nil; updated = 0; capturedAt = 0; broken = false }

    /// Frame width / height (portrait iPhone as a fallback).
    var aspect: Double { size.width > 0 && size.height > 0 ? size.width / size.height : 9 / 19.5 }

    /// One `video` notify from BoardLink.
    func receive(_ chunk: Data) {
        metrics?.videoBytes(chunk.count)
        let dropsBefore = assembler.drops
        let unit = assembler.add(chunk)
        if assembler.drops > dropsBefore { metrics?.videoDrops(assembler.drops - dropsBefore); broken = true }
        guard let unit else { return }
        // Snap is explicitly requested on BLE even when Wi-Fi carries the live stream.
        // Discarding all BLE data here also discarded recovery snapshots.
        if wifiAvailable && !unit.snap { return }
        if !unit.snap { boardClock(ms: Int(unit.boardMs)) }  // snap units are resent late: their ages say nothing
        if unit.idr { metrics?.idrSeen() }
        decoder.queue.async { [decoder] in decoder.decode(unit) }
    }

    /// One complete Annex-B access unit from the authenticated Wi-Fi link.
    func receiveWiFi(_ unit: AccessUnit) {
        metrics?.wifiVideoBytes(unit.data.count)
        boardClock(ms: Int(unit.boardMs))
        if unit.idr { metrics?.idrSeen() }
        decoder.queue.async { [decoder] in decoder.decode(unit) }
    }

    /// Selects Wi-Fi immediately when authenticated and BLE immediately after a Wi-Fi drop.
    func wifiReady(_ ready: Bool) {
        wifiAvailable = ready
        if !ready { assembler.reset() }
    }

    /// The link (re)connected: chunk numbering starts over.
    func resetStream() { assembler.reset(); clockOffset = nil }  // the board may have rebooted: its clock too

    /// A board timestamp (status `up`, or a streamed unit) just arrived: keep the smallest offset.
    func boardClock(ms: Int) {
        let offset = now() - Double(ms) / 1000
        clockOffset = min(clockOffset ?? offset, offset)
    }

    /// Show a still frame (demo data).
    func show(_ jpeg: Data, width: Int, height: Int) {
        publish(H264Decoder.Frame(jpeg: jpeg, width: width, height: height, decodeMs: 4.2,
                                  unit: AccessUnit(data: Data(), idr: false, config: false, snap: false, boardMs: 0)))
    }

    private func publish(_ f: H264Decoder.Frame) {
        guard !waitingForKeyframe else { return }
        frameRevision += 1
        frame = f.jpeg
        updated = now()
        capturedAt = clockOffset.map { Double(f.unit.boardMs) / 1000 + $0 } ?? updated
        broken = false
        if size.width != CGFloat(f.width) || size.height != CGFloat(f.height) {
            size = CGSize(width: f.width, height: f.height)
        }
        metrics?.frameDecoded(ms: f.decodeMs)
    }

    /// The app asked the board to forward the stream (`stream on`).
    private(set) var streamOn = false

    /// Seconds a snap may take: the buffered GOP over the measured BLE rate, doubled, at least 8 s.
    var snapTimeout: Double {
        let gopBytes = Double(metrics?.board?.gop ?? 0)
        let kbps = max(metrics?.bleKbps ?? 0, metrics?.board?.ble?.kbps ?? 0, 40)
        return max(8, gopBytes * 8 / 1000 / kbps * 2 + 3)
    }

    /// The phone can invalidate its decoder on entering background. Replaying a long, locally cached
    /// GOP then takes several seconds. Never inject a BLE snapshot into an active live decoder:
    /// the board's replay cache may be empty even though the live dependency chain is intact.
    /// Explicit snapshot mode retains its own bounded retry path.
    @discardableResult
    func currentFrame(maxAge: Double = 1, wait: Double = 3, recoveryWait: Double = 30) async throws -> String {
        if waitingForKeyframe { throw DeviceError(message: "waiting for video keyframe; restart Screen Mirroring") }
        if streamOn {
            let since = now() - maxAge
            let started = ProcessInfo.processInfo.systemUptime
            if try await waitForLiveFrame(since: since, timeout: 0) { return "live" }
            if try await waitForLiveFrame(since: since, timeout: wait) { return "live (waited)" }
            DebugLog.write(String(format: "video: waiting for live recovery (decoder %@, GOP %d bytes); no snapshot fallback",
                                  decoderRecovering ? "replaying" : "pending", metrics?.board?.gop ?? -1))
            let remaining = max(0, recoveryWait - (ProcessInfo.processInfo.systemUptime - started))
            if try await waitForLiveFrame(since: since, timeout: remaining) { return "live (recovered)" }
            throw DeviceError(message: "live video did not recover in time; no fresh screen received")
        }
        do {
            try await snap()
        } catch is BoardTimeout {
            DebugLog.write("snap timed out; retrying once with a longer timeout")
            try await snap(timeout: snapTimeout * 1.5)
        }
        return "snap"
    }

    private func waitForLiveFrame(since: Double, timeout: Double) async throws -> Bool {
        let deadline = ProcessInfo.processInfo.systemUptime + max(0, timeout)
        while true {
            try Task.checkCancellation()
            guard streamOn, mirroring else { throw DeviceError(message: "Screen Mirroring stopped while waiting for video") }
            guard device?.connected == true else { throw DeviceError(message: "board disconnected while waiting for video") }
            if waitingForKeyframe { throw DeviceError(message: "waiting for video keyframe; live chain needs resynchronization") }
            if live, updated >= since, capturedAt == 0 || capturedAt >= since { return true }
            if ProcessInfo.processInfo.systemUptime >= deadline { return false }
            try await Task.sleep(nanoseconds: 20_000_000)
        }
    }

    /// Ask the board for the current GOP and wait until it is decoded. Returns the latency in ms.
    @discardableResult
    func snap(timeout: Double? = nil) async throws -> Double {
        guard let device else { throw DeviceError(message: "board not connected") }
        let started = now()
        if let channel = device as? CommandChannel {
            try await channel.send("snap", timeout: timeout ?? snapTimeout)
        } else {
            try await device.send("snap")
        }
        await withCheckedContinuation { c in decoder.queue.async { c.resume() } }  // the GOP is decoded
        try? await Task.sleep(nanoseconds: 30_000_000)  // let the last frame reach the main queue
        let ms = (now() - started) * 1000
        metrics?.snapLatency(ms)
        return ms
    }

    func stream(_ on: Bool) async throws {
        try await device?.send("stream \(on ? "on" : "off")")
        streamOn = on
    }

    /// The model must never see a frame from before the action that ended at `since`. Stream: let the screen
    /// settle, then wait (bounded) for a frame captured after the action; none while the stream is healthy means
    /// the screen did not change, but a broken stream (waiting for an IDR) needs a snap. Snap: give the UI a moment,
    /// snap, and snap once more if the GOP still ends before the action.
    func afterAction(since: Double) async {
        do {
            if FrameMode.current == .snap {
                try await Task.sleep(nanoseconds: 700_000_000)
                try await currentFrame()
                guard capturedAt < since else { return }
                DebugLog.write(String(format: "frame: snap ends %.1f s before the action; snapping again", since - capturedAt))
                try await Task.sleep(nanoseconds: 500_000_000)
                try await currentFrame()
            } else {
                await settled()
                if await newer(than: since, timeout: 1.5) { return }
                guard broken || decoderRecovering || !streamOn else { return DebugLog.write("frame: none after the action; screen unchanged") }
                DebugLog.write("frame: stream stale; waiting for current video")
                try await currentFrame()
            }
        } catch {
            DebugLog.write("frame after action failed: \(error.localizedDescription)")
        }
        if capturedAt < since { DebugLog.write(String(format: "frame: still %.1f s older than the action", since - capturedAt)) }
    }

    /// Waits until the shown frame was captured after `t`; false on timeout.
    private func newer(than t: Double, timeout: Double) async -> Bool {
        let deadline = now() + timeout
        while capturedAt < t {
            if now() > deadline { return false }
            try? await Task.sleep(nanoseconds: 50_000_000)
        }
        return true
    }

    /// The link dropped: the board forgets `stream on`.
    func linkLost() { streamOn = false }
}
