#include "uwp/gamepad_input.h"

#include <algorithm>
#include <cmath>

namespace Core::Uwp {
namespace {
using namespace winrt::Windows::Gaming::Input;
int Stick(double value, bool invert = false) {
    if (!std::isfinite(value))
        return 128;
    value = std::clamp(invert ? -value : value, -1.0, 1.0);
    return value >= 0 ? 128 + static_cast<int>(std::lround(value * 127))
                      : 128 + static_cast<int>(std::lround(value * 128));
}
int Trigger(double value) {
    return std::isfinite(value) ? static_cast<int>(std::lround(std::clamp(value, 0.0, 1.0) * 255))
                                : 0;
}
} // namespace

Input::State MapGamepadReading(const GamepadReading& reading) {
    using Button = Libraries::Pad::OrbisPadButtonDataOffset;
    Input::State result;
    result.connected = true;
    constexpr std::array mapping{
        std::pair{GamepadButtons::A, Button::Cross},
        std::pair{GamepadButtons::B, Button::Circle},
        std::pair{GamepadButtons::X, Button::Square},
        std::pair{GamepadButtons::Y, Button::Triangle},
        std::pair{GamepadButtons::Menu, Button::Options},
        std::pair{GamepadButtons::View, Button::TouchPad},
        std::pair{GamepadButtons::LeftShoulder, Button::L1},
        std::pair{GamepadButtons::RightShoulder, Button::R1},
        std::pair{GamepadButtons::LeftThumbstick, Button::L3},
        std::pair{GamepadButtons::RightThumbstick, Button::R3},
        std::pair{GamepadButtons::DPadUp, Button::Up},
        std::pair{GamepadButtons::DPadDown, Button::Down},
        std::pair{GamepadButtons::DPadLeft, Button::Left},
        std::pair{GamepadButtons::DPadRight, Button::Right},
    };
    for (const auto& [source, target] : mapping)
        if ((reading.Buttons & source) != GamepadButtons::None)
            result.buttonsState |= target;
    result.axes = {Stick(reading.LeftThumbstickX),  Stick(reading.LeftThumbstickY, true),
                   Stick(reading.RightThumbstickX), Stick(reading.RightThumbstickY, true),
                   Trigger(reading.LeftTrigger),    Trigger(reading.RightTrigger)};
    if (result.axes[4])
        result.buttonsState |= Button::L2;
    if (result.axes[5])
        result.buttonsState |= Button::R2;
    return result;
}

} // namespace Core::Uwp
