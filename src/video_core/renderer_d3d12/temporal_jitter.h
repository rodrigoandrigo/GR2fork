#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <d3d12.h>

namespace D3D12 {
class TemporalJitter {
public:
    struct Config {
        uint64_t target{};
        uint32_t render_width{}, render_height{}, output_width{}, output_height{}, phases{};
        bool operator==(const Config&) const = default;
    };
    struct Sample {
        uint64_t frame{};
        uint64_t generation{};
        uint32_t phase{}, phases{};
        std::array<float, 2> pixels{}, previous_pixels{}, ndc{};
        bool reset{true};
        bool applied{};
    };
    void Configure(Config value) {
        if (value.target &&
            (!value.render_width || !value.render_height || !value.output_width ||
             !value.output_height || value.render_width > 16384 || value.render_height > 16384 ||
             value.output_width > 16384 || value.output_height > 16384 ||
             value.render_width > value.output_width || value.render_height > value.output_height ||
             value.phases > 1024))
            throw std::invalid_argument("Invalid D3D12 temporal jitter configuration");
        if (value.target && !value.phases) {
            const double ratio = double(value.output_width) / value.render_width;
            const double phases = 8 * ratio * ratio;
            if (phases > 1024)
                throw std::invalid_argument("D3D12 temporal jitter phase count exceeds limit");
            value.phases = static_cast<uint32_t>(phases);
        }
        std::scoped_lock lock{mutex};
        if (config == value)
            return;
        config = value;
        ResetLocked();
    }
    void Reset() {
        std::scoped_lock lock{mutex};
        ResetLocked();
    }
    Sample Current() const {
        std::scoped_lock lock{mutex};
        return current;
    }
    Sample LastCompleted() const {
        std::scoped_lock lock{mutex};
        return last_completed;
    }
    D3D12_VIEWPORT Apply(uint64_t target, D3D12_VIEWPORT viewport) {
        std::scoped_lock lock{mutex};
        if (!config.target || target != config.target || viewport.TopLeftX != 0 ||
            viewport.TopLeftY != 0 || viewport.Width != config.render_width ||
            viewport.Height != config.render_height)
            return viewport;
        viewport.TopLeftX += current.pixels[0];
        viewport.TopLeftY += current.pixels[1];
        current.applied = true;
        return viewport;
    }
    Sample CompleteFrame() {
        std::scoped_lock lock{mutex};
        const auto completed = current;
        last_completed = completed;
        if (current.applied) {
            current.previous_pixels = current.pixels;
            ++current.frame;
            current.phase = (current.phase + 1) % config.phases;
            current.reset = false;
            current.applied = false;
            UpdateLocked();
        } else if (config.target) {
            ResetLocked();
        }
        return completed;
    }

private:
    static float Halton(uint32_t index, uint32_t base) {
        float value = 0, weight = 1;
        while (index) {
            weight /= base;
            value += weight * (index % base);
            index /= base;
        }
        return value;
    }
    void UpdateLocked() {
        current.phases = config.phases;
        if (config.target) {
            current.pixels = {Halton(current.phase + 1, 2) - 0.5f,
                              Halton(current.phase + 1, 3) - 0.5f};
            current.ndc = {2 * current.pixels[0] / config.render_width,
                           -2 * current.pixels[1] / config.render_height};
        }
    }
    void ResetLocked() {
        const auto generation = current.generation + 1;
        current = {};
        current.generation = generation;
        UpdateLocked();
        current.previous_pixels = current.pixels;
        last_completed = current;
    }
    mutable std::mutex mutex;
    Config config;
    Sample current;
    Sample last_completed;
};
inline TemporalJitter& NativeTemporalJitter() {
    static TemporalJitter jitter;
    return jitter;
}
} // namespace D3D12
