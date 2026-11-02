// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Ssl2 {

struct OrbisSslData {
    char* ptr;
    u64 size;
};

struct OrbisSslCaCerts {
    OrbisSslData* certs;
    u64 num;
    void* pool;
};

struct OrbisSslMemoryPoolStats {
    u64 pool_size;
    u64 max_in_use_size;
    u64 current_in_use_size;
    s32 reserved;
};

int PS4_SYSV_ABI sceSslInit(std::size_t pool_size);
int PS4_SYSV_ABI sceSslTerm(s32 context_id);
int PS4_SYSV_ABI sceSslGetMemoryPoolStats(s32 context_id, OrbisSslMemoryPoolStats* stats);
int PS4_SYSV_ABI sceSslGetCaCerts(s32 context_id, OrbisSslCaCerts* certs);
int PS4_SYSV_ABI sceSslFreeCaCerts(s32 context_id, OrbisSslCaCerts* certs);
void RegisterLib(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::Ssl2