#pragma once

#include "EncoderConfig.h"
#include "IEncodedPacketSink.h"
#include "core/media/AudioFrame.h"
#include "core/media/EncodedStreamDescriptor.h"

#include <string>

namespace streamforge {

class IAudioEncoder {
public:
    virtual ~IAudioEncoder() = default;
    virtual bool initialize(std::string& error) = 0;
    virtual bool encode(const AudioFrame& frame, std::string& error) = 0;
    virtual bool flush(std::string& error) = 0;
    virtual bool getStreamDescriptor(
        EncodedStreamDescriptor& descriptor, std::string& error) const = 0;
    virtual void shutdown() noexcept = 0;
};

} // namespace streamforge
