#pragma once

#include <memory>
#include "common/types.h"

namespace VideoCore {
struct ImageInfo;
struct ResidentFrame {
    std::shared_ptr<void> handle;
    u64 bytes{};
    u32 row_pitch{};
};
} // namespace VideoCore
