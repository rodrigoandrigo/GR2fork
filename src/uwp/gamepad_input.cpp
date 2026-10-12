#include "uwp/gamepad_input.h"

#include <algorithm>
#include <future>
#include <thread>
#include <winrt/Windows.Foundation.Collections.h>
#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/user_settings.h"

namespace Core::Uwp {
namespace {
using namespace winrt::Windows::Gaming::Input;
std::mutex lifecycle_mutex;
std::jthread polling;
std::atomic_bool captured{};
} // namespace

void StartGamepadInput() {
    std::scoped_lock lock{lifecycle_mutex};
    if (polling.joinable())
        return;
    auto* controllers = Common::Singleton<Input::GameControllers>::Instance();
    for (size_t slot = 0; slot < 4; ++slot) {
        auto* user = UserManagement.GetUserByPlayerIndex(static_cast<int>(slot + 1));
        (*controllers)[slot]->user_id = user ? user->user_id : -1;
    }
    captured.store(false);
    std::promise<void> initialized;
    auto ready = initialized.get_future();
    polling = std::jthread([controllers,
                            initialized = std::move(initialized)](std::stop_token stop) mutable {
        bool apartment{};
        bool started{};
        std::array<Gamepad, 4> slots{nullptr, nullptr, nullptr, nullptr};
        std::array<uint16_t, 4> vibration{};
        const auto started_at = std::chrono::steady_clock::now();
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            apartment = true;
            const auto initial_gamepads = Gamepad::Gamepads();
            LOG_INFO(Input, "Windows.Gaming.Input: {} controllers", initial_gamepads.Size());
            initialized.set_value();
            started = true;
            while (!stop.stop_requested()) {
                const auto pads = Gamepad::Gamepads();
                for (size_t slot = 0; slot < slots.size(); ++slot) {
                    if (!slots[slot])
                        continue;
                    const bool present = std::ranges::any_of(
                        pads, [&](const auto& pad) { return pad == slots[slot]; });
                    if (!present) {
                        try {
                            slots[slot].Vibration({});
                        } catch (const winrt::hresult_error&) {
                        }
                        slots[slot] = nullptr;
                        vibration[slot] = 0;
                        (*controllers)[slot]->UpdateNativeState({});
                    }
                }
                for (const auto& pad : pads) {
                    if (std::ranges::any_of(slots, [&](const auto& item) { return item == pad; }))
                        continue;
                    auto empty = std::ranges::find(slots, nullptr);
                    if (empty != slots.end()) {
                        *empty = pad;
                        pad.Vibration({});
                    }
                }
                for (size_t slot = 0; slot < slots.size(); ++slot) {
                    if (!slots[slot])
                        continue;
                    auto* controller = (*controllers)[slot];
                    try {
                        const bool intercept = captured.load();
                        auto state = intercept ? Input::State{}
                                               : MapGamepadReading(slots[slot].GetCurrentReading());
                        state.connected = true;
                        state.time = std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now() - started_at)
                                         .count();
                        if (intercept)
                            state.buttonsState =
                                Libraries::Pad::OrbisPadButtonDataOffset::Intercepted;
                        controller->UpdateNativeState(state);
                        if (intercept)
                            controller->SetVibration(0, 0);
                        const auto requested =
                            intercept ? uint16_t{} : controller->GetNativeVibration();
                        if (requested != vibration[slot]) {
                            GamepadVibration output{};
                            output.LeftMotor = (requested >> 8) / 255.0;
                            output.RightMotor = (requested & 255) / 255.0;
                            slots[slot].Vibration(output);
                            vibration[slot] = requested;
                        }
                    } catch (const winrt::hresult_error& error) {
                        controller->UpdateNativeState({});
                        slots[slot] = nullptr;
                        vibration[slot] = 0;
                        LOG_WARNING(Input, "Windows.Gaming.Input HRESULT {:#x}: {}",
                                    static_cast<uint32_t>(error.code().value),
                                    winrt::to_string(error.message()));
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        } catch (...) {
            if (!started)
                initialized.set_exception(std::current_exception());
            else
                LOG_ERROR(Input, "Windows.Gaming.Input polling stopped unexpectedly");
        }
        for (size_t slot = 0; slot < slots.size(); ++slot) {
            if (slots[slot]) {
                try {
                    slots[slot].Vibration({});
                } catch (const winrt::hresult_error&) {
                }
            }
            (*controllers)[slot]->UpdateNativeState({});
            slots[slot] = nullptr;
        }
        if (apartment)
            winrt::uninit_apartment();
    });
    try {
        ready.get();
    } catch (...) {
        polling.join();
        throw;
    }
}
void StopGamepadInput() {
    std::scoped_lock lock{lifecycle_mutex};
    if (polling.joinable()) {
        polling.request_stop();
        polling.join();
    }
}
void CaptureGamepadInput(bool value) {
    captured.store(value);
}
} // namespace Core::Uwp
