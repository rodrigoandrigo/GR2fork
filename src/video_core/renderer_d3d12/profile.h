#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include "common/types.h"

namespace D3D12 {
class Profile {
public:
    Profile() : enabled{std::getenv("GAME_PROFILE") != nullptr} {}
    bool enabled{};
    s64 total{};
    u32 count{};
};

template <typename Function>
    requires std::invocable<Function>
auto ProfileSample(Profile& profile, const char* name, u32 interval, Function&& function) {
    constexpr u32 slow_us = 5000;
    if (!profile.enabled) {
        return function();
    }
    const auto start = std::chrono::steady_clock::now();
    if constexpr (std::is_void_v<std::invoke_result_t<Function>>) {
        function();
    } else {
        const auto result = function();
        profile.total += std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
        ++profile.count;
        if (profile.total > slow_us) {
            std::fprintf(stderr, "PROFILE %s: %llu us sample\n", name,
                         static_cast<unsigned long long>(profile.total));
        }
        profile.total = 0;
        return result;
    }
    profile.total += std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count();
    ++profile.count;
    if (profile.total > slow_us) {
        std::fprintf(stderr, "PROFILE %s: %llu us sample\n", name,
                     static_cast<unsigned long long>(profile.total));
    }
    profile.total = 0;
}
} // namespace D3D12
