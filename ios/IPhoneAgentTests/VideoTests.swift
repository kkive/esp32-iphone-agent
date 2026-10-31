import XCTest
@testable import IPhoneAgent

/// `video` chunk reassembly (docs/ble-protocol.md).
final class VideoTests: XCTestCase {
    typealias F = VideoAssembler.Flags

    func chunk(_ seq: UInt16, _ flags: F, _ payload: [UInt8], ms: UInt32? = nil) -> Data {
        var d = Data([UInt8(seq & 0xff), UInt8(seq >> 8), flags.rawValue])
        if let ms { d.append(contentsOf: [UInt8(ms & 0xff), UInt8(ms >> 8 & 0xff), UInt8(ms >> 16 & 0xff), UInt8(ms >> 24)]) }
        d.append(contentsOf: payload)
        return d
    }

    let sps: [UInt8] = [0, 0, 0, 1, 0x67, 1, 2]
    let idr: [UInt8] = [0, 0, 0, 1, 0x65, 9, 9]
    let p: [UInt8] = [0, 0, 0, 1, 0x41, 7]

    func testInOrderChunksMakeAccessUnits() {
        var a = VideoAssembler()
        XCTAssertEqual(a.add(chunk(0, [.start, .end, .config], sps, ms: 1000))?.config, true)
        XCTAssertNil(a.add(chunk(1, [.start, .idr], Array(idr[0..<5]), ms: 1001)))
        let unit = a.add(chunk(2, [.end, .idr], Array(idr[5...])))
        XCTAssertEqual(unit, AccessUnit(data: Data(idr), idr: true, config: false, snap: false, boardMs: 1001))
        XCTAssertEqual(a.add(chunk(3, [.start, .end], p, ms: 1200))?.data, Data(p))
        XCTAssertEqual(a.drops, 0)
    }

    func testGapDropsUntilTheNextKeyUnit() {
        var a = VideoAssembler()
        _ = a.add(chunk(10, [.start, .end, .idr], idr, ms: 1))
        XCTAssertNil(a.add(chunk(11, [.start], p, ms: 2)))
        XCTAssertNil(a.add(chunk(13, [.end], p)))           // 12 lost: the half unit is dropped
        XCTAssertEqual(a.drops, 1)
        XCTAssertNil(a.add(chunk(14, [.start, .end], p, ms: 3)))  // a P-frame after a gap is useless
        XCTAssertNotNil(a.add(chunk(15, [.start, .end, .config], sps, ms: 4)))  // recovered on CONFIG
        XCTAssertNotNil(a.add(chunk(16, [.start, .end], p, ms: 5)))
    }

    func testRecoversOnIDRAndWrapsSeq() {
        var a = VideoAssembler()
        _ = a.add(chunk(65534, [.start, .end, .idr], idr, ms: 1))
        XCTAssertNotNil(a.add(chunk(65535, [.start, .end], p, ms: 2)))
        XCTAssertNotNil(a.add(chunk(0, [.start, .end], p, ms: 3)))  // u16 wrap is not a gap
        XCTAssertEqual(a.drops, 0)
        XCTAssertNil(a.add(chunk(5, [.start, .end], p, ms: 4)))
        XCTAssertEqual(a.drops, 4)
        let unit = a.add(chunk(6, [.start, .end, .idr, .snap], idr, ms: 5))
        XCTAssertEqual(unit?.snap, true)
    }

    func testWaitsForAKeyUnitAtStartAndIgnoresBench() {
        var a = VideoAssembler()
        XCTAssertNil(a.add(chunk(0, [.start, .end], p, ms: 1)))
        XCTAssertNil(a.add(chunk(1, [.bench], [UInt8](repeating: 0, count: 100))))
        XCTAssertEqual(a.benchBytes, 103)
        XCTAssertNotNil(a.add(chunk(2, [.start, .end, .idr], idr, ms: 2)))
    }

    func testAnnexBSplitsNALUnits() {
        let units = annexBUnits(Data(sps + [0, 0, 1, 0x68, 5] + idr))
        XCTAssertEqual(units.map { $0.first! & 0x1f }, [7, 8, 5])
        XCTAssertEqual(units[0], Data([0x67, 1, 2]))
    }

    @MainActor func testLiveStartupWaitsForDecoderRecoveryBeyondInitialTimeoutWithoutSnapshot() async throws {
        let device = FakeDevice()
        let feed = VideoFeed()
        feed.device = device
        feed.mirroring = true
        try await feed.stream(true)
        feed.show(Data([1]), width: 480, height: 1040)
        feed.decoderRecoveryChanged(true)
        XCTAssertFalse(feed.live)
        let arrival = Task { @MainActor in
            try await Task.sleep(nanoseconds: 100_000_000)
            feed.show(Data([2]), width: 480, height: 1040)
            feed.decoderRecoveryChanged(false)
        }
        let source = try await feed.currentFrame(wait: 0, recoveryWait: 1)
        try await arrival.value
        XCTAssertEqual(source, "live (recovered)")
        XCTAssertEqual(feed.frame, Data([2]))
        XCTAssertEqual(device.sent, ["stream on"], "recovery must not request the board's possibly empty GOP")
    }

    @MainActor func testLiveRecoveryWorksWithAnEmptyBoardGOP() async throws {
        let device = FakeDevice()
        let feed = VideoFeed()
        let metrics = Metrics()
        metrics.board = BoardStatus.decode(Data(#"{"ap":"streaming","gop":0,"gover":2,"gwait":0,"gcfg":1}"#.utf8))
        feed.metrics = metrics
        feed.device = device
        feed.mirroring = true
        try await feed.stream(true)
        let arrival = Task { @MainActor in
            try await Task.sleep(nanoseconds: 80_000_000)
            feed.show(Data([3]), width: 480, height: 1040)
        }
        let source = try await feed.currentFrame(wait: 0, recoveryWait: 1)
        try await arrival.value
        XCTAssertEqual(source, "live (recovered)")
        XCTAssertEqual(device.sent, ["stream on"])
    }

    @MainActor func testLiveRecoveryTimeoutRejectsOldScreenAndNeverFallsBackToSnapshot() async throws {
        let device = FakeDevice()
        let feed = VideoFeed()
        feed.device = device
        feed.mirroring = true
        try await feed.stream(true)
        feed.show(Data([1]), width: 480, height: 1040)
        feed.decoderRecoveryChanged(true)
        feed.decoderRecoveryChanged(false)  // the old image is not a new decoder output
        do {
            _ = try await feed.currentFrame(wait: 0.01, recoveryWait: 0.06)
            XCTFail("expected bounded live timeout")
        } catch {
            XCTAssertTrue(error.localizedDescription.contains("live video did not recover"))
        }
        XCTAssertEqual(device.sent, ["stream on"])
    }

    @MainActor func testLiveFrameWaitStopsOnCancellationAndDoesNotSendSnapshot() async throws {
        let device = FakeDevice()
        let feed = VideoFeed()
        feed.device = device
        feed.mirroring = true
        try await feed.stream(true)
        let waiting = Task { try await feed.currentFrame(wait: 2, recoveryWait: 3) }
        await Task.yield()
        waiting.cancel()
        do { _ = try await waiting.value; XCTFail("expected cancellation") }
        catch { XCTAssertTrue(error is CancellationError) }
        XCTAssertEqual(device.sent, ["stream on"])
    }

    @MainActor func testLiveFrameWaitStopsWhenMirroringStops() async throws {
        let device = FakeDevice()
        let feed = VideoFeed()
        feed.device = device
        feed.mirroring = true
        try await feed.stream(true)
        feed.mirroring = false
        do { _ = try await feed.currentFrame(wait: 0.01); XCTFail("expected mirror stopped") }
        catch { XCTAssertTrue(error.localizedDescription.contains("Screen Mirroring stopped")) }
        XCTAssertEqual(device.sent, ["stream on"])
    }

    @MainActor func testExplicitSnapshotModeStillRequestsSnapshot() async throws {
        let feed = VideoFeed()
        let device = SnapshotTestDevice { feed.show(Data([4]), width: 480, height: 1040) }
        feed.device = device
        feed.mirroring = true
        let source = try await feed.currentFrame()
        XCTAssertEqual(source, "snap")
        XCTAssertEqual(device.sent, ["snap"])
        XCTAssertEqual(feed.frame, Data([4]))
    }
}

@MainActor private final class SnapshotTestDevice: HIDDevice {
    var links = 1
    var connected = true
    var sent: [String] = []
    let onSnap: () -> Void
    init(onSnap: @escaping () -> Void) { self.onSnap = onSnap }
    func send(_ command: String) async throws -> String {
        sent.append(command)
        if command == "snap" { onSnap() }
        return ""
    }
    func waitConnected(_ timeout: TimeInterval) async -> Bool { connected }
}
