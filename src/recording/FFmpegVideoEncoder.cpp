#include "FFmpegVideoEncoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <utility>

namespace streamforge {
namespace {

std::string ffmpegError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(code, buffer, sizeof(buffer)) < 0)
        return "Unbekannter FFmpeg-Fehler (" + std::to_string(code) + ")";
    return buffer;
}

bool codecId(VideoCodec codec, AVCodecID& id) {
    switch (codec) {
    case VideoCodec::H264: id = AV_CODEC_ID_H264; return true;
    case VideoCodec::HEVC: id = AV_CODEC_ID_HEVC; return true;
    case VideoCodec::AV1: id = AV_CODEC_ID_AV1; return true;
    }
    return false;
}

bool inputPixelFormat(PixelFormat format, AVPixelFormat& result) {
    switch (format) {
    case PixelFormat::BGRA8: result = AV_PIX_FMT_BGRA; return true;
    case PixelFormat::NV12: result = AV_PIX_FMT_NV12; return true;
    case PixelFormat::YUV420P: result = AV_PIX_FMT_YUV420P; return true;
    case PixelFormat::Unknown: return false;
    }
    return false;
}

} // namespace

struct FFmpegVideoEncoder::Impl final {
    enum class State : std::uint8_t { Empty, Ready, Flushing, Finished, Failed };

    VideoEncoderConfig config;
    EncodedPacketCallback callback;
    std::int32_t streamIndex{-1};
    AVCodecContext* codec{nullptr};
    SwsContext* scaler{nullptr};
    State state{State::Empty};
    std::int64_t nextFallbackPts{0};
    std::int64_t lastInputPts{AV_NOPTS_VALUE};
    std::int64_t timestampOriginNs{kNoTimestamp};
    bool hasTimestampOrigin{false};

    Impl(VideoEncoderConfig c, EncodedPacketCallback cb, std::int32_t index)
        : config(std::move(c)), callback(std::move(cb)), streamIndex(index) {}

    void release() noexcept {
        sws_freeContext(scaler);
        scaler = nullptr;
        avcodec_free_context(&codec);
    }

    bool receivePackets(std::string& error, int& produced, bool& eof) {
        produced = 0;
        eof = false;
        AVPacket* packet = av_packet_alloc();
        if (!packet) {
            error = "FFmpeg konnte keinen Video-AVPacket reservieren.";
            return false;
        }

        for (;;) {
            const int result = avcodec_receive_packet(codec, packet);
            if (result == AVERROR(EAGAIN)) break;
            if (result == AVERROR_EOF) { eof = true; break; }
            if (result < 0) {
                error = "Videoencoder-Paket konnte nicht gelesen werden: " +
                        ffmpegError(result);
                av_packet_free(&packet);
                return false;
            }
            if (packet->size <= 0 || !packet->data) {
                error = "Videoencoder lieferte ein leeres Paket.";
                av_packet_free(&packet);
                return false;
            }

            EncodedPacket encoded;
            encoded.mediaType = EncodedMediaType::Video;
            encoded.streamIndex = streamIndex;
            encoded.pts = packet->pts;
            encoded.dts = packet->dts;
            encoded.duration = std::max<std::int64_t>(packet->duration, 0);
            encoded.timebase = {codec->time_base.num, codec->time_base.den};
            encoded.keyFrame = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            encoded.data = std::make_shared<const std::vector<std::uint8_t>>(
                packet->data, packet->data + packet->size);

            av_packet_unref(packet);
            ++produced;
            try {
                if (!callback(std::move(encoded), error)) {
                    if (error.empty()) error = "RecordingPipeline hat Videopaket abgelehnt.";
                    av_packet_free(&packet);
                    return false;
                }
            } catch (const std::exception& ex) {
                error = std::string("Video-Packet-Callback fehlgeschlagen: ") + ex.what();
                av_packet_free(&packet);
                return false;
            } catch (...) {
                error = "Unbekannte Ausnahme im Video-Packet-Callback.";
                av_packet_free(&packet);
                return false;
            }
        }

        av_packet_free(&packet);
        return true;
    }

    bool sendFrame(AVFrame* frame, std::string& error) {
        for (int attempt = 0; attempt < 8; ++attempt) {
            const int result = avcodec_send_frame(codec, frame);
            if (result == 0) {
                if (frame) nextFallbackPts = std::max(nextFallbackPts, frame->pts + 1);
                av_frame_free(&frame);
                int produced = 0;
                bool eof = false;
                return receivePackets(error, produced, eof);
            }
            if (result != AVERROR(EAGAIN)) {
                error = "Videoframe konnte nicht an FFmpeg übergeben werden: " +
                        ffmpegError(result);
                av_frame_free(&frame);
                return false;
            }

            int produced = 0;
            bool eof = false;
            if (!receivePackets(error, produced, eof)) {
                av_frame_free(&frame);
                return false;
            }
            if (produced == 0) {
                error = "Videoencoder meldet EAGAIN ohne ausstehende Pakete.";
                av_frame_free(&frame);
                return false;
            }
        }

        error = "Videoencoder erreichte beim Senden keine Fortschritte.";
        av_frame_free(&frame);
        return false;
    }
};

FFmpegVideoEncoder::FFmpegVideoEncoder(
    VideoEncoderConfig config, EncodedPacketCallback callback, std::int32_t streamIndex)
    : m_impl(std::make_unique<Impl>(
          std::move(config), std::move(callback), streamIndex)) {}

FFmpegVideoEncoder::~FFmpegVideoEncoder() {
    shutdown();
}

bool FFmpegVideoEncoder::initialize(std::string& error) {
    const auto& config = m_impl->config;
    if (m_impl->state != Impl::State::Empty) {
        error = "Videoencoder wurde bereits initialisiert.";
        return false;
    }
    if (!m_impl->callback || m_impl->streamIndex < 0 ||
        config.width <= 0 || config.height <= 0 ||
        (config.width % 2) != 0 || (config.height % 2) != 0 ||
        config.fpsNumerator <= 0 || config.fpsDenominator <= 0 ||
        config.bitrate <= 0 || config.maxBitrate < config.bitrate ||
        config.keyframeInterval <= 0 ||
        config.rateControl == VideoRateControl::CQP) {
        error = "Ungültige Videencoder-Konfiguration.";
        m_impl->state = Impl::State::Failed;
        return false;
    }
    if (config.backend != VideoEncoderBackend::Software &&
        config.backend != VideoEncoderBackend::NVENC) {
        error = "Dieser FFmpeg-Adapter unterstützt aktuell nur Software und NVIDIA NVENC.";
        m_impl->state = Impl::State::Failed;
        return false;
    }

    AVCodecID id = AV_CODEC_ID_NONE;
    if (!codecId(config.codec, id)) {
        error = "Nicht unterstützter Video-Codec.";
        m_impl->state = Impl::State::Failed;
        return false;
    }

    const AVCodec* encoder = nullptr;
    if (config.backend == VideoEncoderBackend::NVENC) {
        const char* encoderName = nullptr;
        switch (config.codec) {
        case VideoCodec::H264: encoderName = "h264_nvenc"; break;
        case VideoCodec::HEVC: encoderName = "hevc_nvenc"; break;
        case VideoCodec::AV1: encoderName = "av1_nvenc"; break;
        }
        encoder = encoderName ? avcodec_find_encoder_by_name(encoderName) : nullptr;
        if (!encoder) {
            error = "Der angeforderte NVIDIA-NVENC-Encoder ist in dieser FFmpeg-Build nicht verfügbar.";
            m_impl->state = Impl::State::Failed;
            return false;
        }
    } else {
        if (config.codec == VideoCodec::H264) {
            encoder = avcodec_find_encoder_by_name("libx264");
        }
        if (!encoder) encoder = avcodec_find_encoder(id);
        if (!encoder) {
            error = "FFmpeg-Softwareencoder für den angeforderten Video-Codec fehlt.";
            m_impl->state = Impl::State::Failed;
            return false;
        }
    }

    m_impl->codec = avcodec_alloc_context3(encoder);
    if (!m_impl->codec) {
        error = "FFmpeg-Video-CodecContext konnte nicht reserviert werden.";
        m_impl->state = Impl::State::Failed;
        return false;
    }

    auto* context = m_impl->codec;
    context->codec_type = AVMEDIA_TYPE_VIDEO;
    context->codec_id = id;
    context->width = config.width;
    context->height = config.height;
    context->pix_fmt = AV_PIX_FMT_YUV420P;
    context->time_base = AVRational{config.fpsDenominator, config.fpsNumerator};
    context->framerate = AVRational{config.fpsNumerator, config.fpsDenominator};
    context->bit_rate = config.bitrate;
    context->gop_size = config.keyframeInterval;
    context->max_b_frames =
        (!config.lowLatency && config.allowBFrames) ? 2 : 0;
    context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    context->thread_count = 0;

    if (config.rateControl == VideoRateControl::CBR) {
        context->rc_min_rate = config.bitrate;
        context->rc_max_rate = config.bitrate;
    } else {
        context->rc_min_rate = 0;
        context->rc_max_rate = config.maxBitrate;
    }
    const std::int64_t bufferSize = std::min<std::int64_t>(
        static_cast<std::int64_t>(config.maxBitrate) * 2,
        std::numeric_limits<int>::max());
    context->rc_buffer_size = static_cast<int>(bufferSize);

    AVDictionary* options = nullptr;
    if (std::string(encoder->name) == "libx264") {
        av_dict_set(&options, "preset", "veryfast", 0);
        if (config.lowLatency) {
            av_dict_set(&options, "tune", "zerolatency", 0);
        }
        if (config.rateControl == VideoRateControl::CBR) {
            av_dict_set(&options, "x264-params", "nal-hrd=cbr:force-cfr=1", 0);
        }
    }
    if (config.backend == VideoEncoderBackend::NVENC) {
        // NVENC receives the software-converted YUV frame and performs the
        // actual compression on the NVIDIA GPU. Direct D3D11 texture import
        // is deliberately a separate future optimization.
        av_dict_set(&options, "preset", "p4", 0);
        if (config.lowLatency) {
            av_dict_set(&options, "tune", "ll", 0);
            av_dict_set(&options, "zerolatency", "1", 0);
            av_dict_set(&options, "rc-lookahead", "0", 0);
        }
        av_dict_set(&options, "rc",
                    config.rateControl == VideoRateControl::CBR ? "cbr" : "vbr", 0);
    }

    const int openResult = avcodec_open2(context, encoder, &options);
    av_dict_free(&options);
    if (openResult < 0) {
        error = "FFmpeg-Videoencoder konnte nicht geöffnet werden: " +
                ffmpegError(openResult);
        m_impl->release();
        m_impl->state = Impl::State::Failed;
        return false;
    }

    if (context->pix_fmt != AV_PIX_FMT_YUV420P) {
        error = "Videoencoder hat das angeforderte YUV420P-Format nicht übernommen.";
        m_impl->release();
        m_impl->state = Impl::State::Failed;
        return false;
    }

    m_impl->state = Impl::State::Ready;
    return true;
}

bool FFmpegVideoEncoder::encode(const VideoFrameInput& input, std::string& error) {
    if (m_impl->state != Impl::State::Ready) {
        error = "Videoencoder ist nicht aufnahmebereit.";
        return false;
    }
    if (!input.isValid()) {
        error = "Ungültiger Video-Frame.";
        m_impl->state = Impl::State::Failed;
        return false;
    }
    const bool hasCpuPlanes = input.data[0] &&
        (input.pixelFormat == PixelFormat::BGRA8 ||
         (input.pixelFormat == PixelFormat::NV12 && input.data[1]) ||
         (input.pixelFormat == PixelFormat::YUV420P && input.data[1] && input.data[2]));
    if (!hasCpuPlanes) {
        error = "FFmpeg-Softwareencoder benötigt CPU-Bilddaten; native GPU-Frames werden hier nicht unterstützt.";
        m_impl->state = Impl::State::Failed;
        return false;
    }

    AVPixelFormat sourceFormat = AV_PIX_FMT_NONE;
    if (!inputPixelFormat(input.pixelFormat, sourceFormat)) {
        error = "Eingabe-Pixelformat wird nicht unterstützt.";
        m_impl->state = Impl::State::Failed;
        return false;
    }

    m_impl->scaler = sws_getCachedContext(
        m_impl->scaler,
        input.width, input.height, sourceFormat,
        m_impl->codec->width, m_impl->codec->height, m_impl->codec->pix_fmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_impl->scaler) {
        error = "FFmpeg-Videoscaler konnte nicht erstellt werden.";
        m_impl->state = Impl::State::Failed;
        return false;
    }

    std::int64_t pts = m_impl->nextFallbackPts;
    if (input.timestampNs != kNoTimestamp) {
        if (!m_impl->hasTimestampOrigin) {
            m_impl->timestampOriginNs =
                m_impl->config.timestampOriginNs != kNoTimestamp
                    ? m_impl->config.timestampOriginNs : input.timestampNs;
            m_impl->hasTimestampOrigin = true;
        }
        if (input.timestampNs < m_impl->timestampOriginNs) {
            error = "Videoframe liegt vor dem gemeinsamen Session-Zeitursprung.";
            m_impl->state = Impl::State::Failed;
            return false;
        }
        pts = av_rescale_q(
            input.timestampNs - m_impl->timestampOriginNs,
            AVRational{1, 1'000'000'000}, m_impl->codec->time_base);
    } else if (input.pts != kNoTimestamp) {
        pts = av_rescale_q(input.pts,
                           AVRational{input.timebase.numerator, input.timebase.denominator},
                           m_impl->codec->time_base);
    }
    if (m_impl->lastInputPts != AV_NOPTS_VALUE && pts <= m_impl->lastInputPts) {
        error = "Video-PTS müssen streng monoton ansteigen.";
        m_impl->state = Impl::State::Failed;
        return false;
    }

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        error = "Video-AVFrame konnte nicht reserviert werden.";
        m_impl->state = Impl::State::Failed;
        return false;
    }
    frame->format = m_impl->codec->pix_fmt;
    frame->width = m_impl->codec->width;
    frame->height = m_impl->codec->height;
    frame->pts = pts;

    int result = av_frame_get_buffer(frame, 32);
    if (result < 0) {
        error = "Video-Frame-Puffer konnte nicht reserviert werden: " +
                ffmpegError(result);
        av_frame_free(&frame);
        m_impl->state = Impl::State::Failed;
        return false;
    }

    const int rows = sws_scale(
        m_impl->scaler, input.data.data(), input.linesize.data(),
        0, input.height, frame->data, frame->linesize);
    if (rows != m_impl->codec->height) {
        error = "Videoscaler hat nicht alle Bildzeilen konvertiert.";
        av_frame_free(&frame);
        m_impl->state = Impl::State::Failed;
        return false;
    }

    m_impl->lastInputPts = pts;
    if (!m_impl->sendFrame(frame, error)) {
        m_impl->state = Impl::State::Failed;
        return false;
    }
    return true;
}

bool FFmpegVideoEncoder::flush(std::string& error) {
    if (m_impl->state != Impl::State::Ready) {
        error = "Videoencoder kann in diesem Zustand nicht geflusht werden.";
        return false;
    }
    m_impl->state = Impl::State::Flushing;

    bool sentNullFrame = false;
    for (int attempt = 0; attempt < 8 && !sentNullFrame; ++attempt) {
        const int result = avcodec_send_frame(m_impl->codec, nullptr);
        if (result == 0 || result == AVERROR_EOF) {
            sentNullFrame = true;
            break;
        }
        if (result != AVERROR(EAGAIN)) {
            error = "Videoencoder konnte Flush-Frame nicht senden: " +
                    ffmpegError(result);
            m_impl->state = Impl::State::Failed;
            return false;
        }
        int produced = 0;
        bool eof = false;
        if (!m_impl->receivePackets(error, produced, eof) || produced == 0) {
            if (error.empty()) error = "Videoencoder-Flush machte keinen Fortschritt.";
            m_impl->state = Impl::State::Failed;
            return false;
        }
    }
    if (!sentNullFrame) {
        error = "Videoencoder-Flush erreichte die Fortschrittsgrenze.";
        m_impl->state = Impl::State::Failed;
        return false;
    }

    for (int attempt = 0; attempt < 128; ++attempt) {
        int produced = 0;
        bool eof = false;
        if (!m_impl->receivePackets(error, produced, eof)) {
            m_impl->state = Impl::State::Failed;
            return false;
        }
        if (eof) {
            m_impl->state = Impl::State::Finished;
            return true;
        }
        if (produced == 0) {
            error = "Videoencoder meldete EAGAIN statt EOF nach dem Flush.";
            m_impl->state = Impl::State::Failed;
            return false;
        }
    }

    error = "Videoencoder-Flush überschritt die Paketgrenze.";
    m_impl->state = Impl::State::Failed;
    return false;
}

bool FFmpegVideoEncoder::getStreamDescriptor(
    EncodedStreamDescriptor& descriptor, std::string& error) const {
    if (!m_impl->codec ||
        (m_impl->state != Impl::State::Ready &&
         m_impl->state != Impl::State::Flushing &&
         m_impl->state != Impl::State::Finished)) {
        error = "Video-Stream-Deskriptor ist noch nicht verfügbar.";
        return false;
    }

    descriptor = {};
    descriptor.streamIndex = m_impl->streamIndex;
    descriptor.mediaType = EncodedMediaType::Video;
    switch (m_impl->config.codec) {
    case VideoCodec::H264: descriptor.codec = EncodedCodec::H264; break;
    case VideoCodec::HEVC: descriptor.codec = EncodedCodec::HEVC; break;
    case VideoCodec::AV1: descriptor.codec = EncodedCodec::AV1; break;
    }
    descriptor.timebase = {
        m_impl->codec->time_base.num, m_impl->codec->time_base.den};
    descriptor.width = m_impl->codec->width;
    descriptor.height = m_impl->codec->height;
    descriptor.pixelFormat = PixelFormat::YUV420P;
    descriptor.frameRateNumerator = m_impl->config.fpsNumerator;
    descriptor.frameRateDenominator = m_impl->config.fpsDenominator;
    descriptor.bitrate = m_impl->config.bitrate;
    descriptor.profile = m_impl->codec->profile >= 0
        ? m_impl->codec->profile : -1;
    descriptor.level = m_impl->codec->level >= 0
        ? m_impl->codec->level : -1;
    if (m_impl->codec->extradata && m_impl->codec->extradata_size > 0) {
        descriptor.codecExtradata.assign(
            m_impl->codec->extradata,
            m_impl->codec->extradata + m_impl->codec->extradata_size);
    }
    if (!descriptor.isValid()) {
        error = "FFmpeg lieferte einen ungültigen Video-Stream-Deskriptor.";
        return false;
    }
    return true;
}

void FFmpegVideoEncoder::shutdown() noexcept {
    if (!m_impl) return;
    m_impl->release();
    if (m_impl->state != Impl::State::Finished &&
        m_impl->state != Impl::State::Failed) {
        m_impl->state = Impl::State::Empty;
    }
}

} // namespace streamforge
