#include "FFmpegAudioEncoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cerrno>
#include <exception>
#include <cstring>
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

bool codecId(AudioCodec codec, AVCodecID& id) {
    switch (codec) {
    case AudioCodec::AAC: id = AV_CODEC_ID_AAC; return true;
    case AudioCodec::Opus: id = AV_CODEC_ID_OPUS; return true;
    }
    return false;
}

bool supportedRate(const AVCodec* codec, int rate) {
    if (!codec->supported_samplerates) return true;
    for (const int* value = codec->supported_samplerates; *value != 0; ++value) {
        if (*value == rate) return true;
    }
    return false;
}

bool supportedSampleFormat(const AVCodec* codec, AVSampleFormat& chosen) {
    if (!codec->sample_fmts) {
        chosen = AV_SAMPLE_FMT_FLTP;
        return true;
    }
    constexpr AVSampleFormat preference[] = {
        AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT,
        AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_S16
    };
    for (const auto preferred : preference) {
        for (const AVSampleFormat* current = codec->sample_fmts;
             *current != AV_SAMPLE_FMT_NONE; ++current) {
            if (*current == preferred) {
                chosen = preferred;
                return true;
            }
        }
    }
    return false;
}

AudioChannelLayout channelLayoutForCount(int channels) {
    switch (channels) {
    case 1: return AudioChannelLayout::Mono;
    case 2: return AudioChannelLayout::Stereo;
    case 6: return AudioChannelLayout::Surround51;
    case 8: return AudioChannelLayout::Surround71;
    default: return AudioChannelLayout::Unknown;
    }
}

AudioSampleFormat sampleFormatFor(AVSampleFormat format) {
    switch (format) {
    case AV_SAMPLE_FMT_FLT: return AudioSampleFormat::Float32;
    case AV_SAMPLE_FMT_FLTP: return AudioSampleFormat::Float32Planar;
    case AV_SAMPLE_FMT_S16: return AudioSampleFormat::Signed16;
    case AV_SAMPLE_FMT_S16P: return AudioSampleFormat::Signed16Planar;
    default: return AudioSampleFormat::Unknown;
    }
}

} // namespace

struct FFmpegAudioEncoder::Impl final {
    enum class State : std::uint8_t { Empty, Ready, Flushing, Finished, Failed };

    AudioEncoderConfig config;
    EncodedPacketCallback callback;
    std::int32_t streamIndex{-1};
    AVCodecContext* codec{nullptr};
    SwrContext* resampler{nullptr};
    AVAudioFifo* fifo{nullptr};
    State state{State::Empty};
    std::int64_t nextPts{0};
    std::int64_t expectedTimestampNs{0};
    bool hasTimestamp{false};

    explicit Impl(AudioEncoderConfig c, EncodedPacketCallback cb, std::int32_t index)
        : config(std::move(c)), callback(std::move(cb)), streamIndex(index) {}

    void release() noexcept {
        if (fifo) av_audio_fifo_free(fifo);
        fifo = nullptr;
        swr_free(&resampler);
        avcodec_free_context(&codec);
    }

    bool receivePackets(std::string& error, int& produced, bool& eof) {
        produced = 0;
        eof = false;
        AVPacket* packet = av_packet_alloc();
        if (!packet) {
            error = "FFmpeg konnte keinen Audio-AVPacket reservieren.";
            return false;
        }

        for (;;) {
            const int result = avcodec_receive_packet(codec, packet);
            if (result == AVERROR(EAGAIN)) break;
            if (result == AVERROR_EOF) { eof = true; break; }
            if (result < 0) {
                error = "Audioencoder-Paket konnte nicht gelesen werden: " +
                        ffmpegError(result);
                av_packet_free(&packet);
                return false;
            }
            if (packet->size <= 0 || !packet->data) {
                error = "Audioencoder lieferte ein leeres Paket.";
                av_packet_free(&packet);
                return false;
            }

            EncodedPacket encoded;
            encoded.mediaType = EncodedMediaType::Audio;
            encoded.streamIndex = streamIndex;
            encoded.pts = packet->pts;
            encoded.dts = packet->dts;
            encoded.duration = packet->duration > 0
                ? packet->duration : std::max(codec->frame_size, 1);
            encoded.timebase = {codec->time_base.num, codec->time_base.den};
            encoded.keyFrame = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            encoded.data = std::make_shared<const std::vector<std::uint8_t>>(
                packet->data, packet->data + packet->size);

            av_packet_unref(packet);
            ++produced;
            try {
                if (!callback(std::move(encoded), error)) {
                    if (error.empty()) error = "RecordingPipeline hat Audiopaket abgelehnt.";
                    av_packet_free(&packet);
                    return false;
                }
            } catch (const std::exception& ex) {
                error = std::string("Audio-Packet-Callback fehlgeschlagen: ") + ex.what();
                av_packet_free(&packet);
                return false;
            } catch (...) {
                error = "Unbekannte Ausnahme im Audio-Packet-Callback.";
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
                nextPts += frame ? frame->nb_samples : 0;
                av_frame_free(&frame);
                int produced = 0;
                bool eof = false;
                return receivePackets(error, produced, eof);
            }
            if (result != AVERROR(EAGAIN)) {
                error = "Audioframe konnte nicht an FFmpeg übergeben werden: " +
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
                error = "Audioencoder meldet EAGAIN ohne ausstehende Pakete.";
                av_frame_free(&frame);
                return false;
            }
        }

        error = "Audioencoder erreichte beim Senden keine Fortschritte.";
        av_frame_free(&frame);
        return false;
    }

    bool encodeFifoFrame(int frameSamples, std::string& error) {
        AVFrame* frame = av_frame_alloc();
        if (!frame) {
            error = "Audio-AVFrame konnte nicht reserviert werden.";
            return false;
        }
        frame->nb_samples = frameSamples;
        frame->format = codec->sample_fmt;
        frame->sample_rate = codec->sample_rate;
        frame->pts = nextPts;
        int result = av_channel_layout_copy(&frame->ch_layout, &codec->ch_layout);
        if (result >= 0) result = av_frame_get_buffer(frame, 0);
        if (result < 0) {
            error = "Audio-AVFrame konnte nicht vorbereitet werden: " +
                    ffmpegError(result);
            av_frame_free(&frame);
            return false;
        }

        const int available = av_audio_fifo_size(fifo);
        const int toRead = std::min(available, frameSamples);
        if (toRead > 0) {
            result = av_audio_fifo_read(
                fifo, reinterpret_cast<void**>(frame->extended_data), toRead);
            if (result != toRead) {
                error = "Audio-FIFO lieferte weniger Samples als angefordert.";
                av_frame_free(&frame);
                return false;
            }
        }
        if (toRead < frameSamples) {
            result = av_samples_set_silence(
                frame->extended_data, toRead, frameSamples - toRead,
                codec->ch_layout.nb_channels, codec->sample_fmt);
            if (result < 0) {
                error = "Audioframe konnte nicht mit Stille aufgefüllt werden: " +
                        ffmpegError(result);
                av_frame_free(&frame);
                return false;
            }
        }
        return sendFrame(frame, error);
    }

    bool encodeAvailableFrames(std::string& error) {
        const int frameSize = codec->frame_size > 0 ? codec->frame_size : 1024;
        while (av_audio_fifo_size(fifo) >= frameSize) {
            if (!encodeFifoFrame(frameSize, error)) return false;
        }
        return true;
    }

    bool appendConverted(const std::uint8_t** input, int inputSamples,
                         std::string& error) {
        const int capacity = swr_get_out_samples(resampler, inputSamples);
        if (capacity < 0) {
            error = "Resampler-Ausgabemenge konnte nicht berechnet werden: " +
                    ffmpegError(capacity);
            return false;
        }
        if (capacity == 0) return true;

        AVFrame* converted = av_frame_alloc();
        if (!converted) {
            error = "Resampler-AVFrame konnte nicht reserviert werden.";
            return false;
        }
        converted->nb_samples = capacity;
        converted->format = codec->sample_fmt;
        converted->sample_rate = codec->sample_rate;
        int result = av_channel_layout_copy(&converted->ch_layout, &codec->ch_layout);
        if (result >= 0) result = av_frame_get_buffer(converted, 0);
        if (result < 0) {
            error = "Resampler-AVFrame konnte nicht vorbereitet werden: " +
                    ffmpegError(result);
            av_frame_free(&converted);
            return false;
        }

        const int convertedSamples = swr_convert(
            resampler, converted->extended_data, capacity, input, inputSamples);
        if (convertedSamples < 0) {
            error = "Audio-Resampling fehlgeschlagen: " + ffmpegError(convertedSamples);
            av_frame_free(&converted);
            return false;
        }
        if (convertedSamples > 0) {
            const int current = av_audio_fifo_size(fifo);
            if (av_audio_fifo_realloc(fifo, current + convertedSamples) < 0) {
                error = "Audio-FIFO konnte nicht vergrößert werden.";
                av_frame_free(&converted);
                return false;
            }
            result = av_audio_fifo_write(
                fifo, reinterpret_cast<void**>(converted->extended_data),
                convertedSamples);
            if (result != convertedSamples) {
                error = "Audio-FIFO konnte nicht vollständig beschrieben werden.";
                av_frame_free(&converted);
                return false;
            }
        }
        av_frame_free(&converted);
        return true;
    }

    bool flushResampler(std::string& error) {
        for (int iteration = 0; iteration < 128; ++iteration) {
            const std::int64_t delay = swr_get_delay(resampler, config.inputSampleRate);
            if (delay <= 0) return true;
            const std::int64_t capacity64 = av_rescale_rnd(
                delay, codec->sample_rate, config.inputSampleRate, AV_ROUND_UP);
            if (capacity64 <= 0 ||
                capacity64 > std::numeric_limits<int>::max()) {
                error = "Ungültige Resampler-Flush-Kapazität.";
                return false;
            }
            const int capacity = static_cast<int>(capacity64);
            AVFrame* converted = av_frame_alloc();
            if (!converted) {
                error = "Resampler-Flush-Frame konnte nicht reserviert werden.";
                return false;
            }
            converted->nb_samples = capacity;
            converted->format = codec->sample_fmt;
            converted->sample_rate = codec->sample_rate;
            int result = av_channel_layout_copy(&converted->ch_layout, &codec->ch_layout);
            if (result >= 0) result = av_frame_get_buffer(converted, 0);
            if (result < 0) {
                error = "Resampler-Flush-Frame konnte nicht vorbereitet werden: " +
                        ffmpegError(result);
                av_frame_free(&converted);
                return false;
            }

            const int samples = swr_convert(
                resampler, converted->extended_data, capacity, nullptr, 0);
            if (samples < 0) {
                error = "Resampler-Flush fehlgeschlagen: " + ffmpegError(samples);
                av_frame_free(&converted);
                return false;
            }
            if (samples == 0) {
                av_frame_free(&converted);
                return true;
            }

            const int current = av_audio_fifo_size(fifo);
            if (av_audio_fifo_realloc(fifo, current + samples) < 0 ||
                av_audio_fifo_write(fifo,
                    reinterpret_cast<void**>(converted->extended_data), samples) != samples) {
                error = "Resampler-Rest konnte nicht in den Audio-FIFO geschrieben werden.";
                av_frame_free(&converted);
                return false;
            }
            av_frame_free(&converted);
        }
        error = "Resampler-Flush überschritt die Fortschrittsgrenze.";
        return false;
    }
};

FFmpegAudioEncoder::FFmpegAudioEncoder(
    AudioEncoderConfig config, EncodedPacketCallback callback, std::int32_t streamIndex)
    : m_impl(std::make_unique<Impl>(
          std::move(config), std::move(callback), streamIndex)) {}

FFmpegAudioEncoder::~FFmpegAudioEncoder() {
    shutdown();
}

bool FFmpegAudioEncoder::initialize(std::string& error) {
    if (m_impl->state != Impl::State::Empty) {
        error = "Audioencoder wurde bereits initialisiert.";
        return false;
    }
    if (!m_impl->callback || m_impl->streamIndex < 0 ||
        m_impl->config.inputSampleRate <= 0 ||
        m_impl->config.outputSampleRate <= 0 ||
        m_impl->config.inputChannels <= 0 ||
        m_impl->config.outputChannels <= 0 ||
        m_impl->config.bitrate <= 0 ||
        channelLayoutForCount(m_impl->config.inputChannels) == AudioChannelLayout::Unknown ||
        channelLayoutForCount(m_impl->config.outputChannels) == AudioChannelLayout::Unknown) {
        error = "Ungültige FFmpeg-Audioencoder-Konfiguration.";
        m_impl->state = Impl::State::Failed;
        return false;
    }

    AVCodecID id = AV_CODEC_ID_NONE;
    if (!codecId(m_impl->config.codec, id)) {
        error = "Nicht unterstützter Audio-Codec.";
        m_impl->state = Impl::State::Failed;
        return false;
    }
    const AVCodec* encoder = avcodec_find_encoder(id);
    if (!encoder) {
        error = "FFmpeg-Encoder für den angeforderten Audio-Codec fehlt.";
        m_impl->state = Impl::State::Failed;
        return false;
    }
    if (!supportedRate(encoder, m_impl->config.outputSampleRate)) {
        error = "Audioencoder unterstützt die konfigurierte Samplerate nicht.";
        m_impl->state = Impl::State::Failed;
        return false;
    }

    AVSampleFormat chosenFormat = AV_SAMPLE_FMT_NONE;
    if (!supportedSampleFormat(encoder, chosenFormat)) {
        error = "Audioencoder bietet kein unterstütztes PCM-Sampleformat.";
        m_impl->state = Impl::State::Failed;
        return false;
    }

    m_impl->codec = avcodec_alloc_context3(encoder);
    if (!m_impl->codec) {
        error = "FFmpeg-Audio-CodecContext konnte nicht reserviert werden.";
        m_impl->state = Impl::State::Failed;
        return false;
    }

    m_impl->codec->sample_rate = m_impl->config.outputSampleRate;
    m_impl->codec->sample_fmt = chosenFormat;
    m_impl->codec->bit_rate = m_impl->config.bitrate;
    m_impl->codec->time_base = AVRational{1, m_impl->config.outputSampleRate};
    m_impl->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_channel_layout_default(
        &m_impl->codec->ch_layout, m_impl->config.outputChannels);

    int result = avcodec_open2(m_impl->codec, encoder, nullptr);
    if (result < 0) {
        error = "FFmpeg-Audioencoder konnte nicht geöffnet werden: " +
                ffmpegError(result);
        m_impl->release();
        m_impl->state = Impl::State::Failed;
        return false;
    }

    AVChannelLayout inputLayout{};
    av_channel_layout_default(&inputLayout, m_impl->config.inputChannels);
    result = swr_alloc_set_opts2(
        &m_impl->resampler,
        &m_impl->codec->ch_layout, m_impl->codec->sample_fmt,
        m_impl->codec->sample_rate,
        &inputLayout, AV_SAMPLE_FMT_FLT,
        m_impl->config.inputSampleRate, 0, nullptr);
    av_channel_layout_uninit(&inputLayout);
    if (result < 0 || !m_impl->resampler) {
        error = "Audio-Resampler konnte nicht konfiguriert werden: " +
                ffmpegError(result < 0 ? result : AVERROR(ENOMEM));
        m_impl->release();
        m_impl->state = Impl::State::Failed;
        return false;
    }
    result = swr_init(m_impl->resampler);
    if (result < 0) {
        error = "Audio-Resampler konnte nicht initialisiert werden: " +
                ffmpegError(result);
        m_impl->release();
        m_impl->state = Impl::State::Failed;
        return false;
    }

    const int initialFifoSize = std::max(m_impl->codec->frame_size * 2, 2048);
    m_impl->fifo = av_audio_fifo_alloc(
        m_impl->codec->sample_fmt, m_impl->codec->ch_layout.nb_channels,
        initialFifoSize);
    if (!m_impl->fifo) {
        error = "Audio-FIFO konnte nicht reserviert werden.";
        m_impl->release();
        m_impl->state = Impl::State::Failed;
        return false;
    }

    m_impl->state = Impl::State::Ready;
    return true;
}

bool FFmpegAudioEncoder::encode(const AudioFrame& frame, std::string& error) {
    if (m_impl->state != Impl::State::Ready) {
        error = "Audioencoder ist nicht aufnahmebereit.";
        return false;
    }
    if (!frame.isValid() ||
        frame.sampleRate != m_impl->config.inputSampleRate ||
        frame.channels != m_impl->config.inputChannels) {
        error = "Audioframe passt nicht zur konfigurierten Eingabeformatierung.";
        m_impl->state = Impl::State::Failed;
        return false;
    }

    if (!m_impl->hasTimestamp) {
        m_impl->expectedTimestampNs = frame.timestampNs;
        m_impl->hasTimestamp = true;
    } else {
        const std::int64_t delta = frame.timestampNs - m_impl->expectedTimestampNs;
        constexpr std::int64_t toleranceNs = 5'000'000;
        if (delta < -toleranceNs || delta > toleranceNs) {
            error = "Audioframe-Zeitstempel weist eine Lücke oder Überlappung auf.";
            m_impl->state = Impl::State::Failed;
            return false;
        }
    }

    const std::int64_t frameDurationNs = av_rescale_q(
        frame.frameCount, AVRational{1, frame.sampleRate}, AVRational{1, 1'000'000'000});
    m_impl->expectedTimestampNs = frame.timestampNs + frameDurationNs;

    const std::uint8_t* input[1] = {
        reinterpret_cast<const std::uint8_t*>(frame.samples.data())
    };
    if (!m_impl->appendConverted(input, frame.frameCount, error) ||
        !m_impl->encodeAvailableFrames(error)) {
        m_impl->state = Impl::State::Failed;
        return false;
    }
    return true;
}

bool FFmpegAudioEncoder::flush(std::string& error) {
    if (m_impl->state != Impl::State::Ready) {
        error = "Audioencoder kann in diesem Zustand nicht geflusht werden.";
        return false;
    }
    m_impl->state = Impl::State::Flushing;

    if (!m_impl->flushResampler(error) ||
        !m_impl->encodeAvailableFrames(error)) {
        m_impl->state = Impl::State::Failed;
        return false;
    }

    const int remaining = av_audio_fifo_size(m_impl->fifo);
    if (remaining > 0) {
        const int frameSize = m_impl->codec->frame_size > 0
            ? m_impl->codec->frame_size : remaining;
        if (!m_impl->encodeFifoFrame(frameSize, error)) {
            m_impl->state = Impl::State::Failed;
            return false;
        }
    }

    bool sentNullFrame = false;
    for (int attempt = 0; attempt < 8 && !sentNullFrame; ++attempt) {
        const int result = avcodec_send_frame(m_impl->codec, nullptr);
        if (result == 0 || result == AVERROR_EOF) {
            sentNullFrame = true;
            break;
        }
        if (result != AVERROR(EAGAIN)) {
            error = "Audioencoder konnte Flush-Frame nicht senden: " +
                    ffmpegError(result);
            m_impl->state = Impl::State::Failed;
            return false;
        }
        int produced = 0;
        bool eof = false;
        if (!m_impl->receivePackets(error, produced, eof) || produced == 0) {
            if (error.empty()) error = "Audioencoder-Flush machte keinen Fortschritt.";
            m_impl->state = Impl::State::Failed;
            return false;
        }
    }
    if (!sentNullFrame) {
        error = "Audioencoder-Flush erreichte die Fortschrittsgrenze.";
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
            error = "Audioencoder meldete EAGAIN statt EOF nach dem Flush.";
            m_impl->state = Impl::State::Failed;
            return false;
        }
    }

    error = "Audioencoder-Flush überschritt die Paketgrenze.";
    m_impl->state = Impl::State::Failed;
    return false;
}

bool FFmpegAudioEncoder::getStreamDescriptor(
    EncodedStreamDescriptor& descriptor, std::string& error) const {
    if (!m_impl->codec ||
        (m_impl->state != Impl::State::Ready &&
         m_impl->state != Impl::State::Flushing &&
         m_impl->state != Impl::State::Finished)) {
        error = "Audio-Stream-Deskriptor ist noch nicht verfügbar.";
        return false;
    }

    descriptor = {};
    descriptor.streamIndex = m_impl->streamIndex;
    descriptor.mediaType = EncodedMediaType::Audio;
    descriptor.codec = m_impl->config.codec == AudioCodec::Opus
        ? EncodedCodec::Opus : EncodedCodec::AAC;
    descriptor.timebase = {
        m_impl->codec->time_base.num, m_impl->codec->time_base.den};
    descriptor.sampleRate = m_impl->codec->sample_rate;
    descriptor.channels = m_impl->codec->ch_layout.nb_channels;
    descriptor.channelLayout = channelLayoutForCount(descriptor.channels);
    descriptor.sampleFormat = sampleFormatFor(m_impl->codec->sample_fmt);
    descriptor.bitrate = static_cast<std::int32_t>(m_impl->codec->bit_rate);
    descriptor.profile = m_impl->codec->profile;
    descriptor.level = m_impl->codec->level;
    if (m_impl->codec->extradata && m_impl->codec->extradata_size > 0) {
        descriptor.codecExtradata.assign(
            m_impl->codec->extradata,
            m_impl->codec->extradata + m_impl->codec->extradata_size);
    }
    if (!descriptor.isValid()) {
        error = "FFmpeg lieferte einen nicht unterstützten Audio-Stream-Deskriptor.";
        return false;
    }
    return true;
}

void FFmpegAudioEncoder::shutdown() noexcept {
    if (!m_impl) return;
    m_impl->release();
    if (m_impl->state != Impl::State::Finished &&
        m_impl->state != Impl::State::Failed) {
        m_impl->state = Impl::State::Empty;
    }
}

} // namespace streamforge
