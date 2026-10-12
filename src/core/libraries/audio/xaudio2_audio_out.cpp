#include "core/libraries/audio/audioout.h"
#include "core/libraries/audio/audioout_backend.h"

#include <algorithm>
#include <cstring>
#include <winrt/base.h>
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "uwp/xaudio2_output.h"

namespace Libraries::AudioOut {
namespace {
class XAudio2PortBackend final : public PortBackend {
public:
    explicit XAudio2PortBackend(const PortOut& port)
        : format(port.format_info),
          samples(static_cast<size_t>(port.buffer_frames) * format.num_channels),
          output(format.num_channels, port.sample_rate, port.buffer_frames) {
        volumes.fill(ORBIS_AUDIO_OUT_VOLUME_0DB);
    }
    void Output(void* ptr) override {
        if (!ptr || failed)
            return;
        const auto* bytes = static_cast<const unsigned char*>(ptr);
        const float gain = EmulatorSettings.GetVolumeSlider() * 0.01f;
        for (size_t index = 0; index < samples.size(); ++index) {
            const auto channel = index % format.num_channels;
            const auto source = index - channel + format.channel_layout[channel];
            float sample;
            if (format.is_float) {
                std::memcpy(&sample, bytes + source * sizeof(float), sizeof(float));
            } else {
                int16_t value;
                std::memcpy(&value, bytes + source * sizeof(int16_t), sizeof(int16_t));
                sample = value / 32768.0f;
            }
            samples[index] = sample * gain * volumes[format.channel_layout[channel]] / 32768.0f;
        }
        try {
            if (!output.Submit(samples) && ++dropped % 64 == 1)
                LOG_WARNING(Lib_AudioOut, "XAudio2 queue full; dropped {} buffers", dropped);
        } catch (const winrt::hresult_error& error) {
            failed = true;
            LOG_ERROR(Lib_AudioOut, "XAudio2 HRESULT {:#x}: {}",
                      static_cast<uint32_t>(error.code().value), winrt::to_string(error.message()));
        }
    }
    void SetVolume(const std::array<int, 8>& values) override {
        volumes = values;
    }

private:
    AudioFormatInfo format;
    std::vector<float> samples;
    std::array<int, 8> volumes;
    Core::Uwp::XAudio2Output output;
    uint64_t dropped{};
    bool failed{};
};
} // namespace
std::unique_ptr<PortBackend> XAudio2AudioOut::Open(PortOut& port) {
    try {
        return std::make_unique<XAudio2PortBackend>(port);
    } catch (const winrt::hresult_error& error) {
        LOG_ERROR(Lib_AudioOut, "XAudio2 open HRESULT {:#x}: {}",
                  static_cast<uint32_t>(error.code().value), winrt::to_string(error.message()));
        return {};
    }
}
} // namespace Libraries::AudioOut
