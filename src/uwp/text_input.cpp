#include "uwp/text_input.h"

#include <chrono>
#include <cwchar>
#include <iterator>
#include <mutex>

namespace {
std::mutex text_mutex;
ShadPS4TextInput current{};
std::optional<Core::Uwp::TextInputResult> result;
uint64_t next_id{static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
} // namespace

namespace Core::Uwp {
void BeginTextInput(ShadPS4TextInput request) {
    std::scoped_lock lock{text_mutex};
    request.size = sizeof(request);
    request.id = ++next_id;
    current = request;
    result.reset();
}

std::optional<TextInputResult> TakeTextInputResult() {
    std::scoped_lock lock{text_mutex};
    auto completed = std::move(result);
    result.reset();
    if (completed) {
        current = {};
    }
    return completed;
}

void ResetTextInput() {
    std::scoped_lock lock{text_mutex};
    current = {};
    result.reset();
}
} // namespace Core::Uwp

int32_t shadps4_get_text_input(ShadPS4TextInput* request) {
    if (!request || request->size != sizeof(*request)) {
        return -1;
    }
    std::scoped_lock lock{text_mutex};
    *request = current;
    request->size = sizeof(*request);
    return 0;
}

int32_t shadps4_complete_text_input(uint64_t id, int32_t confirmed, const wchar_t* text) {
    if (confirmed != 0 && confirmed != 1) {
        return -1;
    }
    std::scoped_lock lock{text_mutex};
    if (!id || id != current.id || result || (confirmed && !text)) {
        return -1;
    }
    const size_t length = confirmed ? wcsnlen(text, std::size(current.text)) : 0;
    if (length > current.max_length || length >= std::size(current.text)) {
        return -1;
    }
    for (size_t i = 0; i < length; ++i) {
        const auto ch = text[i];
        if ((current.flags & SHADPS4_TEXT_NUMBER) && !(ch >= L'0' && ch <= L'9') && ch != L',' &&
            ch != L'-' && ch != L'.') {
            return -1;
        }
        if (ch >= 0xd800 && ch <= 0xdbff) {
            if (++i == length || text[i] < 0xdc00 || text[i] > 0xdfff) {
                return -1;
            }
        } else if (ch >= 0xdc00 && ch <= 0xdfff) {
            return -1;
        }
    }
    std::u16string value;
    if (confirmed) {
        value.assign(text, text + length);
    }
    result = Core::Uwp::TextInputResult{confirmed != 0, std::move(value)};
    return 0;
}
