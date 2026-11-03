// SPDX-License-Identifier: GPL-2.0-or-later
#include <inttypes.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "guest_memory.h"

#define PAGE UINT64_C(16384)
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
static uint64_t physical_size, direct_size;
#define POOL_SIZE direct_size
#define FLEX_SPAN (physical_size - direct_size)
static uintptr_t canonical_base;
static int mapping_pageable;
static INIT_ONCE gate_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION gate;
static __declspec(thread) unsigned gate_depth;
static __declspec(thread) jmp_buf* failure_target;
static char* swap_path;
static unsigned char transfer_page[16384];
static void* temporary_promotion;
static HANDLE temporary_section;
static void (*log_callback)(const char*);
static char log_buffer[2048];

static BOOL CALLBACK initialize_gate(PINIT_ONCE once, void* parameter, void** context) {
    InitializeCriticalSection(&gate);
    return TRUE;
}
static void memory_lock(void) {
    InitOnceExecuteOnce(&gate_once, initialize_gate, NULL, NULL);
    EnterCriticalSection(&gate);
    ++gate_depth;
}
static void memory_unlock(void) {
    DWORD error = GetLastError();
    --gate_depth;
    LeaveCriticalSection(&gate);
    SetLastError(error);
}
#define AcquireSRWLockExclusive(lock) memory_lock()
#define ReleaseSRWLockExclusive(lock) memory_unlock()
#define AcquireSRWLockShared(lock) memory_lock()
#define ReleaseSRWLockShared(lock) memory_unlock()
#define read_lock() memory_lock()
#define read_unlock() memory_unlock()
#define __thread __declspec(thread)
#define MEMORY_BEGIN(failure)                                                                      \
    jmp_buf target;                                                                                \
    jmp_buf* previous_target = failure_target;                                                     \
    unsigned depth = gate_depth;                                                                   \
    memory_lock();                                                                                 \
    failure_target = &target;                                                                      \
    if (setjmp(target)) {                                                                          \
        DWORD error = GetLastError();                                                              \
        if (temporary_promotion) {                                                                 \
            UnmapViewOfFile(temporary_promotion);                                                  \
            temporary_promotion = NULL;                                                            \
        }                                                                                          \
        if (temporary_section) {                                                                   \
            CloseHandle(temporary_section);                                                        \
            temporary_section = NULL;                                                              \
        }                                                                                          \
        while (gate_depth > depth)                                                                 \
            memory_unlock();                                                                       \
        failure_target = previous_target;                                                          \
        SetLastError(error);                                                                       \
        return failure;                                                                            \
    }
#define MEMORY_END()                                                                               \
    do {                                                                                           \
        failure_target = previous_target;                                                          \
        memory_unlock();                                                                           \
    } while (0)

static void uwp_fail(const char* what, uintptr_t address) {
    DWORD error = GetLastError();
    if (!error)
        error = ERROR_NOT_ENOUGH_MEMORY;
    SetLastError(error);
    longjmp(*failure_target, 1);
}
static uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}
static int memory_printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    int result = vsnprintf(log_buffer, sizeof(log_buffer), format, args);
    va_end(args);
    if (log_callback)
        log_callback(log_buffer);
    else
        fputs(log_buffer, stderr);
    return result;
}
#define printf(...) memory_printf(__VA_ARGS__)
#define fprintf(file, ...) memory_printf(__VA_ARGS__)
#define puts(text) memory_printf("%s\n", text)

static void memory_breadcrumb(const char* text) {
    fprintf(stderr, "[UwpMemory] %s\n", text);
}
static void memory_operation(const char* text, uint64_t first, uint64_t second) {}
static void memory_operation_done(void) {}
static const char* memory_environment(const char* name) {
    if (!strcmp(name, "SHADPS4_UWP_SWAP_FILE") && swap_path)
        return swap_path;
    return getenv(name);
}
static int memory_set_swap_path(const char* path) {
    char* replacement = _strdup(path);
    if (!replacement)
        return 0;
    free(swap_path);
    swap_path = replacement;
    return 1;
}
#define getenv memory_environment

typedef struct RuntimeSwapStats {
    uint64_t disk_used, disk_budget, payload, pages, blank_pages;
    uint64_t blank_bytes, raw_bytes, compressed_bytes, device_free, reserve;
    uint64_t atbudget_bytes, write_failed_bytes, copy_out_pages, copy_in_pages;
    uint64_t copy_in_failures, write_failures, zero_paged_bytes;
    uint64_t pool_capacity, pool_committed, pool_peak_committed, pool_pages;
    uint64_t pool_page_bytes, pool_saved, pool_pages_in, pool_refused;
} RuntimeSwapStats;
static void uwp_swap_report(void);
#include "guest_memory.inc"

void uwp_memory_set_log(void (*callback)(const char*)) {
    memory_lock();
    log_callback = callback;
    memory_unlock();
}

int uwp_memory_init(uint64_t bytes, uint64_t direct, uintptr_t canonical, const char* path,
                    int enabled) {
    MEMORY_BEGIN(0);
    if (uwp_physical || !bytes || bytes % PAGE || direct > bytes) {
        SetLastError(ERROR_INVALID_PARAMETER);
        MEMORY_END();
        return 0;
    }
    physical_size = bytes;
    direct_size = direct;
    canonical_base = canonical;
    const char* configured_path = getenv("SHADPS4_UWP_SWAP_FILE");
    if (!memory_set_swap_path(configured_path && *configured_path ? configured_path : path))
        uwp_fail("swap path allocation", 0);
    runtime_memory_reserve();
    const char* swap_flag = getenv("SHADPS4_UWP_SWAP");
    uwp_swap_disabled = !enabled || (swap_flag && *swap_flag == '0');
    uwp_swap_disk_budget = uwp_swap_disk_ceiling = UINT64_C(4096) << 20;
    const char* budget = getenv("SHADPS4_UWP_SWAP_DISK_MB");
    unsigned long long mb = budget ? strtoull(budget, NULL, 10) : 0;
    if (mb >= 64 && mb <= 8192)
        uwp_swap_disk_budget = uwp_swap_disk_ceiling =
            ((mb << 20) < UWP_SWAP_FILE_MAX_BYTES ? (mb << 20) : UWP_SWAP_FILE_MAX_BYTES);
    uwp_swap_reserve = UINT64_C(384) << 20;
    const char* reserve = getenv("SHADPS4_UWP_SWAP_RESERVE_MB");
    if (reserve && *reserve) {
        char* end = NULL;
        mb = strtoull(reserve, &end, 10);
        if (end && end != reserve && !*end && mb <= 4096)
            uwp_swap_reserve = mb << 20;
    }
    if (!uwp_swap_disabled && uwp_swap_open()) {
        uint64_t free_bytes = uwp_swap_free_space();
        if (uwp_swap_free_valid) {
            uint64_t usable = free_bytes > uwp_swap_reserve ? free_bytes - uwp_swap_reserve : 0;
            if (uwp_swap_disk_budget > usable)
                uwp_swap_disk_budget = uwp_swap_disk_ceiling = usable;
        }
    }
    MEMORY_END();
    return 1;
}

int uwp_memory_map(uintptr_t address, uint64_t size, uint64_t physical, int protection,
                   int pageable) {
    MEMORY_BEGIN(0);
    if (physical > physical_size || size > physical_size - physical || address % PAGE ||
        physical % PAGE || size % PAGE) {
        SetLastError(ERROR_INVALID_PARAMETER);
        MEMORY_END();
        return 0;
    }
    mapping_pageable = pageable;
    int result = host_map(address, size, protection, 0, physical) == 0;
    MEMORY_END();
    return result;
}

int uwp_memory_unmap(uintptr_t address, uint64_t size) {
    MEMORY_BEGIN(0);
    for (uint64_t at = 0; at < size; at += PAGE) {
        UwpPage* page = uwp_page(address + at);
        if (!page || !page->mapped)
            continue;
        uint64_t physical = page->physical;
        uwp_detach(page, 1);
        page->isolated = 0;
        if (physical >= direct_size) {
            UwpPhysical* state = &uwp_physical[physical / PAGE];
            state->discard_pending = 1;
            if (!state->leases) {
                UwpPage* canonical = uwp_page(canonical_base + physical);
                if (canonical && canonical->mapped)
                    uwp_detach(canonical, 0);
                if (!state->references)
                    host_discard(physical, PAGE);
            }
        }
    }
    MEMORY_END();
    return 1;
}

int uwp_memory_protect(uintptr_t address, uint64_t size, int protection) {
    MEMORY_BEGIN(0);
    int result = host_protect(address, size, protection) == 0;
    MEMORY_END();
    return result;
}

static UwpPage* canonical_page(uintptr_t address) {
    uint64_t offset = address - canonical_base;
    if (offset >= physical_size)
        return NULL;
    uintptr_t base = address & ~(uintptr_t)(PAGE - 1);
    UwpPage* page = uwp_page(base);
    if (!page || !page->mapped) {
        mapping_pageable = 0;
        if (host_map(base, PAGE, PROT_READ | PROT_WRITE, 0, offset & ~(PAGE - 1)))
            uwp_fail("canonical page map", address);
        page = uwp_page(base);
    }
    return page;
}

int uwp_memory_fault(uintptr_t address, int access, int guest_access, int* failed) {
    *failed = 1;
    MEMORY_BEGIN(0);
    canonical_page(address);
    int result = runtime_memory_handle_commit_fault_at(address, access, guest_access);
    *failed = 0;
    MEMORY_END();
    return result;
}

static int physical_copy(uint64_t offset, void* data, size_t size, int write) {
    UwpPhysical* physical = &uwp_physical[offset / PAGE];
    uintptr_t owner = physical->owner;
    size_t within = (size_t)(offset % PAGE);
    physical->recent = 1;
    if (owner) {
        return write ? uwp_backing_write(owner + within, data, size)
                     : uwp_backing_read(owner + within, data, size);
    }
    if (physical->section) {
        void* view = MapViewOfFile3FromApp(physical->section, GetCurrentProcess(), NULL, 0, PAGE, 0,
                                           PAGE_READWRITE, NULL, 0);
        if (!view)
            return 0;
        if (write)
            memcpy((unsigned char*)view + within, data, size);
        else
            memcpy(data, (unsigned char*)view + within, size);
        UnmapViewOfFile(view);
    } else if (physical->disk) {
        return write ? uwp_swap_write_backing(offset - within, within, data, size)
                     : uwp_swap_read_backing(offset - within, within, data, size);
    } else {
        if (write && !physical->saved)
            physical->saved = calloc(1, PAGE);
        if (write && !physical->saved)
            return 0;
        if (write)
            memcpy(physical->saved + within, data, size);
        else if (physical->saved)
            memcpy(data, physical->saved + within, size);
        else
            memset(data, 0, size);
    }
    return 1;
}

int uwp_memory_copy(void* destination, const void* source, uint64_t size) {
    MEMORY_BEGIN(0);
    uintptr_t dst = (uintptr_t)destination, src = (uintptr_t)source;
    int dst_physical = dst - canonical_base < physical_size;
    int src_physical = src - canonical_base < physical_size;
    if ((!dst_physical && !src_physical) ||
        (dst_physical && size > physical_size - (dst - canonical_base)) ||
        (src_physical && size > physical_size - (src - canonical_base))) {
        SetLastError(ERROR_INVALID_PARAMETER);
        MEMORY_END();
        return 0;
    }
    while (size) {
        size_t bytes = (size_t)(size < PAGE ? size : PAGE);
        if (src_physical && bytes > PAGE - (src - canonical_base) % PAGE)
            bytes = (size_t)(PAGE - (src - canonical_base) % PAGE);
        if (dst_physical && bytes > PAGE - (dst - canonical_base) % PAGE)
            bytes = (size_t)(PAGE - (dst - canonical_base) % PAGE);
        if (src_physical) {
            if (!physical_copy(src - canonical_base, transfer_page, bytes, 0))
                uwp_fail("physical copy read", src);
        } else
            memcpy(transfer_page, (void*)src, bytes);
        if (dst_physical) {
            if (!physical_copy(dst - canonical_base, transfer_page, bytes, 1))
                uwp_fail("physical copy write", dst);
        } else
            memcpy((void*)dst, transfer_page, bytes);
        dst += bytes;
        src += bytes;
        size -= bytes;
    }
    MEMORY_END();
    return 1;
}

int uwp_memory_discard(uint64_t physical, uint64_t size) {
    MEMORY_BEGIN(0);
    if (physical > physical_size || size > physical_size - physical) {
        SetLastError(ERROR_INVALID_PARAMETER);
        MEMORY_END();
        return 0;
    }
    for (uint64_t at = physical; at < physical + size; at += PAGE) {
        UwpPhysical* page = &uwp_physical[at / PAGE];
        page->discard_pending = 1;
        if (page->leases)
            continue;
        UwpPage* canonical = uwp_page(canonical_base + at);
        if (canonical && canonical->mapped)
            uwp_detach(canonical, 0);
        if (!page->references)
            host_discard(at, PAGE);
    }
    MEMORY_END();
    return 1;
}

uint64_t uwp_memory_reclaim(uint64_t bytes, int urgent) {
    MEMORY_BEGIN(0);
    uint64_t result = urgent ? runtime_memory_page_out(bytes) : uwp_swap_reclaim(bytes, 0, 0);
    MEMORY_END();
    return result;
}

void uwp_memory_report(void) {
    MEMORY_BEGIN();
    uwp_memory_report_internal();
    MEMORY_END();
}

void uwp_memory_shutdown(void) {
    MEMORY_BEGIN();
    for (size_t i = 0; i < uwp_count; ++i) {
        UwpPage* page = &uwp_pages[i];
        if (!page->mapped)
            continue;
        if (page->kind == 2) {
            UnmapViewOfFile2(GetCurrentProcess(), (void*)page->address, MEM_PRESERVE_PLACEHOLDER);
        } else if (page->kind == 3) {
            uwp_swap_release_views(page);
        } else if (page->placed) {
            VirtualFree((void*)page->address, PAGE, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
        }
    }
    for (uint64_t i = 0; uwp_physical && i < physical_size / PAGE; ++i) {
        free(uwp_physical[i].saved);
        if (uwp_physical[i].section)
            CloseHandle(uwp_physical[i].section);
    }
    if (uwp_swap_alias)
        UnmapViewOfFile(uwp_swap_alias);
    if (uwp_swap_section)
        CloseHandle(uwp_swap_section);
    if (uwp_swap_file != INVALID_HANDLE_VALUE)
        CloseHandle(uwp_swap_file);
    if (uwp_pool_base)
        VirtualFree(uwp_pool_base, 0, MEM_RELEASE);
    free(uwp_pool_blobs);
    free(uwp_pool_commit_map);
    free(uwp_swap_slot);
    free(uwp_swap_frames);
    free(uwp_physical);
    free(uwp_pages);
    free(swap_path);
    uwp_swap_frame_count = uwp_swap_store_next = uwp_swap_disk_pages = uwp_swap_views = 0;
    uwp_swap_live_count = uwp_swap_live_full = 0;
    uwp_swap_whole_maps = uwp_swap_view_recoveries = uwp_swap_io_retries = 0;
    uwp_swap_slot_count = uwp_swap_cursor = uwp_swap_view_cursor = 0;
    uwp_pool_size = uwp_pool_commit = uwp_pool_pages = uwp_pool_bytes = 0;
    uwp_pool_blob_used = uwp_pool_free_count = 0;
    uwp_pool_unavailable = uwp_pool_off = 0;
    uwp_swap_disk_full = uwp_swap_full_tick = uwp_swap_free_checked_tick = 0;
    uwp_swap_sweep_deadline = 0;
    uwp_physical = NULL;
    uwp_pages = NULL;
    uwp_count = uwp_capacity = 0;
    uwp_pool_base = NULL;
    uwp_pool_blobs = NULL;
    uwp_pool_commit_map = NULL;
    uwp_swap_slot = NULL;
    uwp_swap_frames = uwp_swap_alias = NULL;
    uwp_swap_section = NULL;
    uwp_swap_file = INVALID_HANDLE_VALUE;
    swap_path = NULL;
    log_callback = NULL;
    MEMORY_END();
}

typedef struct MemoryLease {
    size_t count;
    uint64_t physical[];
} MemoryLease;

int uwp_memory_acquire(uintptr_t address, uint64_t size, int populate, void** output) {
    *output = NULL;
    if (size > UINTPTR_MAX - address || size > SIZE_MAX - PAGE) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    size_t count = (size_t)((size + address % PAGE + PAGE - 1) / PAGE);
    if (count > (SIZE_MAX - sizeof(MemoryLease)) / sizeof(uint64_t))
        return 0;
    MemoryLease* lease = malloc(sizeof(*lease) + count * sizeof(uint64_t));
    if (!lease) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    lease->count = 0;
    *output = lease;
    MEMORY_BEGIN(0);
    uintptr_t end = address + size;
    for (uintptr_t at = address & ~(uintptr_t)(PAGE - 1); at < end; at += PAGE) {
        uint64_t physical = at - canonical_base;
        if (physical >= physical_size) {
            UwpPage* page = uwp_page(at);
            if (!page || !page->mapped)
                continue;
            physical = page->physical;
        }
        ++uwp_physical[physical / PAGE].leases;
        lease->physical[lease->count++] = physical;
    }
    if (populate) {
        for (uintptr_t at = address & ~(uintptr_t)(PAGE - 1); at < end; at += PAGE) {
            UwpPage* page = canonical_page(at);
            if (!page)
                page = uwp_page(at);
            if (page && page->mapped)
                uwp_commit(page);
        }
    }
    MEMORY_END();
    return 1;
}

void uwp_memory_release(void* opaque) {
    if (!opaque)
        return;
    MemoryLease* lease = opaque;
    MEMORY_BEGIN();
    for (size_t i = 0; i < lease->count; ++i) {
        UwpPhysical* physical = &uwp_physical[lease->physical[i] / PAGE];
        if (physical->leases)
            --physical->leases;
        if (!physical->leases && physical->discard_pending) {
            UwpPage* canonical = uwp_page(canonical_base + lease->physical[i]);
            if (canonical && canonical->mapped)
                uwp_detach(canonical, 0);
            if (!physical->references)
                host_discard(lease->physical[i], PAGE);
        }
    }
    free(lease);
    MEMORY_END();
}
void uwp_memory_free_lease(void* lease) {
    free(lease);
}
