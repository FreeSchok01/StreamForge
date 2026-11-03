#include "FFmpegMediaMuxer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace streamforge {
namespace {

std::string ffmpegError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(code, buffer, sizeof(buffer)) < 0) {
        return "Unbekannter FFmpeg-Fehler (" + std::to_string(code) + ")";
    }
    return buffer;
}

bool codecId(EncodedCodec codec, AVCodecID& result) {
    switch (codec) {
    case EncodedCodec::H264: result = AV_CODEC_ID_H264; return true;
    case EncodedCodec::HEVC: result = AV_CODEC_ID_HEVC; return true;
    case EncodedCodec::AV1: result = AV_CODEC_ID_AV1; return true;
    case EncodedCodec::AAC: result = AV_CODEC_ID_AAC; return true;
    case EncodedCodec::Opus: result = AV_CODEC_ID_OPUS; return true;
    }
    return false;
}

bool pixelFormat(PixelFormat format, AVPixelFormat& result) {
    switch (format) {
    case PixelFormat::BGRA8: result = AV_PIX_FMT_BGRA; return true;
    case PixelFormat::NV12: result = AV_PIX_FMT_NV12; return true;
    case PixelFormat::YUV420P: result = AV_PIX_FMT_YUV420P; return true;
    case PixelFormat::Unknown: return false;
    }
    return false;
}

bool sampleFormat(AudioSampleFormat format, AVSampleFormat& result) {
    switch (format) {
    case AudioSampleFormat::Float32: result = AV_SAMPLE_FMT_FLT; return true;
    case AudioSampleFormat::Float32Planar: result = AV_SAMPLE_FMT_FLTP; return true;
    case AudioSampleFormat::Signed16: result = AV_SAMPLE_FMT_S16; return true;
    case AudioSampleFormat::Signed16Planar: result = AV_SAMPLE_FMT_S16P; return true;
    case AudioSampleFormat::Unknown: return false;
    }
    return false;
}

std::uint64_t channelMask(AudioChannelLayout layout) {
    switch (layout) {
    case AudioChannelLayout::Mono: return AV_CH_LAYOUT_MONO;
    case AudioChannelLayout::Stereo: return AV_CH_LAYOUT_STEREO;
    case AudioChannelLayout::Surround51: return AV_CH_LAYOUT_5POINT1;
    case AudioChannelLayout::Surround71: return AV_CH_LAYOUT_7POINT1;
    case AudioChannelLayout::Unknown: return 0;
    }
    return 0;
}

AVRational toAVRational(MediaTimebase timebase) {
    return AVRational{timebase.numerator, timebase.denominator};
}

bool sameTimebase(MediaTimebase left, MediaTimebase right) {
    return av_cmp_q(toAVRational(left), toAVRational(right)) == 0;
}

} // namespace

struct FFmpegMediaMuxer::Impl final {
    enum class State : std::uint8_t { Empty, Configured, Open, Finalized, Aborted, Failed };

    struct Stream {
        AVStream* avStream{nullptr};
        MediaTimebase encoderTimebase{};
        EncodedMediaType mediaType{EncodedMediaType::Video};
        std::int64_t lastWrittenDts{AV_NOPTS_VALUE};
    };

    State state{State::Empty};
    AVFormatContext* format{nullptr};
    RecordingOutputConfig config;
    std::unordered_map<std::int32_t, Stream> streams;

    void releaseContext() noexcept {
        if (!format) return;
        if (format->pb && format->oformat &&
            (format->oformat->flags & AVFMT_NOFILE) == 0) {
            (void)avio_closep(&format->pb);
        }
        avformat_free_context(format);
        format = nullptr;
    }

    bool addStream(const EncodedStreamDescriptor& descriptor, std::string& error) {
        AVCodecID id = AV_CODEC_ID_NONE;
        if (!codecId(descriptor.codec, id)) {
            error = "Nicht unterstützter Codec im Stream-Deskriptor.";
            return false;
        }

        AVStream* stream = avformat_new_stream(format, nullptr);
        if (!stream) {
            error = "FFmpeg konnte keinen Ausgabestream anlegen.";
            return false;
        }

        stream->time_base = toAVRational(descriptor.timebase);
        if (descriptor.mediaType == EncodedMediaType::Video) {
            stream->avg_frame_rate = AVRational{
                descriptor.frameRateNumerator, descriptor.frameRateDenominator};
            stream->r_frame_rate = stream->avg_frame_rate;
        }

        AVCodecParameters* parameters = stream->codecpar;
        parameters->codec_type = descriptor.mediaType == EncodedMediaType::Video
            ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
        parameters->codec_id = id;
        parameters->codec_tag = 0;
        parameters->bit_rate = descriptor.bitrate;
        if (descriptor.profile >= 0) parameters->profile = descriptor.profile;
        if (descriptor.level >= 0) parameters->level = descriptor.level;

        if (descriptor.mediaType == EncodedMediaType::Video) {
            AVPixelFormat formatValue = AV_PIX_FMT_NONE;
            if (!pixelFormat(descriptor.pixelFormat, formatValue)) {
                error = "Unbekanntes Pixelformat im Video-Deskriptor.";
                return false;
            }
            parameters->width = descriptor.width;
            parameters->height = descriptor.height;
            parameters->format = formatValue;
        } else {
            AVSampleFormat formatValue = AV_SAMPLE_FMT_NONE;
            const std::uint64_t mask = channelMask(descriptor.channelLayout);
            if (!sampleFormat(descriptor.sampleFormat, formatValue) || mask == 0) {
                error = "Unbekanntes Audioformat oder Kanal-Layout im Deskriptor.";
                return false;
            }
            parameters->sample_rate = descriptor.sampleRate;
            parameters->format = formatValue;
            const int layoutResult = av_channel_layout_from_mask(
                &parameters->ch_layout, mask);
            if (layoutResult < 0) {
                error = "FFmpeg konnte das Audio-Kanal-Layout nicht setzen: " +
                        ffmpegError(layoutResult);
                return false;
            }
            if (parameters->ch_layout.nb_channels != descriptor.channels) {
                error = "Kanalanzahl passt nicht zum angegebenen Kanal-Layout.";
                return false;
            }
        }

        if (!descriptor.codecExtradata.empty()) {
            if (descriptor.codecExtradata.size() >
                static_cast<std::size_t>(std::numeric_limits<int>::max() -
                                         AV_INPUT_BUFFER_PADDING_SIZE)) {
                error = "Codec-Extradata ist zu groß für FFmpeg.";
                return false;
            }
            const auto size = static_cast<int>(descriptor.codecExtradata.size());
            parameters->extradata = static_cast<std::uint8_t*>(
                av_mallocz(static_cast<std::size_t>(size) +
                           AV_INPUT_BUFFER_PADDING_SIZE));
            if (!parameters->extradata) {
                error = "Speicher für Codec-Extradata konnte nicht reserviert werden.";
                return false;
            }
            std::memcpy(parameters->extradata, descriptor.codecExtradata.data(),
                        descriptor.codecExtradata.size());
            parameters->extradata_size = size;
        }

        Stream entry;
        entry.avStream = stream;
        entry.encoderTimebase = descriptor.timebase;
        entry.mediaType = descriptor.mediaType;
        streams.emplace(descriptor.streamIndex, entry);
        return true;
    }
};

FFmpegMediaMuxer::FFmpegMediaMuxer() : m_impl(std::make_unique<Impl>()) {}

FFmpegMediaMuxer::~FFmpegMediaMuxer() {
    abort();
}

bool FFmpegMediaMuxer::configure(
    const RecordingOutputConfig& config,
    const std::vector<EncodedStreamDescriptor>& descriptors,
    std::string& error) {

    if (m_impl->state != Impl::State::Empty) {
        error = "FFmpeg-Muxer wurde bereits konfiguriert.";
        return false;
    }
    if (config.outputPath.empty() || descriptors.empty()) {
        error = "Ausgabepfad und mindestens ein Stream sind erforderlich.";
        m_impl->state = Impl::State::Failed;
        return false;
    }

    std::error_code filesystemError;
    const auto* utf8Begin =
        reinterpret_cast<const char8_t*>(config.outputPath.data());
    const std::u8string utf8Path(
        utf8Begin, utf8Begin + config.outputPath.size());
    const std::filesystem::path path(utf8Path);
    const bool exists = std::filesystem::exists(path, filesystemError);
    if (filesystemError) {
        error = "Ausgabedatei konnte nicht geprüft werden: " +
                filesystemError.message();
        m_impl->state = Impl::State::Failed;
        return false;
    }
    if (exists && !config.overwriteExisting) {
        error = "Ausgabedatei existiert bereits; Überschreiben ist deaktiviert.";
        m_impl->state = Impl::State::Failed;
        return false;
    }

    const char* formatName = nullptr;
    switch (config.container) {
    case RecordingContainer::Matroska: formatName = "matroska"; break;
    case RecordingContainer::Mp4: formatName = "mp4"; break;
    default:
        error = "Nicht unterstützter Container.";
        m_impl->state = Impl::State::Failed;
        return false;
    }

    const int allocation = avformat_alloc_output_context2(
        &m_impl->format, nullptr, formatName, config.outputPath.c_str());
    if (allocation < 0 || !m_impl->format) {
        error = "FFmpeg-Ausgabeformat konnte nicht angelegt werden: " +
                ffmpegError(allocation < 0 ? allocation : AVERROR_UNKNOWN);
        m_impl->releaseContext();
        m_impl->state = Impl::State::Failed;
        return false;
    }

    m_impl->config = config;
    for (const auto& descriptor : descriptors) {
        if (!descriptor.isValid() ||
            m_impl->streams.contains(descriptor.streamIndex)) {
            error = "Ungültiger oder doppelter Stream-Deskriptor.";
            m_impl->releaseContext();
            m_impl->streams.clear();
            m_impl->state = Impl::State::Failed;
            return false;
        }
        if (!m_impl->addStream(descriptor, error)) {
            m_impl->releaseContext();
            m_impl->streams.clear();
            m_impl->state = Impl::State::Failed;
            return false;
        }
    }

    m_impl->state = Impl::State::Configured;
    return true;
}

bool FFmpegMediaMuxer::open(std::string& error) {
    if (m_impl->state != Impl::State::Configured || !m_impl->format) {
        error = "FFmpeg-Muxer ist nicht konfiguriert.";
        return false;
    }

    if ((m_impl->format->oformat->flags & AVFMT_NOFILE) == 0) {
        const int openResult = avio_open(
            &m_impl->format->pb, m_impl->config.outputPath.c_str(), AVIO_FLAG_WRITE);
        if (openResult < 0) {
            error = "Ausgabedatei konnte nicht geöffnet werden: " +
                    ffmpegError(openResult);
            m_impl->releaseContext();
            m_impl->state = Impl::State::Failed;
            return false;
        }
    }

    const int headerResult = avformat_write_header(m_impl->format, nullptr);
    if (headerResult < 0) {
        error = "Container-Header konnte nicht geschrieben werden: " +
                ffmpegError(headerResult);
        m_impl->releaseContext();
        m_impl->state = Impl::State::Failed;
        return false;
    }

    // Muxers may replace stream time bases while writing the header.
    m_impl->state = Impl::State::Open;
    return true;
}

bool FFmpegMediaMuxer::writePacket(const EncodedPacket& packet, std::string& error) {
    if (m_impl->state != Impl::State::Open || !m_impl->format) {
        error = "FFmpeg-Muxer ist nicht geöffnet.";
        return false;
    }
    if (!packet.isValid()) {
        error = "Ungültiges EncodedPacket.";
        return false;
    }

    const auto found = m_impl->streams.find(packet.streamIndex);
    if (found == m_impl->streams.end()) {
        error = "Paket verweist auf einen unbekannten StreamIndex.";
        return false;
    }
    auto& stream = found->second;
    if (packet.mediaType != stream.mediaType ||
        !sameTimebase(packet.timebase, stream.encoderTimebase)) {
        error = "Paket-Medientyp oder Zeitbasis stimmt nicht mit dem Stream überein.";
        return false;
    }
    if (packet.data->size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = "EncodedPacket ist zu groß für einen FFmpeg-AVPacket.";
        return false;
    }

    AVPacket* avPacket = av_packet_alloc();
    if (!avPacket) {
        error = "FFmpeg konnte keinen AVPacket reservieren.";
        return false;
    }

    const int packetSize = static_cast<int>(packet.data->size());
    const int allocation = av_new_packet(avPacket, packetSize);
    if (allocation < 0) {
        error = "AVPacket-Puffer konnte nicht reserviert werden: " +
                ffmpegError(allocation);
        av_packet_free(&avPacket);
        return false;
    }
    std::memcpy(avPacket->data, packet.data->data(), packet.data->size());

    avPacket->pts = packet.pts;
    avPacket->dts = packet.dts;
    avPacket->duration = packet.duration;
    avPacket->stream_index = stream.avStream->index;
    avPacket->pos = -1;
    if (packet.keyFrame) avPacket->flags |= AV_PKT_FLAG_KEY;

    av_packet_rescale_ts(avPacket, toAVRational(stream.encoderTimebase),
                         stream.avStream->time_base);

    if (avPacket->dts != AV_NOPTS_VALUE &&
        stream.lastWrittenDts != AV_NOPTS_VALUE &&
        avPacket->dts <= stream.lastWrittenDts) {
        error = "Nicht-monotone DTS-Reihenfolge im Stream.";
        av_packet_free(&avPacket);
        return false;
    }

    const std::int64_t writtenDts = avPacket->dts;
    const int writeResult = av_interleaved_write_frame(m_impl->format, avPacket);
    av_packet_free(&avPacket);
    if (writeResult < 0) {
        error = "Paket konnte nicht in den Container geschrieben werden: " +
                ffmpegError(writeResult);
        m_impl->state = Impl::State::Failed;
        return false;
    }

    if (writtenDts != AV_NOPTS_VALUE) stream.lastWrittenDts = writtenDts;
    return true;
}

bool FFmpegMediaMuxer::finalize(std::string& error) {
    if (m_impl->state != Impl::State::Open || !m_impl->format) {
        error = "FFmpeg-Muxer kann nicht finalisiert werden, da er nicht geöffnet ist.";
        return false;
    }

    const int trailerResult = av_write_trailer(m_impl->format);
    int closeResult = 0;
    if (m_impl->format->pb && m_impl->format->oformat &&
        (m_impl->format->oformat->flags & AVFMT_NOFILE) == 0) {
        closeResult = avio_closep(&m_impl->format->pb);
    }

    if (trailerResult < 0 || closeResult < 0) {
        if (trailerResult < 0) {
            error = "Container-Trailer konnte nicht geschrieben werden: " +
                    ffmpegError(trailerResult);
        }
        if (closeResult < 0) {
            if (!error.empty()) error += " ";
            error += "Ausgabedatei konnte nicht sauber geschlossen werden: " +
                     ffmpegError(closeResult);
        }
        avformat_free_context(m_impl->format);
        m_impl->format = nullptr;
        m_impl->state = Impl::State::Failed;
        return false;
    }

    avformat_free_context(m_impl->format);
    m_impl->format = nullptr;
    m_impl->state = Impl::State::Finalized;
    return true;
}

void FFmpegMediaMuxer::abort() noexcept {
    if (!m_impl) return;
    if (m_impl->state == Impl::State::Finalized ||
        m_impl->state == Impl::State::Aborted) return;
    m_impl->releaseContext();
    m_impl->streams.clear();
    m_impl->state = Impl::State::Aborted;
}

std::unique_ptr<IMediaMuxer> createFFmpegMediaMuxer() {
    return std::make_unique<FFmpegMediaMuxer>();
}

} // namespace streamforge
