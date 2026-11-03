#include "FFmpegMediaMuxer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
}

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace streamforge;

namespace {

bool drainEncoder(AVCodecContext* context, std::vector<EncodedPacket>& output,
                  std::string& error) {
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        error = "AVPacket allocation failed";
        return false;
    }

    bool ok = true;
    for (;;) {
        const int result = avcodec_receive_packet(context, packet);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
        if (result < 0) {
            error = "avcodec_receive_packet: " + std::to_string(result);
            ok = false;
            break;
        }

        EncodedPacket encoded;
        encoded.mediaType = EncodedMediaType::Audio;
        encoded.streamIndex = 0;
        encoded.pts = packet->pts;
        encoded.dts = packet->dts;
        encoded.duration = packet->duration > 0
            ? packet->duration : context->frame_size;
        encoded.timebase = {context->time_base.num, context->time_base.den};
        encoded.keyFrame = (packet->flags & AV_PKT_FLAG_KEY) != 0;
        encoded.data = std::make_shared<const std::vector<std::uint8_t>>(
            packet->data, packet->data + packet->size);
        output.push_back(std::move(encoded));
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    return ok;
}

bool makeAacPackets(EncodedStreamDescriptor& descriptor,
                    std::vector<EncodedPacket>& packets,
                    std::string& error) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        error = "FFmpeg AAC encoder is unavailable";
        return false;
    }

    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (!context) {
        error = "AVCodecContext allocation failed";
        return false;
    }

    context->sample_rate = 48000;
    context->bit_rate = 128000;
    context->time_base = AVRational{1, context->sample_rate};
    context->sample_fmt = AV_SAMPLE_FMT_FLTP;
    context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_channel_layout_default(&context->ch_layout, 2);

    int result = avcodec_open2(context, codec, nullptr);
    if (result < 0) {
        error = "avcodec_open2: " + std::to_string(result);
        avcodec_free_context(&context);
        return false;
    }

    descriptor = {};
    descriptor.streamIndex = 0;
    descriptor.mediaType = EncodedMediaType::Audio;
    descriptor.codec = EncodedCodec::AAC;
    descriptor.timebase = {context->time_base.num, context->time_base.den};
    descriptor.sampleRate = context->sample_rate;
    descriptor.channels = context->ch_layout.nb_channels;
    descriptor.channelLayout = AudioChannelLayout::Stereo;
    descriptor.sampleFormat = AudioSampleFormat::Float32Planar;
    descriptor.bitrate = 128000;
    if (context->extradata && context->extradata_size > 0) {
        descriptor.codecExtradata.assign(
            context->extradata,
            context->extradata + context->extradata_size);
    }

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        error = "AVFrame allocation failed";
        avcodec_free_context(&context);
        return false;
    }
    frame->format = context->sample_fmt;
    frame->sample_rate = context->sample_rate;
    frame->nb_samples = context->frame_size;
    frame->pts = 0;
    result = av_channel_layout_copy(&frame->ch_layout, &context->ch_layout);
    if (result >= 0) result = av_frame_get_buffer(frame, 0);
    if (result >= 0) {
        result = av_samples_set_silence(
            frame->extended_data, 0, frame->nb_samples,
            frame->ch_layout.nb_channels, context->sample_fmt);
    }
    if (result < 0) {
        error = "AAC frame preparation: " + std::to_string(result);
        av_frame_free(&frame);
        avcodec_free_context(&context);
        return false;
    }

    result = avcodec_send_frame(context, frame);
    if (result < 0) {
        error = "avcodec_send_frame: " + std::to_string(result);
        av_frame_free(&frame);
        avcodec_free_context(&context);
        return false;
    }
    av_frame_free(&frame);

    if (!drainEncoder(context, packets, error)) {
        avcodec_free_context(&context);
        return false;
    }

    result = avcodec_send_frame(context, nullptr);
    if (result < 0) {
        error = "AAC flush send: " + std::to_string(result);
        avcodec_free_context(&context);
        return false;
    }

    if (!drainEncoder(context, packets, error)) {
        avcodec_free_context(&context);
        return false;
    }

    avcodec_free_context(&context);
    if (packets.empty()) {
        error = "AAC encoder produced no packets";
        return false;
    }
    return descriptor.isValid();
}

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    std::string error;
    EncodedStreamDescriptor descriptor;
    std::vector<EncodedPacket> packets;
    check(makeAacPackets(descriptor, packets, error),
          "create AAC test packets: " + error);

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
    int result = avformat_open_input(&input, path.string().c_str(), nullptr, nullptr);
    check(result >= 0, "reopen Matroska output");
    result = avformat_find_stream_info(input, nullptr);
    check(result >= 0, "read Matroska stream information");
    check(input->nb_streams == 1, "exactly one stream in output");
    check(input->streams[0]->codecpar->codec_id == AV_CODEC_ID_AAC,
          "output stream is AAC");
    avformat_close_input(&input);

    std::filesystem::remove(path, fsError);
    std::cout << "FFmpeg muxer integration test passed.\n";
    return 0;
}
