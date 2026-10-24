#pragma once

#include <winrt/Windows.Gaming.Input.h>
#include "input/controller.h"

namespace Core::Uwp {
Input::State MapGamepadReading(const winrt::Windows::Gaming::Input::GamepadReading& reading);
class GamepadTouchpadMapper {
  public:
    void Apply(const winrt::Windows::Gaming::Input::GamepadReading& reading, Input::State& state);
    void Reset();

  private:
    bool touching{};
    bool blocked_until_release{true};
    u8 next_id{};
    Input::TouchpadEntry touch;
    u64 started_at{};
};
void StartGamepadInput();
void StopGamepadInput();
void CaptureGamepadInput(bool captured);
} // namespace Core::Uwp
