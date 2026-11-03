#pragma once

#include "core/media/EncodedStreamDescriptor.h"
#include "core/media/MediaTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace streamforge {

enum class RecordingContainer : std::uint8_t { Matroska, Mp4 };

struct RecordingOutputConfig {
    std::string outputPath;
    RecordingContainer container{RecordingContainer::Matroska};
    bool overwriteExisting{false};
};

class IMediaMuxer {
public:
    virtual ~IMediaMuxer() = default;
    virtual bool configure(
        const RecordingOutputConfig& config,
        const std::vector<EncodedStreamDescriptor>& streams,
        std::string& error) = 0;
    virtual bool open(std::string& error) = 0;
    virtual bool writePacket(const EncodedPacket& packet, std::string& error) = 0;
    virtual bool finalize(std::string& error) = 0;
    virtual void abort() noexcept = 0;
};

} // namespace streamforge
