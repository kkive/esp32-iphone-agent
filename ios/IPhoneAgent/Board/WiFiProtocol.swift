import Foundation

enum WiFiMessage: UInt8 {
    case hello = 1, helloReply = 2, videoConfig = 3, videoAU = 4, status = 5
    case command = 6, reply = 7, ping = 8, pong = 9, error = 10
}

struct WiFiFrame: Equatable {
    let type: WiFiMessage
    let flags: UInt8
    let sequence: UInt32
    let boardMilliseconds: UInt32
    let payload: Data
}

enum WiFiFrameCodec {
    static let headerLength = 20
    static let maximumPayload = 65_536

    static func encode(type: WiFiMessage, sequence: UInt32, boardMilliseconds: UInt32 = 0,
                       payload: Data = Data(), flags: UInt8 = 0) -> Data {
        var out = Data("IAW1".utf8)
        out.append(type.rawValue)
        out.append(flags)
        append(UInt16(headerLength), to: &out)
        append(UInt32(payload.count), to: &out)
        append(sequence, to: &out)
        append(boardMilliseconds, to: &out)
        out.append(payload)
        return out
    }

    private static func append(_ value: UInt16, to data: inout Data) {
        data.append(UInt8(value & 0xff)); data.append(UInt8(value >> 8))
    }

    private static func append(_ value: UInt32, to data: inout Data) {
        data.append(UInt8(value & 0xff)); data.append(UInt8((value >> 8) & 0xff))
        data.append(UInt8((value >> 16) & 0xff)); data.append(UInt8(value >> 24))
    }
}

struct WiFiFrameParser {
    private var bytes = Data()

    mutating func append(_ data: Data) throws -> [WiFiFrame] {
        bytes.append(data)
        var frames: [WiFiFrame] = []
        while bytes.count >= WiFiFrameCodec.headerLength {
            guard bytes.prefix(4) == Data("IAW1".utf8), u16(6) == WiFiFrameCodec.headerLength else {
                throw DeviceError(message: "bad Wi-Fi frame header")
            }
            let count = Int(u32(8))
            guard count <= WiFiFrameCodec.maximumPayload else { throw DeviceError(message: "Wi-Fi frame too large") }
            let total = WiFiFrameCodec.headerLength + count
            guard bytes.count >= total else { break }
            guard let type = WiFiMessage(rawValue: bytes[4]) else { throw DeviceError(message: "unknown Wi-Fi message") }
            frames.append(WiFiFrame(type: type, flags: bytes[5], sequence: u32(12), boardMilliseconds: u32(16),
                                    payload: bytes.subdata(in: WiFiFrameCodec.headerLength..<total)))
            // Data.removeFirst keeps a sliced collection's non-zero startIndex; normalise so the next
            // frame can keep using protocol offsets 0...19 safely.
            bytes = Data(bytes.dropFirst(total))
        }
        return frames
    }

    private func u16(_ i: Int) -> Int { Int(bytes[i]) | Int(bytes[i + 1]) << 8 }
    private func u32(_ i: Int) -> UInt32 {
        UInt32(bytes[i]) | UInt32(bytes[i + 1]) << 8 | UInt32(bytes[i + 2]) << 16 | UInt32(bytes[i + 3]) << 24
    }
}

