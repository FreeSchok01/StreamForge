#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace streamforge {

struct AudioFrame {
    std::int32_t sampleRate{0};
    std::int32_t channels{0};
    std::int32_t frameCount{0};
    std::vector<float> samples;
    std::int64_t timestampNs{0};

    [[nodiscard]] bool isValid() const noexcept {
        if (sampleRate <= 0 || channels <= 0 || frameCount <= 0) return false;
        const auto frames = static_cast<std::size_t>(frameCount);
        const auto channelCount = static_cast<std::size_t>(channels);
        if (frames > std::numeric_limits<std::size_t>::max() / channelCount) {
            return false;
        }
        return samples.size() == frames * channelCount;
    }
};

} // namespace streamforge
