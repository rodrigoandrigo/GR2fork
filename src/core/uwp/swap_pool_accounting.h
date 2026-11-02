// SPDX-License-Identifier: GPL-2.0-only
// Adapted from Test-project-arena-8ac58bdb-test-project, xbox/platform.

#pragma once
#include <stddef.h>
#include <stdint.h>

#define BB_POOL_PAGE_SIZE UINT64_C(4096)

static inline uint64_t swap_pool_mark_pages(unsigned char* map, uint64_t offset, uint64_t length,
                                            int committed) {
    uint64_t changed = 0;
    const uint64_t first = offset / BB_POOL_PAGE_SIZE;
    const uint64_t count = length / BB_POOL_PAGE_SIZE;
    for (uint64_t i = first; i < first + count; ++i) {
        const unsigned char mask = (unsigned char)(1u << (i & 7u));
        unsigned char* const cell = &map[i >> 3];
        const int old = (*cell & mask) != 0;
        if (old != (committed != 0)) {
            if (committed)
                *cell |= mask;
            else
                *cell &= (unsigned char)~mask;
            changed += BB_POOL_PAGE_SIZE;
        }
    }
    return changed;
}

static inline uint64_t swap_pool_net_saved(uint64_t guest_bytes, uint64_t committed_bytes) {
    return guest_bytes > committed_bytes ? guest_bytes - committed_bytes : 0;
}
