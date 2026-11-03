#pragma once

#include "RecordingPipeline.h"

namespace streamforge {

[[nodiscard]] RecordingPipelineFactories makeFFmpegRecordingPipelineFactories();

} // namespace streamforge
