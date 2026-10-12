#pragma once
#include <span>
#include "video_core/texture_cache/image_info.h"
namespace VideoCore {
void ConvertTiling(std::span<const u8> source, std::span<u8> destination, const ImageInfo& info,
                   bool encode);
}
