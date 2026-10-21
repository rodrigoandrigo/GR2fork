#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <concepts>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include "common/types.h"

namespace D3D12 {
class Profile {
public:
    Profile() : enabled{std::getenv("GAME_PROFILE") != nullptr} {}
    ~Profile() {
        for (const auto& [name, sample] : samples)
            if (sample.pending)
                Print(name.c_str(), sample);
    }
    void Record(const char* name, u32 interval, u64 elapsed, u64 bytes = 0) noexcept {
        try {
            RecordSample(name, interval, elapsed, bytes);
        } catch (...) {
        }
    }
    void Frame(const char* name) {
        if (!enabled)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (last_frame.time_since_epoch().count())
            Record(name, 128, std::chrono::duration_cast<std::chrono::microseconds>(
                                  now - last_frame).count());
        last_frame = now;
    }
    bool enabled{};

private:
    void RecordSample(const char* name, u32 interval, u64 elapsed, u64 bytes) {
        std::scoped_lock lock{mutex};
        auto found = samples.find(name);
        if (found == samples.end())
            found = samples.try_emplace(name).first;
        auto& sample = found->second;
        ++sample.count;
        ++sample.pending;
        sample.total += elapsed;
        sample.maximum = std::max(sample.maximum, elapsed);
        sample.bytes += bytes;
        sample.slow += elapsed >= 5000;
        ++sample.buckets[std::min<size_t>(std::bit_width(elapsed), 31)];
        if (sample.pending >= std::max(1u, interval)) {
            Print(name, sample);
            sample.pending = 0;
        }
    }
    struct Sample {
        u64 count{}, pending{}, total{}, maximum{}, bytes{}, slow{};
        std::array<u64, 32> buckets{};
    };
    static u64 Percentile(const Sample& sample, u64 percent) {
        const auto target = (sample.count * percent + 99) / 100;
        u64 count{};
        for (size_t index = 0; index < sample.buckets.size(); ++index) {
            count += sample.buckets[index];
            if (count >= target)
                return index == sample.buckets.size() - 1 ? sample.maximum
                                                          : (u64{1} << index) - 1;
        }
        return sample.maximum;
    }
    static void Print(const char* name, const Sample& sample) {
        std::fprintf(stderr,
                     "NATIVE_PROFILE name=%s count=%llu total_us=%llu max_us=%llu "
                     "p50_upper_us=%llu p95_upper_us=%llu p99_upper_us=%llu "
                     "slow_5ms=%llu bytes=%llu\n",
                     name, sample.count, sample.total, sample.maximum,
                     Percentile(sample, 50), Percentile(sample, 95), Percentile(sample, 99),
                     sample.slow, sample.bytes);
    }
    std::chrono::steady_clock::time_point last_frame;
    std::mutex mutex;
    std::map<std::string, Sample, std::less<>> samples;
};

class ProfileScope {
public:
    ProfileScope(Profile& profile, const char* name, u32 interval = 128, u64 bytes = 0)
        : profile{profile}, name{name}, interval{interval}, bytes{bytes} {
        if (profile.enabled)
            start = std::chrono::steady_clock::now();
    }
    ~ProfileScope() {
        if (profile.enabled) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - start).count();
            profile.Record(name, interval, static_cast<u64>(elapsed), bytes);
        }
    }

private:
    Profile& profile;
    const char* name;
    u32 interval;
    u64 bytes;
    std::chrono::steady_clock::time_point start;
};

template <typename Function>
    requires std::invocable<Function>
auto ProfileSample(Profile& profile, const char* name, u32 interval, Function&& function) {
    ProfileScope sample{profile, name, interval};
    return function();
}
} // namespace D3D12
