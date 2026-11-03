#pragma once

#include <cstddef>

namespace streamforge {

struct PacketQueueConfig {
    std::size_t maxPackets{512};
    std::size_t maxBytes{64 * 1024 * 1024};
};

} // namespace streamforge
