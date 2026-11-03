#include "RecordingPipeline.h"

#include "core/media/EncodedStreamDescriptor.h"
#include "core/media/MediaTypes.h"

#include <condition_variable>
#include <exception>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

namespace streamforge {
namespace {

class CallGate final {
public:
    bool tryEnter() {
        std::lock_guard lock(m_mutex);
        if (!m_open) return false;
        ++m_active;
        return true;
    }

    void open() {
        std::lock_guard lock(m_mutex);
        m_open = true;
    }

    void close() {
        std::lock_guard lock(m_mutex);
        m_open = false;
        if (m_active == 0) m_idle.notify_all();
    }

    void closeAndWait() {
        std::unique_lock lock(m_mutex);
        m_open = false;
        m_idle.wait(lock, [this] { return m_active == 0; });
    }

    void waitUntilIdle() {
        std::unique_lock lock(m_mutex);
        m_idle.wait(lock, [this] { return m_active == 0; });
    }

    [[nodiscard]] bool isOpen() const {
        std::lock_guard lock(m_mutex);
        return m_open;
    }

    void leave() noexcept {
        std::lock_guard lock(m_mutex);
        if (m_active > 0) --m_active;
        if (!m_open && m_active == 0) m_idle.notify_all();
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_idle;
    std::size_t m_active{0};
    bool m_open{false};
};

class GateTicket final {
public:
    explicit GateTicket(CallGate& gate) noexcept : m_gate(&gate) {}
    ~GateTicket() { if (m_gate) m_gate->leave(); }
    GateTicket(const GateTicket&) = delete;
    GateTicket& operator=(const GateTicket&) = delete;
private:
    CallGate* m_gate;
};

enum class QueueState : std::uint8_t { Open, Draining, Aborted };
enum class PopResult : std::uint8_t { Packet, Drained, Aborted };

class EncodedPacketQueue final {
public:
    explicit EncodedPacketQueue(PacketQueueConfig config) : m_config(config) {}

    bool push(EncodedPacket&& packet, std::string& error) {
        if (!packet.isValid()) {
            error = "Paketwarteschlange erhielt ein ungültiges Paket.";
            return false;
        }

        const std::size_t bytes = packet.data->size();
        std::lock_guard lock(m_mutex);
        if (m_state != QueueState::Open) {
            error = "Paketwarteschlange nimmt keine Pakete mehr an.";
            return false;
        }
        if (m_config.maxPackets == 0 || m_config.maxBytes == 0 ||
            m_packets.size() >= m_config.maxPackets ||
            bytes > m_config.maxBytes || m_bytes > m_config.maxBytes - bytes) {
            error = "Paketwarteschlange ist voll; Aufnahme wird abgebrochen.";
            return false;
        }

        try {
            m_packets.emplace(std::move(packet));
        } catch (const std::exception& ex) {
            error = std::string("Paket konnte nicht eingereiht werden: ") + ex.what();
            return false;
        } catch (...) {
            error = "Paket konnte wegen eines unbekannten Fehlers nicht eingereiht werden.";
            return false;
        }

        m_bytes += bytes;
        m_changed.notify_one();
        return true;
    }

    PopResult pop(EncodedPacket& packet) {
        std::unique_lock lock(m_mutex);
        m_changed.wait(lock, [this] {
            return !m_packets.empty() || m_state != QueueState::Open;
        });

        if (m_state == QueueState::Aborted) return PopResult::Aborted;
        if (!m_packets.empty()) {
            packet = std::move(m_packets.front());
            m_packets.pop();
            const std::size_t bytes = packet.data ? packet.data->size() : 0;
            m_bytes = bytes <= m_bytes ? m_bytes - bytes : 0;
            return PopResult::Packet;
        }
        return PopResult::Drained;
    }

    void closeForDrain() noexcept {
        std::lock_guard lock(m_mutex);
        if (m_state == QueueState::Open) m_state = QueueState::Draining;
        m_changed.notify_all();
    }

    void abort() noexcept {
        std::lock_guard lock(m_mutex);
        m_state = QueueState::Aborted;
        std::queue<EncodedPacket> empty;
        m_packets.swap(empty);
        m_bytes = 0;
        m_changed.notify_all();
    }

private:
    PacketQueueConfig m_config;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::queue<EncodedPacket> m_packets;
    std::size_t m_bytes{0};
    QueueState m_state{QueueState::Open};
};

struct CallbackState final {
    explicit CallbackState(std::shared_ptr<EncodedPacketQueue> q)
        : queue(std::move(q)) {}

    bool submit(EncodedPacket&& packet, std::string& error) {
        if (!gate.tryEnter()) {
            error = "Encoder-Callback liegt außerhalb des erlaubten Lebenszyklus.";
            return false;
        }
        GateTicket ticket(gate);
        return queue->push(std::move(packet), error);
    }

    CallGate gate;
    std::shared_ptr<EncodedPacketQueue> queue;
};

enum class WorkerOutcome : std::uint8_t { Pending, Completed, Aborted, Failed };

struct WorkerResult {
    WorkerOutcome outcome{WorkerOutcome::Pending};
    std::string error;
};

struct WorkerStartResult {
    bool ok{false};
    std::string error;
};

class MuxerWorker final {
public:
    using FailureHandler = std::function<void(const std::string&)>;

    MuxerWorker(std::unique_ptr<IMediaMuxer> muxer,
                std::shared_ptr<EncodedPacketQueue> queue,
                RecordingOutputConfig output,
                std::vector<EncodedStreamDescriptor> streams,
                FailureHandler failureHandler)
        : m_muxer(std::move(muxer)),
          m_queue(std::move(queue)),
          m_output(std::move(output)),
          m_streams(std::move(streams)),
          m_failureHandler(std::move(failureHandler)) {}

    ~MuxerWorker() {
        if (m_thread.joinable()) {
            m_queue->abort();
            m_thread.join();
        }
    }

    MuxerWorker(const MuxerWorker&) = delete;
    MuxerWorker& operator=(const MuxerWorker&) = delete;

    bool start(std::string& error) {
        if (!m_muxer || !m_queue) {
            error = "MuxerWorker benötigt Muxer und Paketwarteschlange.";
            return false;
        }

        auto promise = std::make_shared<std::promise<WorkerStartResult>>();
        auto future = promise->get_future();

        try {
            m_thread = std::thread([this, promise] {
                bool startupPublished = false;
                auto publishStartup = [&](WorkerStartResult value) {
                    if (!startupPublished) {
                        startupPublished = true;
                        promise->set_value(std::move(value));
                    }
                };

                try {
                    std::string localError;
                    if (!m_muxer->configure(m_output, m_streams, localError)) {
                        if (localError.empty()) localError = "Muxer-Konfiguration fehlgeschlagen.";
                        safeAbort();
                        setResult(WorkerOutcome::Failed, localError);
                        publishStartup({false, localError});
                        return;
                    }
                    if (!m_muxer->open(localError)) {
                        if (localError.empty()) localError = "Muxer konnte Ausgabe nicht öffnen.";
                        safeAbort();
                        setResult(WorkerOutcome::Failed, localError);
                        publishStartup({false, localError});
                        return;
                    }

                    publishStartup({true, {}});
                    for (;;) {
                        EncodedPacket packet;
                        const PopResult result = m_queue->pop(packet);
                        if (result == PopResult::Packet) {
                            localError.clear();
                            if (!m_muxer->writePacket(packet, localError)) {
                                if (localError.empty()) localError = "Muxer-Schreibfehler.";
                                failWorker(localError);
                                return;
                            }
                            continue;
                        }
                        if (result == PopResult::Aborted) {
                            safeAbort();
                            setResult(WorkerOutcome::Aborted, {});
                            return;
                        }

                        localError.clear();
                        if (!m_muxer->finalize(localError)) {
                            if (localError.empty()) localError = "Container-Finalisierung fehlgeschlagen.";
                            failWorker(localError);
                            return;
                        }
                        setResult(WorkerOutcome::Completed, {});
                        return;
                    }
                } catch (const std::exception& ex) {
                    const std::string message = std::string("MuxerWorker-Ausnahme: ") + ex.what();
                    if (!startupPublished) {
                        safeAbort();
                        setResult(WorkerOutcome::Failed, message);
                        publishStartup({false, message});
                    } else {
                        failWorker(message);
                    }
                } catch (...) {
                    const std::string message = "Unbekannte Ausnahme im MuxerWorker.";
                    if (!startupPublished) {
                        safeAbort();
                        setResult(WorkerOutcome::Failed, message);
                        publishStartup({false, message});
                    } else {
                        failWorker(message);
                    }
                }
            });
        } catch (const std::exception& ex) {
            error = std::string("Muxer-Thread konnte nicht gestartet werden: ") + ex.what();
            setResult(WorkerOutcome::Failed, error);
            return false;
        }

        WorkerStartResult result;
        try {
            result = future.get();
        } catch (const std::exception& ex) {
            error = std::string("Muxer-Startsignal fehlgeschlagen: ") + ex.what();
            m_queue->abort();
            if (m_thread.joinable()) m_thread.join();
            return false;
        }

        if (!result.ok) {
            error = result.error;
            if (m_thread.joinable()) m_thread.join();
            return false;
        }
        return true;
    }

    WorkerResult finish(bool drain) {
        if (drain) m_queue->closeForDrain();
        else m_queue->abort();
        if (m_thread.joinable()) m_thread.join();
        return getResult();
    }

private:
    void setResult(WorkerOutcome outcome, std::string error) {
        std::lock_guard lock(m_resultMutex);
        if (m_result.outcome == WorkerOutcome::Pending) {
            m_result.outcome = outcome;
            m_result.error = std::move(error);
        }
    }

    WorkerResult getResult() const {
        std::lock_guard lock(m_resultMutex);
        return m_result;
    }

    void safeAbort() noexcept {
        try {
            if (m_muxer) m_muxer->abort();
        } catch (...) {
            // The interface is noexcept; this is a final thread-boundary guard.
        }
    }

    void failWorker(const std::string& message) noexcept {
        m_queue->abort();
        setResult(WorkerOutcome::Failed, message);
        if (m_failureHandler) {
            try { m_failureHandler(message); } catch (...) {}
        }
        safeAbort();
    }

    std::unique_ptr<IMediaMuxer> m_muxer;
    std::shared_ptr<EncodedPacketQueue> m_queue;
    RecordingOutputConfig m_output;
    std::vector<EncodedStreamDescriptor> m_streams;
    FailureHandler m_failureHandler;
    std::thread m_thread;
    mutable std::mutex m_resultMutex;
    WorkerResult m_result;
};

bool validAudioCodec(AudioCodec codec) {
    return codec == AudioCodec::AAC || codec == AudioCodec::Opus;
}

bool validVideoCodec(VideoCodec codec) {
    return codec == VideoCodec::H264 || codec == VideoCodec::HEVC ||
           codec == VideoCodec::AV1;
}

EncodedCodec toEncodedCodec(AudioCodec codec) {
    return codec == AudioCodec::Opus ? EncodedCodec::Opus : EncodedCodec::AAC;
}

EncodedCodec toEncodedCodec(VideoCodec codec) {
    switch (codec) {
    case VideoCodec::HEVC: return EncodedCodec::HEVC;
    case VideoCodec::AV1: return EncodedCodec::AV1;
    default: return EncodedCodec::H264;
    }
}

bool validateConfig(const RecordingPipelineConfig& c, std::string& error) {
    if (c.audioStreamIndex < 0 || c.videoStreamIndex < 0 ||
        c.audioStreamIndex == c.videoStreamIndex) {
        error = "Audio- und Video-Stream benötigen verschiedene nichtnegative IDs.";
        return false;
    }
    if (!validAudioCodec(c.audio.codec) || c.audio.inputSampleRate <= 0 ||
        c.audio.inputChannels <= 0 || c.audio.outputSampleRate <= 0 ||
        c.audio.outputChannels <= 0 || c.audio.bitrate <= 0) {
        error = "Audioencoder-Konfiguration ist ungültig.";
        return false;
    }
    if (!validVideoCodec(c.video.codec) || c.video.width <= 0 ||
        c.video.height <= 0 || (c.video.width % 2) != 0 ||
        (c.video.height % 2) != 0 || c.video.fpsNumerator <= 0 ||
        c.video.fpsDenominator <= 0 || c.video.bitrate <= 0 ||
        c.video.maxBitrate < c.video.bitrate || c.video.keyframeInterval <= 0) {
        error = "Videoencoder-Konfiguration ist ungültig.";
        return false;
    }
    if (c.output.outputPath.empty()) {
        error = "Ausgabepfad darf nicht leer sein.";
        return false;
    }
    if (c.packetQueue.maxPackets == 0 || c.packetQueue.maxBytes == 0) {
        error = "Paketwarteschlange benötigt positive Kapazitätsgrenzen.";
        return false;
    }
    return true;
}

} // namespace

struct RecordingPipeline::Impl final {
    explicit Impl(RecordingPipelineFactories value) : factories(std::move(value)) {}

    RecordingPipelineFactories factories;
    mutable std::mutex stateMutex;
    RecordingState state{RecordingState::Idle};
    std::string error;

    std::mutex lifecycleMutex;
    bool cleanupDone{false};
    CallGate inputGate;
    std::mutex audioMutex;
    std::mutex videoMutex;

    std::shared_ptr<EncodedPacketQueue> queue;
    std::shared_ptr<CallbackState> callbacks;
    std::unique_ptr<IAudioEncoder> audioEncoder;
    std::unique_ptr<IVideoEncoder> videoEncoder;
    std::unique_ptr<MuxerWorker> muxerWorker;

    RecordingState currentState() const {
        std::lock_guard lock(stateMutex);
        return state;
    }

    bool hasFailure() const {
        return currentState() == RecordingState::Failed;
    }

    void setState(RecordingState next) {
        std::lock_guard lock(stateMutex);
        if (state != RecordingState::Failed &&
            state != RecordingState::Completed &&
            state != RecordingState::Aborted) state = next;
    }

    void fail(const std::string& message) noexcept {
        inputGate.close();
        {
            std::lock_guard lock(stateMutex);
            if (state == RecordingState::Completed || state == RecordingState::Aborted) return;
            if (state != RecordingState::Failed) {
                state = RecordingState::Failed;
                error = message.empty() ? "Unbekannter Aufnahmefehler." : message;
            }
        }
        if (queue) queue->abort();
    }

    void shutdownEncoders() noexcept {
        if (audioEncoder) audioEncoder->shutdown();
        if (videoEncoder) videoEncoder->shutdown();
    }

    void cleanupFailedStart(const std::string& message) noexcept {
        inputGate.closeAndWait();
        shutdownEncoders();
        if (callbacks) callbacks->gate.closeAndWait();
        if (queue) queue->abort();
        if (muxerWorker) (void)muxerWorker->finish(false);
        {
            std::lock_guard lock(stateMutex);
            state = RecordingState::Failed;
            if (error.empty()) error = message;
        }
        cleanupDone = true;
    }
};

RecordingPipeline::RecordingPipeline(RecordingPipelineFactories factories)
    : m_impl(std::make_unique<Impl>(std::move(factories))) {}

RecordingPipeline::~RecordingPipeline() {
    if (!m_impl) return;
    const auto state = m_impl->currentState();
    if (state != RecordingState::Idle && state != RecordingState::Completed &&
        state != RecordingState::Aborted) {
        std::string ignored;
        (void)stop(RecordingStopMode::Abort, ignored);
    }
}

bool RecordingPipeline::start(const RecordingPipelineConfig& config, std::string& error) {
    std::lock_guard lifecycle(m_impl->lifecycleMutex);
    if (m_impl->currentState() != RecordingState::Idle) {
        error = "Pipeline-Instanzen sind einmalig. Erzeuge eine neue Instanz.";
        return false;
    }
    m_impl->setState(RecordingState::Initializing);
    if (!validateConfig(config, error)) {
        m_impl->cleanupFailedStart(error);
        return false;
    }
    if (!m_impl->factories.createAudioEncoder || !m_impl->factories.createVideoEncoder ||
        !m_impl->factories.createMuxer) {
        error = "Audio-, Video- und Muxer-Factories müssen gesetzt sein.";
        m_impl->cleanupFailedStart(error);
        return false;
    }

    try {
        m_impl->queue = std::make_shared<EncodedPacketQueue>(config.packetQueue);
        m_impl->callbacks = std::make_shared<CallbackState>(m_impl->queue);
        std::weak_ptr<CallbackState> weak = m_impl->callbacks;

        const auto makeCallback = [weak](EncodedMediaType type, std::int32_t streamIndex) {
            return [weak, type, streamIndex](EncodedPacket&& packet, std::string& callbackError) {
                if (packet.mediaType != type || packet.streamIndex != streamIndex) {
                    callbackError = "Encoder lieferte falschen Medientyp oder StreamIndex.";
                    return false;
                }
                auto state = weak.lock();
                if (!state) {
                    callbackError = "RecordingPipeline existiert nicht mehr.";
                    return false;
                }
                return state->submit(std::move(packet), callbackError);
            };
        };

        m_impl->audioEncoder = m_impl->factories.createAudioEncoder(
            config.audio, makeCallback(EncodedMediaType::Audio, config.audioStreamIndex),
            config.audioStreamIndex);
        m_impl->videoEncoder = m_impl->factories.createVideoEncoder(
            config.video, makeCallback(EncodedMediaType::Video, config.videoStreamIndex),
            config.videoStreamIndex);
        if (!m_impl->audioEncoder || !m_impl->videoEncoder) {
            error = "Eine Encoder-Factory hat nullptr geliefert.";
            m_impl->cleanupFailedStart(error);
            return false;
        }

        if (!m_impl->audioEncoder->initialize(error)) {
            if (error.empty()) error = "Audioencoder-Initialisierung fehlgeschlagen.";
            m_impl->cleanupFailedStart(error);
            return false;
        }
        if (!m_impl->videoEncoder->initialize(error)) {
            if (error.empty()) error = "Videoencoder-Initialisierung fehlgeschlagen.";
            m_impl->cleanupFailedStart(error);
            return false;
        }

        EncodedStreamDescriptor audio;
        EncodedStreamDescriptor video;
        if (!m_impl->audioEncoder->getStreamDescriptor(audio, error) ||
            !m_impl->videoEncoder->getStreamDescriptor(video, error)) {
            if (error.empty()) error = "Encoder-Stream-Deskriptor konnte nicht gelesen werden.";
            m_impl->cleanupFailedStart(error);
            return false;
        }

        if (!audio.isValid() || audio.mediaType != EncodedMediaType::Audio ||
            audio.codec != toEncodedCodec(config.audio.codec) ||
            audio.streamIndex != config.audioStreamIndex ||
            !video.isValid() || video.mediaType != EncodedMediaType::Video ||
            video.codec != toEncodedCodec(config.video.codec) ||
            video.streamIndex != config.videoStreamIndex) {
            error = "Encoder-Stream-Deskriptoren sind ungültig oder widersprüchlich.";
            m_impl->cleanupFailedStart(error);
            return false;
        }

        std::vector<EncodedStreamDescriptor> streams;
        streams.reserve(2);
        streams.push_back(std::move(audio));
        streams.push_back(std::move(video));

        auto muxer = m_impl->factories.createMuxer();
        if (!muxer) {
            error = "Muxer-Factory hat nullptr geliefert.";
            m_impl->cleanupFailedStart(error);
            return false;
        }

        m_impl->muxerWorker = std::make_unique<MuxerWorker>(
            std::move(muxer), m_impl->queue, config.output, std::move(streams),
            [this](const std::string& message) { m_impl->fail(message); });

        if (!m_impl->muxerWorker->start(error)) {
            if (error.empty()) error = "MuxerWorker konnte nicht starten.";
            m_impl->cleanupFailedStart(error);
            return false;
        }

        m_impl->callbacks->gate.open();
        m_impl->setState(RecordingState::Recording);
        m_impl->inputGate.open();
        return true;
    } catch (const std::exception& ex) {
        error = std::string("Ausnahme beim Start: ") + ex.what();
        m_impl->cleanupFailedStart(error);
        return false;
    } catch (...) {
        error = "Unbekannte Ausnahme beim Start.";
        m_impl->cleanupFailedStart(error);
        return false;
    }
}

bool RecordingPipeline::submitAudioFrame(const AudioFrame& frame, std::string& error) {
    if (!frame.isValid()) {
        error = "Ungültiger Audioframe.";
        return false;
    }
    if (!m_impl->inputGate.tryEnter()) {
        error = "Pipeline nimmt keine Audioframes an.";
        return false;
    }
    GateTicket ticket(m_impl->inputGate);
    bool ok = false;
    {
        std::lock_guard lock(m_impl->audioMutex);
        if (m_impl->audioEncoder) ok = m_impl->audioEncoder->encode(frame, error);
        else error = "Audioencoder ist nicht verfügbar.";
    }
    if (!ok) {
        if (error.empty()) error = "Audioencoder ist fehlgeschlagen.";
        m_impl->fail(error);
    }
    return ok;
}

bool RecordingPipeline::submitVideoFrame(const VideoFrameInput& frame, std::string& error) {
    if (!frame.isValid()) {
        error = "Ungültiger Video Frame.";
        return false;
    }
    if (!m_impl->inputGate.tryEnter()) {
        error = "Pipeline nimmt keine Videoframes an.";
        return false;
    }
    GateTicket ticket(m_impl->inputGate);
    bool ok = false;
    {
        std::lock_guard lock(m_impl->videoMutex);
        if (m_impl->videoEncoder) ok = m_impl->videoEncoder->encode(frame, error);
        else error = "Videoencoder ist nicht verfügbar.";
    }
    if (!ok) {
        if (error.empty()) error = "Videoencoder ist fehlgeschlagen.";
        m_impl->fail(error);
    }
    return ok;
}

bool RecordingPipeline::stop(RecordingStopMode mode, std::string& error) {
    std::lock_guard lifecycle(m_impl->lifecycleMutex);
    const auto initial = m_impl->currentState();
    if (initial == RecordingState::Idle) return true;
    if (m_impl->cleanupDone) {
        std::lock_guard lock(m_impl->stateMutex);
        error = m_impl->error;
        return m_impl->state != RecordingState::Failed;
    }

    m_impl->inputGate.closeAndWait();

    if (mode == RecordingStopMode::Graceful && !m_impl->hasFailure()) {
        m_impl->setState(RecordingState::Stopping);
        if (m_impl->audioEncoder) {
            error.clear();
            if (!m_impl->audioEncoder->flush(error)) {
                if (error.empty()) error = "Audioencoder-Flush fehlgeschlagen.";
                m_impl->fail(error);
            }
        }
        if (!m_impl->hasFailure() && m_impl->videoEncoder) {
            error.clear();
            if (!m_impl->videoEncoder->flush(error)) {
                if (error.empty()) error = "Videoencoder-Flush fehlgeschlagen.";
                m_impl->fail(error);
            }
        }
    }

    // shutdown() must quiesce encoder-owned work. Keep callback gate open until it returns.
    m_impl->shutdownEncoders();
    if (m_impl->callbacks) m_impl->callbacks->gate.closeAndWait();

    const bool drain = mode == RecordingStopMode::Graceful && !m_impl->hasFailure();
    WorkerResult result;
    if (m_impl->muxerWorker) {
        if (drain) m_impl->setState(RecordingState::Finalizing);
        result = m_impl->muxerWorker->finish(drain);
        if (result.outcome == WorkerOutcome::Failed) {
            m_impl->fail(result.error.empty() ? "MuxerWorker fehlgeschlagen." : result.error);
        } else if (drain && result.outcome != WorkerOutcome::Completed) {
            m_impl->fail("MuxerWorker endete vor der Finalisierung.");
        }
    }

    {
        std::lock_guard lock(m_impl->stateMutex);
        if (m_impl->state == RecordingState::Failed) {
            error = m_impl->error;
        } else if (mode == RecordingStopMode::Abort) {
            m_impl->state = RecordingState::Aborted;
            error.clear();
        } else {
            m_impl->state = RecordingState::Completed;
            error.clear();
        }
    }
    m_impl->cleanupDone = true;
    return m_impl->currentState() != RecordingState::Failed;
}

RecordingPipelineSnapshot RecordingPipeline::snapshot() const {
    RecordingPipelineSnapshot result;
    {
        std::lock_guard lock(m_impl->stateMutex);
        result.state = m_impl->state;
        result.error = m_impl->error;
    }
    result.acceptsInput = m_impl->inputGate.isOpen();
    return result;
}

} // namespace streamforge
