// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
void uwp_memory_set_log(void (*callback)(const char*));
int uwp_memory_init(uint64_t physical_size, uint64_t direct_size, uintptr_t canonical,
                    const char* swap_path, int swap_enabled);
void uwp_memory_shutdown(void);
int uwp_memory_map(uintptr_t address, uint64_t size, uint64_t physical, int protection,
                   int pageable);
int uwp_memory_unmap(uintptr_t address, uint64_t size);
int uwp_memory_protect(uintptr_t address, uint64_t size, int protection);
int uwp_memory_fault(uintptr_t address, int access, int guest_access, int* failed);
int uwp_memory_acquire(uintptr_t address, uint64_t size, int populate, void** lease);
void uwp_memory_release(void* lease);
void uwp_memory_free_lease(void* lease);
int uwp_memory_copy(void* destination, const void* source, uint64_t size);
int uwp_memory_discard(uint64_t physical, uint64_t size);
uint64_t uwp_memory_reclaim(uint64_t bytes, int urgent);
void uwp_memory_report(void);
#ifdef __cplusplus
}
#endif
