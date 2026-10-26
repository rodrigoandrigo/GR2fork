#pragma once

#include <atomic>
#include <cstdint>

namespace Core::Uwp {
inline std::atomic<uint64_t> game_frame_count{};
}
