#include "FFmpegRecordingFactories.h"
#include "RecordingPipeline.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace streamforge;

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    const auto path = std::filesystem::temp_directory_path() /
                      "streamforge_recording_pipeline_integration.mkv";
    std::error_code fsError;
    std::filesystem::remove(path, fsError);

    RecordingPipelineConfig config;
    config.audio.codec = AudioCodec::AAC;
    config.audio.inputSampleRate = 48000;
    config.audio.inputChannels = 2;
    config.audio.outputSampleRate = 48000;
    config.audio.outputChannels = 2;
    config.audio.bitrate = 128000;

    config.video.codec = VideoCodec::H264;
    config.video.backend = VideoEncoderBackend::Software;
    config.video.rateControl = VideoRateControl::VBR;
    config.video.width = 320;
    config.video.height = 240;
    config.video.fpsNumerator = 30;
    config.video.fpsDenominator = 1;
    config.video.bitrate = 500000;
    config.video.maxBitrate = 700000;
    config.video.keyframeInterval = 30;
    config.video.lowLatency = true;
    config.video.allowBFrames = false;

    config.timestampOriginNs = 1'000'000'000;
    config.output.outputPath = path.string();
    config.output.container = RecordingContainer::Matroska;
    config.output.overwriteExisting = true;
    config.packetQueue.maxPackets = 512;
    config.packetQueue.maxBytes = 32U * 1024U * 1024U;

    RecordingPipeline pipeline(makeFFmpegRecordingPipelineFactories());
    std::string error;
    check(pipeline.start(config, error),
          "real RecordingPipeline start: " + error);

    constexpr int audioFrameCount = 1024;
    constexpr std::int64_t audioFrameDurationNs = 21'333'333;
    for (int index = 0; index < 8; ++index) {
        AudioFrame audio;
        audio.sampleRate = 48000;
        audio.channels = 2;
        audio.frameCount = audioFrameCount;
        audio.timestampNs = config.timestampOriginNs +
            static_cast<std::int64_t>(index) * audioFrameDurationNs;
        audio.samples.assign(
            static_cast<std::size_t>(audioFrameCount) * 2U, 0.0F);
        check(pipeline.submitAudioFrame(audio, error),
              "submit real audio frame: " + error);
    }

    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(config.video.width) *
        static_cast<std::size_t>(config.video.height) * 4U);
    for (int index = 0; index < 8; ++index) {
        std::fill(pixels.begin(), pixels.end(),
                  static_cast<std::uint8_t>(index * 24));
        VideoFrameInput video;
        video.data[0] = pixels.data();
        video.linesize[0] = config.video.width * 4;
        video.width = config.video.width;
        video.height = config.video.height;
        video.pixelFormat = PixelFormat::BGRA8;
        video.timestampNs = config.timestampOriginNs +
            static_cast<std::int64_t>(index) * 33'333'333;
        video.timebase = {1, 30};
        check(pipeline.submitVideoFrame(video, error),
              "submit real video frame: " + error);
    }

    check(pipeline.stop(RecordingStopMode::Graceful, error),
          "real RecordingPipeline graceful stop: " + error);
    check(pipeline.snapshot().state == RecordingState::Completed,
          "pipeline reached Completed");

    AVFormatContext* input = nullptr;
    const int openResult = avformat_open_input(
        &input, path.string().c_str(), nullptr, nullptr);
    check(openResult >= 0, "reopen recorded Matroska file");
    const int infoResult = avformat_find_stream_info(input, nullptr);
    check(infoResult >= 0, "read recorded Matroska stream information");
    check(input->nb_streams == 2, "recording contains audio and video streams");

    bool foundAac = false;
    bool foundH264 = false;
    for (unsigned int index = 0; index < input->nb_streams; ++index) {
        const AVCodecID id = input->streams[index]->codecpar->codec_id;
        foundAac = foundAac || id == AV_CODEC_ID_AAC;
        foundH264 = foundH264 || id == AV_CODEC_ID_H264;
    }
    check(foundAac, "recording contains AAC audio");
    check(foundH264, "recording contains H.264 video");
    std::vector<std::int64_t> lastDts(input->nb_streams, AV_NOPTS_VALUE);
    std::vector<int> packetCounts(input->nb_streams, 0);
    AVPacket* packet = av_packet_alloc();
    check(packet != nullptr, "allocate packet for mux validation");
    int readResult = 0;
    while ((readResult = av_read_frame(input, packet)) >= 0) {
        const auto streamIndex = static_cast<std::size_t>(packet->stream_index);
        check(streamIndex < lastDts.size(), "packet references a valid stream");
        if (packet->dts != AV_NOPTS_VALUE) {
            check(lastDts[streamIndex] == AV_NOPTS_VALUE ||
                      packet->dts >= lastDts[streamIndex],
                  "packet DTS is monotonic within each stream");
            lastDts[streamIndex] = packet->dts;
        }
        ++packetCounts[streamIndex];
        av_packet_unref(packet);
    }
    check(readResult == AVERROR_EOF, "read packets through end of recording");
    av_packet_free(&packet);
    check(packetCounts.size() == 2 && packetCounts[0] > 0 && packetCounts[1] > 0,
          "recording contains muxed packets for both streams");
    avformat_close_input(&input);

    std::filesystem::remove(path, fsError);
    std::cout << "Real RecordingPipeline AAC/H.264 Matroska integration passed.\n";
    return 0;
}
