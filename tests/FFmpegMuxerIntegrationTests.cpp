#include "FFmpegAudioEncoder.h"
#include "FFmpegMediaMuxer.h"
#include "FFmpegVideoEncoder.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace streamforge;

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

bool makeAacPackets(EncodedStreamDescriptor& descriptor,
                    std::vector<EncodedPacket>& packets,
                    std::string& error) {
    AudioEncoderConfig config;
    config.codec = AudioCodec::AAC;
    config.inputSampleRate = 48000;
    config.inputChannels = 2;
    config.outputSampleRate = 48000;
    config.outputChannels = 2;
    config.bitrate = 128000;

    EncodedPacketCallback callback = [&packets](
        EncodedPacket&& packet, std::string&) {
        packets.push_back(std::move(packet));
        return true;
    };

    FFmpegAudioEncoder encoder(config, std::move(callback), 0);
    if (!encoder.initialize(error)) return false;
    if (!encoder.getStreamDescriptor(descriptor, error)) {
        encoder.shutdown();
        return false;
    }

    constexpr int frameCount = 1024;
    constexpr std::int64_t frameDurationNs = 21'333'333;
    for (int index = 0; index < 8; ++index) {
        AudioFrame frame;
        frame.sampleRate = 48000;
        frame.channels = 2;
        frame.frameCount = frameCount;
        frame.timestampNs = static_cast<std::int64_t>(index) * frameDurationNs;
        frame.samples.assign(static_cast<std::size_t>(frameCount) * 2U, 0.0F);
        if (!encoder.encode(frame, error)) {
            encoder.shutdown();
            return false;
        }
    }

    const bool flushed = encoder.flush(error);
    encoder.shutdown();
    if (!flushed) return false;
    return !packets.empty() && descriptor.isValid();
}


bool makeH264Packets(EncodedStreamDescriptor& descriptor,
                     std::vector<EncodedPacket>& packets,
                     std::string& error) {
    VideoEncoderConfig config;
    config.codec = VideoCodec::H264;
    config.backend = VideoEncoderBackend::Software;
    config.rateControl = VideoRateControl::VBR;
    config.width = 320;
    config.height = 240;
    config.fpsNumerator = 30;
    config.fpsDenominator = 1;
    config.bitrate = 500000;
    config.maxBitrate = 700000;
    config.keyframeInterval = 30;
    config.lowLatency = true;
    config.allowBFrames = false;

    EncodedPacketCallback callback = [&packets](
        EncodedPacket&& packet, std::string&) {
        packets.push_back(std::move(packet));
        return true;
    };

    FFmpegVideoEncoder encoder(config, std::move(callback), 1);
    if (!encoder.initialize(error)) return false;
    if (!encoder.getStreamDescriptor(descriptor, error)) {
        encoder.shutdown();
        return false;
    }

    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(config.width) *
        static_cast<std::size_t>(config.height) * 4U);
    for (int index = 0; index < 8; ++index) {
        std::fill(pixels.begin(), pixels.end(),
                  static_cast<std::uint8_t>(index * 24));
        VideoFrameInput frame;
        frame.data[0] = pixels.data();
        frame.linesize[0] = config.width * 4;
        frame.width = config.width;
        frame.height = config.height;
        frame.pixelFormat = PixelFormat::BGRA8;
        frame.pts = index;
        frame.timebase = {1, 30};
        if (!encoder.encode(frame, error)) {
            encoder.shutdown();
            return false;
        }
    }

    const bool flushed = encoder.flush(error);
    encoder.shutdown();
    if (!flushed) return false;
    return !packets.empty() && descriptor.isValid();
}

} // namespace

int main() {
    std::string error;
    EncodedStreamDescriptor audioDescriptor;
    EncodedStreamDescriptor videoDescriptor;
    std::vector<EncodedPacket> audioPackets;
    std::vector<EncodedPacket> videoPackets;
    check(makeAacPackets(audioDescriptor, audioPackets, error),
          "FFmpeg audio encoder produces AAC: " + error);
    check(makeH264Packets(videoDescriptor, videoPackets, error),
          "FFmpeg video encoder produces H.264: " + error);

    const auto path = std::filesystem::temp_directory_path() /
                      "streamforge_ffmpeg_muxer_integration.mkv";
    std::error_code fsError;
    std::filesystem::remove(path, fsError);

    FFmpegMediaMuxer muxer;
    RecordingOutputConfig output;
    output.outputPath = path.string();
    output.container = RecordingContainer::Matroska;
    output.overwriteExisting = true;

    check(muxer.configure(output, {audioDescriptor, videoDescriptor}, error),
          "muxer configure: " + error);
    check(muxer.open(error), "muxer open: " + error);

    for (const auto& packet : audioPackets) {
        check(muxer.writePacket(packet, error),
              "muxer write AAC packet: " + error);
    }
    for (const auto& packet : videoPackets) {
        check(muxer.writePacket(packet, error),
              "muxer write H.264 packet: " + error);
    }
    check(muxer.finalize(error), "muxer finalize: " + error);

    AVFormatContext* input = nullptr;
    const int openResult = avformat_open_input(
        &input, path.string().c_str(), nullptr, nullptr);
    check(openResult >= 0, "reopen Matroska output");
    const int infoResult = avformat_find_stream_info(input, nullptr);
    check(infoResult >= 0, "read Matroska stream information");
    check(input->nb_streams == 2, "audio and video streams in output");
    bool foundAac = false;
    bool foundH264 = false;
    for (unsigned int index = 0; index < input->nb_streams; ++index) {
        const AVCodecID id = input->streams[index]->codecpar->codec_id;
        foundAac = foundAac || id == AV_CODEC_ID_AAC;
        foundH264 = foundH264 || id == AV_CODEC_ID_H264;
    }
    check(foundAac, "output contains AAC audio");
    check(foundH264, "output contains H.264 video");
    avformat_close_input(&input);

    std::filesystem::remove(path, fsError);
    std::cout << "FFmpeg AAC/H.264 and Matroska A/V integration test passed.\n";
    return 0;
}
