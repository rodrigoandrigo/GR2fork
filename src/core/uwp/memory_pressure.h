// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <windows.h>
#include "core/address_space.h"

namespace Core::Uwp {
template <typename Allocate>
HRESULT AllocateMemory(Allocate&& allocate) {
    auto result = allocate();
#ifdef SHADPS4_UWP
    for (unsigned attempt = 0; result == E_OUTOFMEMORY && attempt < 3; ++attempt) {
        if (!AddressSpace::ReclaimMemory(32ULL * 1024 * 1024))
            break;
        result = allocate();
    }
#endif
    return result;
}
} // namespace Core::Uwp
