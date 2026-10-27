// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <string_view>
#include "common/elf_info.h"

namespace Common {

/// Whether the running title is Gravity Rush 2, any release.
inline bool IsGravityRush2() {
    static constexpr std::array<std::string_view, 7> serials = {
        "CUSA03694", "CUSA04943", "CUSA04934", "CUSA00547", "PCJS50010", "PCAS00079", "CUSA04935",
    };
    return std::ranges::contains(serials, ElfInfo::Instance().GameSerial());
}

/// Whether the running title is Gravity Rush Remastered, any release.
inline bool IsGravityRushRemastered() {
    static constexpr std::array<std::string_view, 12> serials = {
        "CUSA01112", "CUSA01113", "CUSA01113P", "CUSA01130", "CUSA02318", "CUSA00546",
        "CUSA02443", "CUSA04246", "PCJS50004",  "PCJS50008", "PCJS66015", "PCJS66029",
    };
    return std::ranges::contains(serials, ElfInfo::Instance().GameSerial());
}

} // namespace Common
