#pragma once

#include <winrt/Windows.Gaming.Input.h>
#include "input/controller.h"

namespace Core::Uwp {
Input::State MapGamepadReading(const winrt::Windows::Gaming::Input::GamepadReading& reading);
void StartGamepadInput();
void StopGamepadInput();
void CaptureGamepadInput(bool captured);
} // namespace Core::Uwp
