#pragma once

#include "video_core/presentation_sink.h"

namespace Vulkan {
class Presenter;
std::shared_ptr<VideoCore::PresentationSink> CreatePresentationSink(Presenter& presenter);
} // namespace Vulkan
