#pragma once
#include <span>
#include <vector>
#include "video_core/texture_cache/image_info.h"
namespace VideoCore {
std::vector<u32> BuildImageAddressTable(const ImageInfo& info);
void ConvertTiling(std::span<const u8> source, std::span<u8> destination, const ImageInfo& info,
                   bool encode);
}
