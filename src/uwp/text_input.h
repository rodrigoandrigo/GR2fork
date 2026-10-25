#pragma once

#include <optional>
#include <string>
#include "uwp/shadps4_api.h"

namespace Core::Uwp {
struct TextInputResult {
    bool confirmed;
    std::u16string text;
};
void BeginTextInput(ShadPS4TextInput request);
std::optional<TextInputResult> TakeTextInputResult();
void ResetTextInput();
} // namespace Core::Uwp
