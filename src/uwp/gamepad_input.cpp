#include "uwp/gamepad_input.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
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
struct SyntheticEvent {
    std::chrono::milliseconds when{};
    std::chrono::milliseconds hold{150};
    Libraries::Pad::OrbisPadButtonDataOffset button{
        Libraries::Pad::OrbisPadButtonDataOffset::None};
};
std::vector<SyntheticEvent> ParseSchedule(const std::string& schedule) {
    static const std::map<std::string_view, Libraries::Pad::OrbisPadButtonDataOffset> names{
        {"l3", Libraries::Pad::OrbisPadButtonDataOffset::L3},
        {"r3", Libraries::Pad::OrbisPadButtonDataOffset::R3},
        {"options", Libraries::Pad::OrbisPadButtonDataOffset::Options},
        {"up", Libraries::Pad::OrbisPadButtonDataOffset::Up},
        {"right", Libraries::Pad::OrbisPadButtonDataOffset::Right},
        {"down", Libraries::Pad::OrbisPadButtonDataOffset::Down},
        {"left", Libraries::Pad::OrbisPadButtonDataOffset::Left},
        {"l2", Libraries::Pad::OrbisPadButtonDataOffset::L2},
        {"r2", Libraries::Pad::OrbisPadButtonDataOffset::R2},
        {"l1", Libraries::Pad::OrbisPadButtonDataOffset::L1},
        {"r1", Libraries::Pad::OrbisPadButtonDataOffset::R1},
        {"triangle", Libraries::Pad::OrbisPadButtonDataOffset::Triangle},
        {"circle", Libraries::Pad::OrbisPadButtonDataOffset::Circle},
        {"cross", Libraries::Pad::OrbisPadButtonDataOffset::Cross},
        {"square", Libraries::Pad::OrbisPadButtonDataOffset::Square},
    };
    std::vector<SyntheticEvent> events;
    for (size_t start = 0; start < schedule.size();) {
        const auto stop = schedule.find(',', start);
        const auto entry = schedule.substr(start, stop == std::string::npos
                                                      ? std::string::npos
                                                      : stop - start);
        const auto at = entry.find('@');
        if (at == std::string::npos)
            throw std::runtime_error("Invalid synthetic pad event");
        const auto name = names.find(entry.substr(0, at));
        if (name == names.end())
            throw std::runtime_error("Unknown synthetic pad button");
        SyntheticEvent event{};
        event.when = std::chrono::milliseconds{std::stoll(entry.substr(at + 1))};
        const auto colon = entry.find(':', at);
        if (colon != std::string::npos)
            event.hold = std::chrono::milliseconds{std::stoll(entry.substr(colon + 1))};
        event.button = name->second;
        events.push_back(event);
        if (stop == std::string::npos)
            break;
        start = stop + 1;
    }
    return events;
}
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
        std::vector<SyntheticEvent> synthetic;
        if (const char* schedule = std::getenv("GAME_SYNTH_PAD"); schedule && *schedule)
            synthetic = ParseSchedule(schedule);
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
                    auto empty = std::ranges::find(slots.begin() + (synthetic.empty() ? 0 : 1),
                                                   slots.end(), nullptr);
                    if (empty != slots.end()) {
                        *empty = pad;
                        pad.Vibration({});
                    }
                }
                for (size_t slot = 0; slot < slots.size(); ++slot) {
                    if (slot == 0 && !synthetic.empty()) {
                        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started_at);
                        Input::State state{};
                        for (const auto& event : synthetic) {
                            if (elapsed >= event.when && elapsed < event.when + event.hold)
                                state.buttonsState |= event.button;
                        }
                        state.connected = true;
                        state.time = std::chrono::duration_cast<std::chrono::microseconds>(
                                         elapsed)
                                         .count();
                        (*controllers)[0]->UpdateNativeState(state);
                        continue;
                    }
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
