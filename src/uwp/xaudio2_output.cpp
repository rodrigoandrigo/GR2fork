#include "uwp/xaudio2_output.h"

#include <algorithm>
#include <condition_variable>
#include <future>
#include <stdexcept>
#include <thread>
#include <ks.h>
#include <ksmedia.h>
#include <winrt/base.h>

namespace Core::Uwp {
class XAudio2Device {
public:
    XAudio2Device() {
        std::promise<void> initialized;
        auto ready = initialized.get_future();
        worker = std::jthread(
            [this, initialized = std::move(initialized)](std::stop_token stop) mutable {
                bool apartment{};
                try {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    apartment = true;
                    winrt::check_hresult(XAudio2Create(engine.put(), 0, XAUDIO2_DEFAULT_PROCESSOR));
                    winrt::check_hresult(engine->CreateMasteringVoice(
                        &master, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, nullptr,
                        nullptr, AudioCategory_GameEffects));
                    initialized.set_value();
                    std::unique_lock lock{wait_mutex};
                    wake.wait(lock, stop, [] { return false; });
                } catch (...) {
                    try {
                        initialized.set_exception(std::current_exception());
                    } catch (const std::future_error&) {
                    }
                }
                if (master) {
                    master->DestroyVoice();
                    master = nullptr;
                }
                engine = nullptr;
                if (apartment)
                    winrt::uninit_apartment();
            });
        ready.get();
    }
    ~XAudio2Device() {
        worker.request_stop();
        wake.notify_all();
        worker.join();
    }
    winrt::com_ptr<IXAudio2> engine;
    IXAudio2MasteringVoice* master{};

private:
    std::mutex wait_mutex;
    std::condition_variable_any wake;
    std::jthread worker;
};

namespace {
std::mutex device_mutex;
std::weak_ptr<XAudio2Device> cached_device;
bool audio_suspended{};
std::shared_ptr<XAudio2Device> AcquireDevice() {
    std::scoped_lock lock{device_mutex};
    auto device = cached_device.lock();
    if (!device) {
        device = std::make_shared<XAudio2Device>();
        cached_device = device;
        if (audio_suspended)
            device->engine->StopEngine();
    }
    return device;
}
} // namespace

void SuspendAudio(bool suspended) {
    std::scoped_lock lock{device_mutex};
    if (audio_suspended == suspended)
        return;
    if (const auto device = cached_device.lock()) {
        if (suspended)
            device->engine->StopEngine();
        else
            winrt::check_hresult(device->engine->StartEngine());
    }
    audio_suspended = suspended;
}

XAudio2Output::XAudio2Output(uint32_t channels, uint32_t sample_rate, uint32_t frames) {
    if ((channels != 1 && channels != 2 && channels != 8) || frames == 0 || frames > 65536 ||
        sample_rate < XAUDIO2_MIN_SAMPLE_RATE || sample_rate > XAUDIO2_MAX_SAMPLE_RATE) {
        throw std::invalid_argument("Invalid XAudio2 stream format");
    }
    for (auto& buffer : buffers)
        buffer.samples.resize(static_cast<size_t>(frames) * channels);
    device = AcquireDevice();
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = static_cast<WORD>(channels);
    format.Format.nSamplesPerSec = sample_rate;
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = static_cast<WORD>(channels * sizeof(float));
    format.Format.nAvgBytesPerSec = sample_rate * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    constexpr DWORD stereo = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    constexpr DWORD surround = stereo | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                               SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT | SPEAKER_SIDE_LEFT |
                               SPEAKER_SIDE_RIGHT;
    format.dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER : channels == 2 ? stereo : surround;
    format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    winrt::check_hresult(device->engine->CreateSourceVoice(&voice, &format.Format, 0,
                                                           XAUDIO2_DEFAULT_FREQ_RATIO, this));
    const auto result = voice->Start();
    if (FAILED(result)) {
        voice->DestroyVoice();
        voice = nullptr;
        winrt::throw_hresult(result);
    }
}
XAudio2Output::~XAudio2Output() {
    std::scoped_lock lock{mutex};
    if (voice)
        voice->DestroyVoice();
}
bool XAudio2Output::Submit(std::span<const float> samples) {
    std::scoped_lock lock{mutex};
    winrt::check_hresult(error.load());
    if (samples.size() != buffers.front().samples.size())
        throw std::invalid_argument("Invalid XAudio2 buffer size");
    for (auto& buffer : buffers) {
        bool expected = false;
        if (!buffer.busy.compare_exchange_strong(expected, true))
            continue;
        std::copy(samples.begin(), samples.end(), buffer.samples.begin());
        XAUDIO2_BUFFER packet{};
        packet.AudioBytes = static_cast<UINT32>(samples.size_bytes());
        packet.pAudioData = reinterpret_cast<const BYTE*>(buffer.samples.data());
        packet.pContext = &buffer;
        const auto result = voice->SubmitSourceBuffer(&packet);
        if (FAILED(result)) {
            buffer.busy.store(false);
            winrt::throw_hresult(result);
        }
        return true;
    }
    return false;
}
void XAudio2Output::OnBufferEnd(void* context) noexcept {
    static_cast<Buffer*>(context)->busy.store(false);
    completed.fetch_add(1);
}
void XAudio2Output::OnVoiceError(void*, HRESULT value) noexcept {
    error.store(value);
}
uint64_t XAudio2Output::SamplesPlayed() const {
    std::scoped_lock lock{mutex};
    XAUDIO2_VOICE_STATE state{};
    voice->GetState(&state);
    return state.SamplesPlayed;
}
uint64_t XAudio2Output::CompletedBuffers() const {
    return completed.load();
}
} // namespace Core::Uwp
