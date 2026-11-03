#pragma once

#include "core/media/MediaTypes.h"

#include <functional>
#include <string>

namespace streamforge {

using EncodedPacketCallback =
    std::function<bool(EncodedPacket&& packet, std::string& error)>;

} // namespace streamforge
