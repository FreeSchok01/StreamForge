#pragma once

#include "core/recording/IMediaMuxer.h"

#include <memory>

namespace streamforge {

// FFmpeg-backed container writer. All methods must be called on one thread;
// RecordingPipeline's MuxerWorker provides that ownership.
class FFmpegMediaMuxer final : public IMediaMuxer {
public:
    FFmpegMediaMuxer();
    ~FFmpegMediaMuxer() override;

    FFmpegMediaMuxer(const FFmpegMediaMuxer&) = delete;
    FFmpegMediaMuxer& operator=(const FFmpegMediaMuxer&) = delete;
    FFmpegMediaMuxer(FFmpegMediaMuxer&&) = delete;
    FFmpegMediaMuxer& operator=(FFmpegMediaMuxer&&) = delete;

    bool configure(const RecordingOutputConfig& config,
                   const std::vector<EncodedStreamDescriptor>& streams,
                   std::string& error) override;
    bool open(std::string& error) override;
    bool writePacket(const EncodedPacket& packet, std::string& error) override;
    bool finalize(std::string& error) override;
    void abort() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

[[nodiscard]] std::unique_ptr<IMediaMuxer> createFFmpegMediaMuxer();

} // namespace streamforge
