#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>
#include <xaudio2.h>

namespace Core::Uwp {
class XAudio2Device;
void SuspendAudio(bool suspended);

class XAudio2Output final : private IXAudio2VoiceCallback {
public:
    XAudio2Output(uint32_t channels, uint32_t sample_rate, uint32_t frames);
    ~XAudio2Output();
    bool Submit(std::span<const float> samples);
    uint64_t SamplesPlayed() const;
    uint64_t CompletedBuffers() const;

private:
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
    void STDMETHODCALLTYPE OnBufferEnd(void* context) noexcept override;
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT error) noexcept override;
    struct Buffer {
        std::vector<float> samples;
        std::atomic_bool busy{};
    };
    std::shared_ptr<XAudio2Device> device;
    IXAudio2SourceVoice* voice{};
    std::array<Buffer, 8> buffers;
    std::atomic<uint64_t> completed{};
    std::atomic<HRESULT> error{S_OK};
    mutable std::mutex mutex;
};
} // namespace Core::Uwp
