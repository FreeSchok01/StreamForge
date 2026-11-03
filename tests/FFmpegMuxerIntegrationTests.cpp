#include "FFmpegAudioEncoder.h"
#include "FFmpegMediaMuxer.h"

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

} // namespace

int main() {
    std::string error;
    EncodedStreamDescriptor descriptor;
    std::vector<EncodedPacket> packets;
    check(makeAacPackets(descriptor, packets, error),
          "FFmpeg audio encoder produces AAC: " + error);

    const auto path = std::filesystem::temp_directory_path() /
                      "streamforge_ffmpeg_muxer_integration.mkv";
    std::error_code fsError;
    std::filesystem::remove(path, fsError);

    FFmpegMediaMuxer muxer;
    RecordingOutputConfig output;
    output.outputPath = path.string();
    output.container = RecordingContainer::Matroska;
    output.overwriteExisting = true;

    check(muxer.configure(output, {descriptor}, error),
          "muxer configure: " + error);
    check(muxer.open(error), "muxer open: " + error);

    for (const auto& packet : packets) {
        check(muxer.writePacket(packet, error),
              "muxer write packet: " + error);
    }
    check(muxer.finalize(error), "muxer finalize: " + error);

    AVFormatContext* input = nullptr;
    const int openResult = avformat_open_input(
        &input, path.string().c_str(), nullptr, nullptr);
    check(openResult >= 0, "reopen Matroska output");
    const int infoResult = avformat_find_stream_info(input, nullptr);
    check(infoResult >= 0, "read Matroska stream information");
    check(input->nb_streams == 1, "exactly one stream in output");
    check(input->streams[0]->codecpar->codec_id == AV_CODEC_ID_AAC,
          "output stream is AAC");
    avformat_close_input(&input);

    std::filesystem::remove(path, fsError);
    std::cout << "FFmpeg AAC encoder and Matroska muxer integration test passed.\n";
    return 0;
}
