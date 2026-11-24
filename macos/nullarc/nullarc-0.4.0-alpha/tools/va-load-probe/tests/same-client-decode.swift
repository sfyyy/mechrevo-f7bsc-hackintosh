import AVFoundation
import Darwin
import VideoToolbox

struct Failure: Error, CustomStringConvertible {
    let description: String
    init(_ message: String) { description = message }
}

struct Fixture {
    let name: String
    let width: Int
    let height: Int
    let samples: [CMSampleBuffer]
    let reference: Data
    var frameBytes: Int { width * height * 3 / 2 }

    init(movie: String, referencePath: String, name: String) async throws {
        self.name = name
        let asset = AVURLAsset(url: URL(fileURLWithPath: movie))
        let tracks = try await asset.loadTracks(withMediaType: .video)
        guard tracks.count == 1 else { throw Failure("\(name) expected one video track") }
        let reader = try AVAssetReader(asset: asset)
        let track = AVAssetReaderTrackOutput(track: tracks[0], outputSettings: nil)
        track.alwaysCopiesSampleData = false
        guard reader.canAdd(track) else { throw Failure("\(name) cannot read compressed track") }
        reader.add(track)
        guard reader.startReading() else { throw Failure("\(name) cannot start reading: \(String(describing: reader.error))") }
        var frames: [CMSampleBuffer] = []
        while let sample = track.copyNextSampleBuffer() {
            if CMSampleBufferGetNumSamples(sample) == 0 { continue }
            let index = frames.count
            guard CMSampleBufferGetNumSamples(sample) == 1, let format = CMSampleBufferGetFormatDescription(sample),
                  CMFormatDescriptionGetMediaSubType(format) == kCMVideoCodecType_H264,
                  let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: true) as? [[String: Any]],
                  attachments.count == 1 else {
                throw Failure("\(name) invalid compressed frame \(index)")
            }
            let nonSync = attachments[0][kCMSampleAttachmentKey_NotSync as String] as? Bool ?? false
            guard nonSync == (index != 0 && index != 12) else { throw Failure("\(name) unexpected keyframe flags at \(index)") }
            frames.append(sample)
        }
        guard reader.status == .completed, frames.count == 14,
              let format = CMSampleBufferGetFormatDescription(frames[0]) else { throw Failure("\(name) incomplete fixture") }
        let size = CMVideoFormatDescriptionGetDimensions(format)
        width = Int(size.width)
        height = Int(size.height)
        guard "\(width)x\(height)" == name else { throw Failure("\(name) unexpected dimensions \(width)x\(height)") }
        samples = frames
        reference = try Data(contentsOf: URL(fileURLWithPath: referencePath), options: .mappedIfSafe)
        guard reference.count == frameBytes * frames.count else { throw Failure("\(name) invalid reference size") }
    }

    func compare(_ image: CVPixelBuffer, frame: Int) -> Bool {
        guard CVPixelBufferGetWidth(image) == width, CVPixelBufferGetHeight(image) == height,
              CVPixelBufferGetPixelFormatType(image) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
              CVPixelBufferGetPlaneCount(image) == 2,
              CVPixelBufferLockBaseAddress(image, .readOnly) == kCVReturnSuccess else { return false }
        let match = reference.withUnsafeBytes { (bytes: UnsafeRawBufferPointer) -> Bool in
            var offset = frame * frameBytes
            for plane in 0..<2 {
                let rows = plane == 0 ? height : height / 2
                let stride = CVPixelBufferGetBytesPerRowOfPlane(image, plane)
                guard let base = CVPixelBufferGetBaseAddressOfPlane(image, plane), stride >= width,
                      CVPixelBufferGetWidthOfPlane(image, plane) >= (plane == 0 ? width : width / 2),
                      CVPixelBufferGetHeightOfPlane(image, plane) >= rows else { return false }
                for row in 0..<rows {
                    if memcmp(bytes.baseAddress! + offset, base + row * stride, width) != 0 { return false }
                    offset += width
                }
            }
            return true
        }
        return CVPixelBufferUnlockBaseAddress(image, .readOnly) == kCVReturnSuccess && match
    }
}

final class Decoder {
    let fixture: Fixture
    var session: VTDecompressionSession?

    init(_ fixture: Fixture) throws {
        self.fixture = fixture
        let spec = [kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder as String: true] as CFDictionary
        let attrs = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange] as CFDictionary
        let result = VTDecompressionSessionCreate(allocator: nil, formatDescription: CMSampleBufferGetFormatDescription(fixture.samples[0])!,
                                                  decoderSpecification: spec, imageBufferAttributes: attrs,
                                                  outputCallback: nil, decompressionSessionOut: &session)
        guard result == noErr, session != nil else { throw Failure("\(fixture.name) create status=\(result)") }
    }

    func close() {
        if let session { VTDecompressionSessionInvalidate(session) }
        session = nil
    }
    deinit { close() }

    func decode(_ index: Int) throws {
        guard let session else { throw Failure("attempted to decode on a closed session") }
        var callbacks = 0
        var matched = false
        var callbackStatus: OSStatus?
        var callbackDropped = false
        var flags = VTDecodeInfoFlags()
        // Without asynchronous or temporal flags the callback completes before the function returns.
        let status = VTDecompressionSessionDecodeFrame(session, sampleBuffer: fixture.samples[index],
                                                       flags: [], infoFlagsOut: &flags) { result, info, image, _, _ in
            callbacks += 1
            callbackStatus = result
            callbackDropped = info.contains(.frameDropped)
            if result == noErr, let image { matched = self.fixture.compare(image, frame: index) }
        }
        guard status == noErr, callbackStatus == noErr, callbacks == 1, !flags.contains(.frameDropped),
              !callbackDropped, matched else {
            throw Failure("\(fixture.name) frame=\(index) submit=\(status) callbackStatus=\(String(describing: callbackStatus)) callbacks=\(callbacks) dropped=\(callbackDropped || flags.contains(.frameDropped)) pixelsMatch=\(matched)")
        }
        if index == 0 {
            var pointer: UnsafeRawPointer?
            let query = VTSessionCopyProperty(session, key: kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
                                              allocator: nil, valueOut: &pointer)
            let hardware = pointer.map { Unmanaged<CFTypeRef>.fromOpaque($0).takeRetainedValue() }
            guard query == noErr, (hardware as? Bool) == true else {
                throw Failure("\(fixture.name) hardware-property status=\(query) value=\(String(describing: hardware))")
            }
        }
    }
}

func runPair(closing: Fixture, surviving: Fixture) throws {
    let first = try Decoder(closing)
    defer { first.close() }
    let second = try Decoder(surviving)
    defer { second.close() }
    for frame in 0..<6 {
        try first.decode(frame)
        try second.decode(frame)
    }
    first.close()
    for frame in 6..<12 { try second.decode(frame) }
    print("closed=\(closing.name) checked=6 survivor=\(surviving.name) checked=12 post_close_p_frames=6")
}

@main struct Test {
    static func main() async {
        do {
            let args = CommandLine.arguments
            guard args.count == 6, ["--check-inputs", "--hardware"].contains(args[1]) else {
                throw Failure("usage: same-client-decode --check-inputs|--hardware 1080.mp4 1080.nv12 480.mp4 480.nv12")
            }
            let hd = try await Fixture(movie: args[2], referencePath: args[3], name: "1920x1080")
            let sd = try await Fixture(movie: args[4], referencePath: args[5], name: "864x480")
            print("pid=\(getpid()) inputs_checked=2 frames_per_input=14 mode=\(args[1])")
            if args[1] == "--hardware" {
                try runPair(closing: hd, surviving: sd)
                try runPair(closing: sd, surviving: hd)
            }
        } catch {
            FileHandle.standardError.write(Data("\(error)\n".utf8))
            exit(1)
        }
    }
}
