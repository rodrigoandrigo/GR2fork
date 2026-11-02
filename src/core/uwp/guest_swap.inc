// SPDX-License-Identifier: GPL-2.0-only
// Adapted from Test-project-arena-8ac58bdb-test-project, xbox/platform.

#include <assert.h>
#include <fileapifromapp.h>
#include <stdlib.h>
#include <winioctl.h>

static HANDLE uwp_swap_file = INVALID_HANDLE_VALUE, uwp_swap_section;

#include "swap_codec.h"
#include "swap_pool_accounting.h"

static unsigned char *uwp_swap_alias, *uwp_swap_frames;
static uint64_t uwp_swap_frame_count;
static unsigned char uwp_swap_copy[16384];
static uint64_t uwp_swap_out, uwp_swap_in, uwp_swap_disk_pages, uwp_swap_views;
static uint64_t uwp_swap_hash_checks, uwp_swap_hash_errors, uwp_swap_view_evictions;
static uint64_t uwp_swap_view_limit = UINT64_C(64) * 1024 * 1024 / PAGE;

static uint64_t uwp_swap_passes, uwp_swap_dry;

static uint64_t uwp_swap_disk_budget = UINT64_C(768) << 20;

static uint64_t uwp_swap_disk_ceiling = UINT64_C(768) << 20;
static const uint64_t uwp_swap_disk_step = UINT64_C(256) << 20;
static int uwp_swap_disk_growable, uwp_swap_disk_raised;
static uint64_t uwp_swap_disk_full, uwp_swap_write_failures;

static uint64_t uwp_swap_full_tick, uwp_swap_full_rearms;

static uint64_t uwp_swap_urgent_pages, uwp_swap_urgent_sweeps;

#define UWP_SWAP_SLOT_ZERO UINT32_C(0xFFFFFFFF)

#define UWP_SWAP_SLOT_POOL UINT32_C(0x80000000)

#define UWP_SWAP_FILE_MAX_BYTES UINT64_C(0x80000000)
#define UWP_SWAP_RECORD_ALIGN UINT64_C(4096)

static uint64_t uwp_swap_free_checked_tick, uwp_swap_free_bytes;
static int uwp_swap_free_valid;

static wchar_t* uwp_swap_wide_path(const char* utf8);
static uint64_t uwp_swap_reserve = UINT64_C(384) << 20;
static uint64_t uwp_swap_reserve_hits;

static uint64_t uwp_swap_free_space(void) {

    const uint64_t now = GetTickCount64();
    if (uwp_swap_free_checked_tick && now - uwp_swap_free_checked_tick < UINT64_C(1000)) {
        return uwp_swap_free_bytes;
    }
    uwp_swap_free_checked_tick = now;
    uwp_swap_free_bytes = 0;
    uwp_swap_free_valid = 0;
    const char* path = getenv("SHADPS4_UWP_SWAP_FILE");
    if (path && *path) {
        char dir[1024];
        size_t length = strlen(path);
        if (length >= sizeof(dir))
            length = sizeof(dir) - 1;
        memcpy(dir, path, length);
        dir[length] = 0;
        for (size_t i = length; i > 0; --i) {
            if (dir[i - 1] == '\\' || dir[i - 1] == '/') {
                dir[i] = 0;
                break;
            }
        }

        wchar_t* wide = uwp_swap_wide_path(dir);
        ULARGE_INTEGER available;
        if (wide) {
            if (GetDiskFreeSpaceExW(wide, &available, NULL, NULL)) {
                uwp_swap_free_bytes = available.QuadPart;
                uwp_swap_free_valid = 1;
            }
            free(wide);
        }
    }
    return uwp_swap_free_bytes;
}

static uint32_t* uwp_swap_slot;
static size_t uwp_swap_slot_count;
static uint64_t uwp_swap_store_next;
static uint64_t uwp_swap_store_pages, uwp_swap_store_raw, uwp_swap_store_zero;
static uint64_t uwp_swap_store_payload;
static int32_t uwp_swap_lz_table[1 << UWP_LZ_HASH_BITS];
static unsigned char uwp_swap_io[PAGE + 2];

static unsigned char uwp_swap_verify[PAGE];
static uint64_t uwp_swap_codec_fallbacks;

static uint64_t uwp_scan_atbudget_bytes, uwp_scan_write_failed_bytes;

static uint64_t uwp_swap_sweep_deadline, uwp_swap_sweep_timeouts;
static uint64_t uwp_swap_sweep_timeout_ms(void) {
    static uint64_t cached;
    if (!cached) {
        const char* env = getenv("SHADPS4_UWP_SWAP_TIMEOUT_MS");
        const unsigned long long ms = env ? strtoull(env, NULL, 10) : 0;
        cached = (ms >= 250 && ms <= 60000) ? ms : UINT64_C(2000);
    }
    return cached;
}
static uint64_t uwp_swap_copy_out_pages, uwp_swap_copy_in_pages, uwp_swap_copy_in_failures;
static uint64_t uwp_swap_blank_bytes, uwp_swap_raw_bytes, uwp_swap_compressed_bytes;
static uint64_t uwp_scan_pages, uwp_scan_unmapped, uwp_scan_unpageable, uwp_scan_kind,
    uwp_scan_uncommitted, uwp_scan_noview, uwp_scan_pinned, uwp_scan_shared, uwp_scan_section,
    uwp_scan_atbudget, uwp_scan_unreadable, uwp_scan_recent_cleared, uwp_scan_viewguard,
    uwp_scan_write_failed;

static uint64_t uwp_scan_zero_paged, uwp_swap_zero_paged;
static uint64_t uwp_scan_pool_fallback;
static void uwp_swap_scan_reset(void) {
    uwp_scan_pages = uwp_scan_unmapped = uwp_scan_unpageable = uwp_scan_kind = 0;
    uwp_scan_uncommitted = uwp_scan_noview = uwp_scan_pinned = uwp_scan_shared = 0;
    uwp_scan_section = uwp_scan_atbudget = uwp_scan_unreadable = uwp_scan_recent_cleared = 0;
    uwp_scan_viewguard = uwp_scan_write_failed = uwp_scan_zero_paged = uwp_scan_pool_fallback = 0;

    uwp_scan_atbudget_bytes = uwp_scan_write_failed_bytes = 0;
}

static uint64_t uwp_swap_lock_tick, uwp_swap_lock_count;
static const char* uwp_swap_lock_what = "-";
static uint64_t uwp_swap_request_bytes, uwp_swap_request_tick;
static uint64_t uwp_swap_last_request_bytes, uwp_swap_last_request_done;
static size_t uwp_swap_cursor, uwp_swap_view_cursor;
static int uwp_swap_disabled, uwp_swap_started;

static uint64_t uwp_swap_page_out_locked(uint64_t bytes, UwpPage* exclude, int include_views,
                                         int ignore_recent);

#define UWP_SWAP_LIVE_VIEWS (16 * 1024)
static SRWLOCK uwp_swap_view_lock = SRWLOCK_INIT;
static struct {
    uintptr_t begin;
    uintptr_t end;
} uwp_swap_live_views[UWP_SWAP_LIVE_VIEWS];
static size_t uwp_swap_live_count;
static uint64_t uwp_swap_view_guard_rejects;
static int uwp_swap_live_full;

#define UWP_SWAP_GUEST_LIMIT (UINT64_C(1) << 40)

static void uwp_swap_note_view(uintptr_t address, size_t size) {
    AcquireSRWLockExclusive(&uwp_swap_view_lock);
    if (uwp_swap_live_count < UWP_SWAP_LIVE_VIEWS) {
        uwp_swap_live_views[uwp_swap_live_count].begin = address;
        uwp_swap_live_views[uwp_swap_live_count].end = address + size;
        ++uwp_swap_live_count;
    } else if (!uwp_swap_live_full) {
        uwp_swap_live_full = 1;
        printf("UWP swap: live-view registry full (%d windows); view guard fails open\n",
               (int)UWP_SWAP_LIVE_VIEWS);
    }
    ReleaseSRWLockExclusive(&uwp_swap_view_lock);
}

static void uwp_swap_forget_view(uintptr_t address, size_t size) {
    AcquireSRWLockExclusive(&uwp_swap_view_lock);
    for (size_t i = 0; i < uwp_swap_live_count; ++i) {
        if (uwp_swap_live_views[i].begin == address &&
            uwp_swap_live_views[i].end == address + size) {
            uwp_swap_live_views[i] = uwp_swap_live_views[--uwp_swap_live_count];
            break;
        }
    }
    ReleaseSRWLockExclusive(&uwp_swap_view_lock);
}

static int uwp_swap_guest_ok(const void* pointer, size_t size) {
    const uintptr_t begin = (uintptr_t)pointer;
    return begin && size <= UINTPTR_MAX - begin;
}

static int uwp_swap_live_view(const void* pointer, size_t size) {
    if (!uwp_swap_guest_ok(pointer, size)) {
        return 0;
    }
    const uintptr_t begin = (uintptr_t)pointer;
    if (uwp_swap_live_full) {
        return 1;
    }
    const uintptr_t end = begin + size;
    int found = 0;
    AcquireSRWLockShared(&uwp_swap_view_lock);
    for (size_t i = 0; i < uwp_swap_live_count; ++i) {
        if (uwp_swap_live_views[i].begin <= begin && uwp_swap_live_views[i].end >= end) {
            found = 1;
            break;
        }
    }
    ReleaseSRWLockShared(&uwp_swap_view_lock);
    return found;
}

static void* uwp_swap_accept_view(void* view, uintptr_t expected, size_t size, uintptr_t owner) {
    if (view && (uintptr_t)view == expected && uwp_swap_guest_ok(view, size)) {
        uwp_swap_note_view((uintptr_t)view, size);
        return view;
    }
    assert(0 && "swap view mapped at an unexpected address");
    if (uwp_swap_view_guard_rejects < 8) {
        ++uwp_swap_view_guard_rejects;
        printf("UWP swap: refusing view %p (expected %#" PRIxPTR ", owner %#" PRIxPTR
               "), returning null\n",
               view, expected, owner);
    }
    return NULL;
}

static int uwp_swap_live_page(const UwpPage* page) {
    if (!page->view_mask) {
        return 0;
    }
    for (unsigned i = 0; i < 4; ++i) {
        if ((page->view_mask & (1u << i)) &&
            !uwp_swap_live_view((void*)(page->address + i * 4096), 4096)) {
            return 0;
        }
    }
    return 1;
}

static uint64_t uwp_swap_hash(const void* data) {
    const uint64_t* words = data;
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < PAGE / sizeof(*words); ++i) {
        hash = (hash ^ words[i]) * UINT64_C(0x100000001b3);
    }
    return hash;
}

static wchar_t* uwp_swap_wide_path(const char* utf8) {
    if (!utf8 || !*utf8)
        return NULL;
    const int need = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (need <= 0)
        return NULL;
    wchar_t* wide = (wchar_t*)calloc((size_t)need, sizeof(*wide));
    if (!wide)
        return NULL;
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, need)) {
        free(wide);
        return NULL;
    }
    return wide;
}

static int uwp_swap_open_slow(void);

static int uwp_swap_open(void) {
    if (uwp_swap_section)
        return 1;
    if (uwp_swap_disabled)
        return 0;
    return uwp_swap_open_slow();
}

static int uwp_swap_open_slow(void) {
    if (uwp_swap_section)
        return 1;
    if (uwp_swap_disabled)
        return 0;
    const char* path = getenv("SHADPS4_UWP_SWAP_FILE");
    if (!path || !*path)
        return 0;
    wchar_t* wide = uwp_swap_wide_path(path);
    if (!wide)
        return 0;
    CREATEFILE2_EXTENDED_PARAMETERS params = {0};
    params.dwSize = sizeof(params);
    params.dwFileAttributes = FILE_ATTRIBUTE_TEMPORARY;
    params.dwFileFlags = FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_RANDOM_ACCESS;

    uwp_swap_file = CreateFile2FromAppW(wide, GENERIC_READ | GENERIC_WRITE, 0, CREATE_NEW, &params);
    free(wide);
    DWORD done = 0;
    if (uwp_swap_file == INVALID_HANDLE_VALUE ||
        !DeviceIoControl(uwp_swap_file, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &done, NULL)) {
        const DWORD first_error = GetLastError();
        if (uwp_swap_file != INVALID_HANDLE_VALUE)
            CloseHandle(uwp_swap_file);
        uwp_swap_file = INVALID_HANDLE_VALUE;
        const char* fallback_env = getenv("SHADPS4_UWP_SWAP_FALLBACK");
        if (fallback_env && *fallback_env && strcmp(path, fallback_env) != 0) {

            const size_t length = strlen(fallback_env) + 1;
            char* fallback = (char*)malloc(length);
            if (fallback) {
                memcpy(fallback, fallback_env, length);
                wchar_t* fallback_wide = uwp_swap_wide_path(fallback);
                if (fallback_wide) {
                    uwp_swap_file = CreateFile2FromAppW(fallback_wide, GENERIC_READ | GENERIC_WRITE,
                                                        0, CREATE_NEW, &params);
                    if (uwp_swap_file != INVALID_HANDLE_VALUE &&
                        !DeviceIoControl(uwp_swap_file, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &done,
                                         NULL)) {
                        CloseHandle(uwp_swap_file);
                        uwp_swap_file = INVALID_HANDLE_VALUE;
                    }
                    if (uwp_swap_file != INVALID_HANDLE_VALUE) {
                        if (!memory_set_swap_path(fallback))
                            uwp_fail("swap fallback path", 0);
                        uwp_swap_free_checked_tick = 0;
                        printf("UWP swap: selected file unavailable (error %lu); using LocalState "
                               "fallback %s\n",
                               first_error, fallback);
                    }
                    free(fallback_wide);
                }
                free(fallback);
            }
        }
        if (uwp_swap_file == INVALID_HANDLE_VALUE) {
            SetLastError(first_error);
            goto failed;
        }
    }
    uwp_swap_section = CreateFileMappingFromApp(
        INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_RESERVE, uwp_swap_view_limit * PAGE, NULL);
    if (!uwp_swap_section)
        goto failed;
    uwp_swap_alias = MapViewOfFile3FromApp(uwp_swap_section, GetCurrentProcess(), NULL, 0,
                                           uwp_swap_view_limit * PAGE, 0, PAGE_READWRITE, NULL, 0);
    if (!uwp_swap_alias)
        goto failed;
    uwp_swap_frames = calloc((size_t)uwp_swap_view_limit, 1);
    if (!uwp_swap_frames)
        goto failed;

    uwp_swap_slot_count = (size_t)((POOL_SIZE + FLEX_SPAN) / PAGE);
    uwp_swap_slot = calloc(uwp_swap_slot_count, sizeof(*uwp_swap_slot));
    if (!uwp_swap_slot)
        goto failed;
    uwp_swap_store_next = 0;
    printf("UWP swap: compressed sparse backing ready; %" PRIu64 " MiB mapped-window limit, %zu "
           "page records\n",
           (uwp_swap_view_limit * PAGE) >> 20, uwp_swap_slot_count);
    return 1;
failed:
    printf("UWP swap: unavailable, error %lu\n", GetLastError());
    if (uwp_swap_alias)
        UnmapViewOfFile(uwp_swap_alias);
    if (uwp_swap_section)
        CloseHandle(uwp_swap_section);
    free(uwp_swap_frames);
    free(uwp_swap_slot);
    uwp_swap_slot = NULL;
    uwp_swap_alias = uwp_swap_frames = NULL;
    uwp_swap_section = NULL;
    if (uwp_swap_file != INVALID_HANDLE_VALUE)
        CloseHandle(uwp_swap_file);
    uwp_swap_file = INVALID_HANDLE_VALUE;
    uwp_swap_disabled = 1;
    return 0;
}

static DWORD uwp_swap_transfer_error;

static int uwp_swap_io_at(uint64_t offset, void* data, size_t size, int write) {
    OVERLAPPED at = {0};
    DWORD done = 0;
    at.Offset = (DWORD)offset;
    at.OffsetHigh = (DWORD)(offset >> 32);
    BOOL ok = write ? WriteFile(uwp_swap_file, data, (DWORD)size, &done, &at)
                    : ReadFile(uwp_swap_file, data, (DWORD)size, &done, &at);
    if (!ok || done != size) {
        uwp_swap_transfer_error = GetLastError();
        return 0;
    }
    return 1;
}

static int uwp_swap_flag_on(const char* name) {
    char buffer[8];
    const DWORD length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    return !(length == 1 && buffer[0] == '0');
}

static int uwp_swap_blank_enabled(void) {
    static int cached = -1;
    if (cached < 0)
        cached = uwp_swap_flag_on("SHADPS4_UWP_SWAP_BLANK");
    return cached;
}

static int uwp_swap_compress_enabled(void) {
    static int cached = -1;
    if (cached < 0)
        cached = uwp_swap_flag_on("SHADPS4_UWP_SWAP_COMPRESS");
    return cached;
}

static int uwp_swap_blank(const void* data) {
    const uint64_t* words = data;
    for (size_t i = 0; i < PAGE / sizeof(*words); ++i) {
        if (words[i] != 0)
            return 0;
    }
    return 1;
}

static size_t uwp_swap_encode(const void* data, int* raw) {
    return uwp_swap_encode_page((const unsigned char*)data, PAGE, uwp_swap_io, sizeof(uwp_swap_io),
                                uwp_swap_lz_table, raw);
}

static int uwp_swap_verify_payloads(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* env = getenv("SHADPS4_UWP_SWAP_VERIFY");
        cached = !(env && *env == '0');
    }
    return cached;
}

#define UWP_POOL_BLOB_MAX 32768u
#define UWP_POOL_EXTENT_MAX 1024u
#define UWP_POOL_ALIGN UINT64_C(4096)

typedef struct {
    uint64_t offset;
    uint32_t length;
} UwpPoolBlob;

typedef struct {
    uint64_t offset, length;
} UwpPoolExtent;

static unsigned char* uwp_pool_base;

static unsigned char* uwp_pool_commit_map;
static uint64_t uwp_pool_size, uwp_pool_commit;
static UwpPoolBlob* uwp_pool_blobs;
static size_t uwp_pool_blob_used;
static UwpPoolExtent uwp_pool_free[UWP_POOL_EXTENT_MAX];
static size_t uwp_pool_free_count;
static uint64_t uwp_pool_pages, uwp_pool_bytes, uwp_pool_pages_in, uwp_pool_refused;
static uint64_t uwp_pool_decommit_failures, uwp_pool_peak_commit, uwp_pool_decommitted;
static DWORD uwp_pool_last_decommit_error;
static int uwp_pool_off, uwp_pool_unavailable;

static uint64_t uwp_pool_limit(void) {
    static int cached = -1;
    static uint64_t bytes;
    if (cached < 0) {
        bytes = UINT64_C(192) << 20;
        const char* env = getenv("SHADPS4_UWP_SWAP_POOL_MB");
        if (env && *env) {
            const unsigned long long mb = strtoull(env, NULL, 10);
            bytes = (mb <= 2048) ? (uint64_t)mb << 20 : UINT64_C(2048) << 20;
        }
        cached = 1;
    }
    return bytes;
}

static int uwp_pool_init(void) {
    if (uwp_pool_blobs)
        return 1;
    if (uwp_pool_unavailable)
        return 0;
    const uint64_t limit = uwp_pool_limit();
    if (limit < (UINT64_C(16) << 20)) {
        uwp_pool_off = 1;
        return 0;
    }
    uwp_pool_blobs = calloc(UWP_POOL_BLOB_MAX, sizeof(*uwp_pool_blobs));
    if (!uwp_pool_blobs) {
        uwp_pool_unavailable = 1;
        return 0;
    }
    uwp_pool_base = VirtualAllocFromApp(NULL, (SIZE_T)limit, MEM_RESERVE, PAGE_NOACCESS);
    if (!uwp_pool_base) {
        free(uwp_pool_blobs);
        uwp_pool_blobs = NULL;
        uwp_pool_unavailable = 1;
        printf("UWP swap pool: reservation of %" PRIu64 " MiB failed (error %lu); disk only\n",
               limit >> 20, GetLastError());
        return 0;
    }
    uwp_pool_commit_map = calloc((size_t)((limit / UWP_POOL_ALIGN + 7) / 8), 1);
    if (!uwp_pool_commit_map) {
        VirtualFree(uwp_pool_base, 0, MEM_RELEASE);
        uwp_pool_base = NULL;
        free(uwp_pool_blobs);
        uwp_pool_blobs = NULL;
        uwp_pool_unavailable = 1;
        puts("UWP swap pool: commit accounting unavailable; disk only");
        return 0;
    }
    uwp_pool_size = limit;
    uwp_pool_free_count = 1;
    uwp_pool_free[0].offset = 0;
    uwp_pool_free[0].length = limit;
    printf("UWP swap pool: %" PRIu64 " MiB reserved for pages the disk cannot take "
           "(SHADPS4_UWP_SWAP_POOL_MB, 0 disables)\n",
           limit >> 20);
    return 1;
}

static void uwp_pool_insert_extent(uint64_t offset, uint64_t length);

static uint32_t uwp_pool_alloc(size_t need) {
    if (need == 0)
        return UINT32_C(0);
    const uint64_t want = (need + UWP_POOL_ALIGN - 1) / UWP_POOL_ALIGN * UWP_POOL_ALIGN;
    if (want > uwp_pool_size)
        return UINT32_C(0);

    size_t index = 1;
    while (index < UWP_POOL_BLOB_MAX && uwp_pool_blobs[index].length)
        ++index;
    if (index >= UWP_POOL_BLOB_MAX) {
        ++uwp_pool_refused;
        return UINT32_C(0);
    }
    for (size_t i = 0; i < uwp_pool_free_count; ++i) {
        if (uwp_pool_free[i].length < want)
            continue;
        const uint64_t offset = uwp_pool_free[i].offset;
        if (uwp_pool_free[i].length == want) {
            memmove(&uwp_pool_free[i], &uwp_pool_free[i + 1],
                    (uwp_pool_free_count - i - 1) * sizeof(*uwp_pool_free));
            --uwp_pool_free_count;
        } else {
            uwp_pool_free[i].offset += want;
            uwp_pool_free[i].length -= want;
        }

        if (!VirtualAllocFromApp(uwp_pool_base + offset, (SIZE_T)want, MEM_COMMIT,
                                 PAGE_READWRITE)) {

            uwp_pool_insert_extent(offset, want);
            ++uwp_pool_refused;
            return UINT32_C(0);
        }
        uwp_pool_commit += swap_pool_mark_pages(uwp_pool_commit_map, offset, want, 1);
        if (uwp_pool_commit > uwp_pool_peak_commit)
            uwp_pool_peak_commit = uwp_pool_commit;
        uwp_pool_blobs[index].offset = offset;
        uwp_pool_blobs[index].length = (uint32_t)want;
        if (index + 1 > uwp_pool_blob_used)
            uwp_pool_blob_used = index + 1;
        return (uint32_t)index;
    }
    return UINT32_C(0);
}

static void uwp_pool_insert_extent(uint64_t offset, uint64_t length) {
    size_t at = 0;
    while (at < uwp_pool_free_count && uwp_pool_free[at].offset < offset)
        ++at;

    if (at > 0 && uwp_pool_free[at - 1].offset + uwp_pool_free[at - 1].length == offset) {
        uwp_pool_free[at - 1].length += length;
        if (at < uwp_pool_free_count &&
            uwp_pool_free[at - 1].offset + uwp_pool_free[at - 1].length ==
                uwp_pool_free[at].offset) {
            uwp_pool_free[at - 1].length += uwp_pool_free[at].length;
            memmove(&uwp_pool_free[at], &uwp_pool_free[at + 1],
                    (uwp_pool_free_count - at - 1) * sizeof(*uwp_pool_free));
            --uwp_pool_free_count;
        }
        return;
    }
    if (at < uwp_pool_free_count && offset + length == uwp_pool_free[at].offset) {
        uwp_pool_free[at].offset = offset;
        uwp_pool_free[at].length += length;
        return;
    }
    if (uwp_pool_free_count == UWP_POOL_EXTENT_MAX) {
        static uint64_t lost_bytes;
        lost_bytes += length;
        if (lost_bytes == length) {
            printf("UWP swap pool: free-extent table full; some reservation cannot be reused\n");
        }
        return;
    }
    memmove(&uwp_pool_free[at + 1], &uwp_pool_free[at],
            (uwp_pool_free_count - at) * sizeof(*uwp_pool_free));
    uwp_pool_free[at].offset = offset;
    uwp_pool_free[at].length = length;
    ++uwp_pool_free_count;
}

static void uwp_pool_free_blob(uint32_t index) {
    if (index == 0 || index >= UWP_POOL_BLOB_MAX || !uwp_pool_blobs[index].length)
        return;
    const uint64_t offset = uwp_pool_blobs[index].offset;
    const uint64_t length = uwp_pool_blobs[index].length;
    uwp_pool_blobs[index].offset = 0;
    uwp_pool_blobs[index].length = 0;

    static int decommit_enabled = -1;
    if (decommit_enabled < 0) {
        const char* flag = getenv("SHADPS4_UWP_POOL_DECOMMIT");
        decommit_enabled = !(flag && flag[0] == '0');
    }
    if (decommit_enabled) {
        if (VirtualFree(uwp_pool_base + offset, (SIZE_T)length, MEM_DECOMMIT)) {
            const uint64_t actual = swap_pool_mark_pages(uwp_pool_commit_map, offset, length, 0);
            uwp_pool_commit -= actual;
            uwp_pool_decommitted += actual;
        } else {
            uwp_pool_last_decommit_error = GetLastError();
            ++uwp_pool_decommit_failures;
            if (uwp_pool_decommit_failures <= 4) {
                printf("UWP swap pool: decommit failed (error %lu, offset=%" PRIu64
                       ", bytes=%" PRIu64 "); still committed and tracked\n",
                       uwp_pool_last_decommit_error, offset, length);
            }
        }
    }
    if (uwp_pool_pages)
        --uwp_pool_pages;
    uwp_pool_insert_extent(offset, length);
    uwp_pool_bytes = uwp_pool_bytes > length ? uwp_pool_bytes - length : 0;
}

static uint32_t uwp_pool_store(size_t total) {
    if (uwp_pool_off || !uwp_pool_init())
        return 0;
    const uint32_t index = uwp_pool_alloc(total);
    if (index == 0) {
        ++uwp_pool_refused;
        return 0;
    }
    memcpy(uwp_pool_base + uwp_pool_blobs[index].offset, uwp_swap_io, total);
    uwp_pool_bytes += uwp_pool_blobs[index].length;
    ++uwp_pool_pages;
    return UWP_SWAP_SLOT_POOL | index;
}

static int uwp_pool_load(uint32_t index, void* data) {
    if (index == 0 || index >= UWP_POOL_BLOB_MAX || !uwp_pool_blobs[index].length) {
        uwp_swap_transfer_error = ERROR_NOT_FOUND;
        return 0;
    }
    const unsigned char* blob = uwp_pool_base + uwp_pool_blobs[index].offset;
    const size_t stored = uwp_pool_blobs[index].length;

    const size_t length = (size_t)blob[0] | ((size_t)blob[1] << 8);
    if (length == 0 || length > stored - 2) {
        uwp_swap_transfer_error = ERROR_INVALID_DATA;
        return 0;
    }
    if (length == PAGE) {
        memcpy(data, blob + 2, PAGE);
    } else if (uwp_lz_decompress(blob + 2, length, data, PAGE) != PAGE) {
        uwp_swap_transfer_error = ERROR_INVALID_DATA;
        return 0;
    }
    ++uwp_pool_pages_in;
    return 1;
}

static int uwp_pool_slot(uint32_t slot) {
    return slot != UWP_SWAP_SLOT_ZERO && (slot & UWP_SWAP_SLOT_POOL) != 0;
}

static int uwp_pool_available(void) {
    if (uwp_pool_off)
        return 0;
    if (!uwp_pool_blobs)
        return !uwp_pool_unavailable && uwp_pool_limit() >= (UINT64_C(16) << 20);
    for (size_t i = 0; i < uwp_pool_free_count; ++i) {
        if (uwp_pool_free[i].length >= UWP_POOL_ALIGN)
            return 1;
    }
    return 0;
}

static int uwp_swap_store_pool_locked(uint64_t physical, size_t total) {
    const size_t key = (size_t)(physical / PAGE);
    if (!uwp_swap_slot || key >= uwp_swap_slot_count)
        return 0;
    const uint32_t pooled = uwp_pool_store(total);
    if (!pooled)
        return 0;
    const uint32_t previous = uwp_swap_slot[key];
    if (uwp_pool_slot(previous))
        uwp_pool_free_blob(previous & ~UWP_SWAP_SLOT_POOL);
    uwp_swap_slot[key] = pooled;
    ++uwp_swap_store_pages;
    return 1;
}

static SRWLOCK uwp_swap_io_lock = SRWLOCK_INIT;

static int uwp_swap_transfer_locked(uint64_t physical, void* data, int write) {
    const size_t key = (size_t)(physical / PAGE);
    if (!uwp_swap_slot || key >= uwp_swap_slot_count) {
        uwp_swap_transfer_error = ERROR_INVALID_PARAMETER;
        return 0;
    }
    if (!write) {
        const uint32_t slot = uwp_swap_slot[key];
        if (slot == 0) {
            uwp_swap_transfer_error = ERROR_NOT_FOUND;
            return 0;
        }
        if (slot == UWP_SWAP_SLOT_ZERO) {
            memset(data, 0, PAGE);
            return 1;
        }
        if (uwp_pool_slot(slot)) {
            return uwp_pool_load(slot & ~UWP_SWAP_SLOT_POOL, data);
        }
        if (!uwp_swap_io_at((uint64_t)slot - 1, uwp_swap_io, 2, 0))
            return 0;
        const size_t length = (size_t)uwp_swap_io[0] | ((size_t)uwp_swap_io[1] << 8);
        if (length == 0 || length > PAGE) {
            uwp_swap_transfer_error = ERROR_INVALID_DATA;
            return 0;
        }
        if (!uwp_swap_io_at((uint64_t)slot - 1 + 2, uwp_swap_io + 2, length, 0))
            return 0;
        if (length == PAGE) {
            memcpy(data, uwp_swap_io + 2, PAGE);
            return 1;
        }
        if (uwp_lz_decompress(uwp_swap_io + 2, length, data, PAGE) != PAGE) {

            uwp_swap_transfer_error = ERROR_INVALID_DATA;
            return 0;
        }
        return 1;
    }
    if (uwp_swap_blank_enabled() && uwp_swap_blank(data)) {
        if (uwp_swap_slot[key] != UWP_SWAP_SLOT_ZERO)
            ++uwp_swap_store_zero;
        ++uwp_swap_store_pages;
        uwp_swap_blank_bytes += PAGE;
        if (uwp_pool_slot(uwp_swap_slot[key]))
            uwp_pool_free_blob(uwp_swap_slot[key] & ~UWP_SWAP_SLOT_POOL);
        uwp_swap_slot[key] = UWP_SWAP_SLOT_ZERO;
        return 1;
    }
    int raw = 0;
    size_t total;
    if (!uwp_swap_compress_enabled()) {

        memcpy(uwp_swap_io + 2, data, PAGE);
        uwp_swap_io[0] = (unsigned char)(PAGE & 0xFF);
        uwp_swap_io[1] = (unsigned char)((PAGE >> 8) & 0xFF);
        total = PAGE + 2;
        raw = 1;
    } else {
        total = uwp_swap_encode(data, &raw);
    }
    if (!raw && uwp_swap_verify_payloads()) {
        if (uwp_lz_decompress(uwp_swap_io + 2, total - 2, uwp_swap_verify, PAGE) != PAGE ||
            memcmp(uwp_swap_verify, data, PAGE) != 0) {
            ++uwp_swap_codec_fallbacks;
            memcpy(uwp_swap_io + 2, data, PAGE);
            uwp_swap_io[0] = (unsigned char)(PAGE & 0xFF);
            uwp_swap_io[1] = (unsigned char)((PAGE >> 8) & 0xFF);
            total = PAGE + 2;
            raw = 1;
        }
    }
    if (raw) {
        ++uwp_swap_store_raw;
        uwp_swap_raw_bytes += PAGE;
    } else {
        uwp_swap_compressed_bytes += PAGE;
    }
    uwp_swap_store_payload += total - 2;
    uint64_t offset = 0;
    uint64_t new_space = 0;
    const uint32_t slot = uwp_swap_slot[key];
    if (slot != 0 && slot != UWP_SWAP_SLOT_ZERO && !uwp_pool_slot(slot)) {

        unsigned char header[2];
        if (uwp_swap_io_at((uint64_t)slot - 1, header, sizeof(header), 0)) {
            const size_t stored = (size_t)header[0] | ((size_t)header[1] << 8);
            if (stored + 2 >= total) {
                offset = (uint64_t)slot - 1;
            }
        }
    }
    if (offset == 0) {
        const uint64_t need =
            (total + UWP_SWAP_RECORD_ALIGN - 1) / UWP_SWAP_RECORD_ALIGN * UWP_SWAP_RECORD_ALIGN;
        const uint64_t free_bytes = uwp_swap_free_space();
        if (uwp_swap_free_valid && free_bytes < need + uwp_swap_reserve) {

            ++uwp_swap_reserve_hits;
            uwp_swap_transfer_error = ERROR_DISK_FULL;
            if (uwp_swap_store_pool_locked(physical, total))
                return 1;
            return 0;
        }
        if (uwp_swap_store_next + need > uwp_swap_disk_budget) {

            uwp_swap_transfer_error = ERROR_DISK_QUOTA_EXCEEDED;
            if (uwp_swap_store_pool_locked(physical, total))
                return 1;
            return 0;
        }
        if (uwp_swap_store_next + need > UWP_SWAP_FILE_MAX_BYTES) {

            uwp_swap_transfer_error = ERROR_DISK_QUOTA_EXCEEDED;
            return 0;
        }
        offset = uwp_swap_store_next;
        uwp_swap_store_next += need;
        new_space = need;
    }
    if (offset + total > UINT32_MAX) {
        uwp_swap_transfer_error = ERROR_DISK_QUOTA_EXCEEDED;
        return 0;
    }
    if (!uwp_swap_io_at(offset, uwp_swap_io, total, 1)) {
        if (new_space)
            uwp_swap_store_next -= new_space;
        ++uwp_swap_write_failures;
        return uwp_swap_store_pool_locked(physical, total);
    }

    if (uwp_swap_free_valid && new_space) {
        uwp_swap_free_bytes = uwp_swap_free_bytes > new_space ? uwp_swap_free_bytes - new_space : 0;
    }

    if (uwp_pool_slot(slot))
        uwp_pool_free_blob(slot & ~UWP_SWAP_SLOT_POOL);
    uwp_swap_slot[key] = (uint32_t)offset + 1;
    ++uwp_swap_store_pages;
    return 1;
}

static void uwp_swap_describe_corruption(uint64_t physical, uint64_t expected, uint64_t actual) {
    const size_t key = (size_t)(physical / PAGE);
    const uint32_t slot = (uwp_swap_slot && key < uwp_swap_slot_count) ? uwp_swap_slot[key] : 0;
    size_t length = 0;
    if (slot != 0 && slot != UWP_SWAP_SLOT_ZERO && uwp_swap_file != INVALID_HANDLE_VALUE) {
        unsigned char header[2];
        if (uwp_swap_io_at((uint64_t)slot - 1, header, sizeof(header), 0)) {
            length = (size_t)header[0] | ((size_t)header[1] << 8);
        }
    }
    fprintf(stderr,
            "UWP swap: corrupt record physical=%" PRIu64 " key=%zu slot=%u length=%zu (%s)"
            " stored=%" PRIu64 " pages codec fallbacks=%" PRIu64 " write failures=%" PRIu64
            " expected=%016" PRIx64 " read=%016" PRIx64 "\n",
            physical, key, slot, length,
            slot == UWP_SWAP_SLOT_ZERO ? "blank page"
            : length == PAGE           ? "raw"
            : length == 0              ? "missing"
                                       : "compressed",
            uwp_swap_store_pages, uwp_swap_codec_fallbacks, uwp_swap_write_failures, expected,
            actual);
    fflush(stderr);
}

static void uwp_swap_drop_pool_record(uint64_t physical) {
    const size_t key = (size_t)(physical / PAGE);
    if (!uwp_swap_slot || key >= uwp_swap_slot_count)
        return;
    AcquireSRWLockExclusive(&uwp_swap_io_lock);
    const uint32_t slot = uwp_swap_slot[key];
    if (uwp_pool_slot(slot)) {
        uwp_pool_free_blob(slot & ~UWP_SWAP_SLOT_POOL);
        uwp_swap_slot[key] = 0;
    }
    ReleaseSRWLockExclusive(&uwp_swap_io_lock);
}

static int uwp_swap_transfer(uint64_t physical, void* data, int write) {
    AcquireSRWLockExclusive(&uwp_swap_io_lock);
    const int ok = uwp_swap_transfer_locked(physical, data, write);
    ReleaseSRWLockExclusive(&uwp_swap_io_lock);

    if (write) {
        ++uwp_swap_copy_out_pages;
    } else if (ok) {
        ++uwp_swap_copy_in_pages;
    } else {
        ++uwp_swap_copy_in_failures;
    }
    return ok;
}

static void uwp_swap_split(UwpPage* page) {
    if (page->split)
        return;
    for (unsigned i = 0; i < 3; ++i) {
        if (!VirtualFree((void*)(page->address + i * 4096), 4096,
                         MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            uwp_fail("swap placeholder split", page->address);
        }
    }
    page->split = 1;
}

static void uwp_swap_coalesce(UwpPage* page) {
    if (!page->split)
        return;
    if (!VirtualFree((void*)page->address, PAGE, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
        uwp_fail("swap placeholder coalesce", page->address);
    }
    page->split = 0;
}

static int uwp_swap_unmap_view(UwpPage* page) {
    UwpPhysical* physical = &uwp_physical[page->physical / PAGE];
    if (physical->frame) {
        DWORD old;
        for (unsigned i = 0; i < 4; ++i) {
            if ((page->view_mask & (1u << i)) && (page->protection[i] & PROT_WRITE)) {
                if (!VirtualProtectFromApp((void*)(page->address + i * 4096), 4096, PAGE_READONLY,
                                           &old)) {
                    uwp_fail("swap frame write protection", page->address);
                }
            }
        }
        unsigned char* data = uwp_swap_alias + (physical->frame - 1) * PAGE;
        physical->hash = uwp_swap_hash(data);
        physical->hash_valid = 1;
        if (!uwp_swap_transfer(page->physical, data, 1)) {

            if (uwp_swap_transfer_error == ERROR_DISK_QUOTA_EXCEEDED) {
                uwp_apply_protection(page);
                return 0;
            }
            ++uwp_swap_write_failures;
            if (uwp_swap_transfer_error == ERROR_DISK_FULL) {
                uwp_swap_disk_full = 1;
                uwp_swap_full_tick = GetTickCount64();
            }
            uwp_apply_protection(page);
            return 0;
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (page->view_mask & (1u << i)) {
            if (!UnmapViewOfFile2(GetCurrentProcess(), (void*)(page->address + i * 4096),
                                  MEM_PRESERVE_PLACEHOLDER)) {
                uwp_fail("swap view release", page->address);
            }
            uwp_swap_forget_view(page->address + i * 4096, 4096);
        }
    }
    if (page->view_mask)
        --uwp_swap_views;
    page->view_mask = 0;
    if (physical->frame) {
        uwp_swap_frames[physical->frame - 1] = 0;
        --uwp_swap_frame_count;
        physical->frame = 0;
    }
    page->kind = 4;
    return 1;
}

static int uwp_swap_view_room(UwpPage* exclude) {
    const size_t max_scan = uwp_count * 2;
    for (size_t n = 0; uwp_swap_frame_count >= uwp_swap_view_limit && n < max_scan; ++n) {
        if (uwp_swap_view_cursor >= uwp_count)
            uwp_swap_view_cursor = 0;
        UwpPage* page = &uwp_pages[uwp_swap_view_cursor++];
        if (page == exclude || !page->mapped || (page->kind != 3 && page->kind != 4))
            continue;
        UwpPhysical* physical = &uwp_physical[page->physical / PAGE];
        if ((physical->pinned || physical->leases) || !physical->frame)
            continue;
        if (physical->recent) {
            physical->recent = 0;
            continue;
        }
        physical->hash_valid = 0;
        if (uwp_swap_unmap_view(page))
            ++uwp_swap_view_evictions;
    }
    return uwp_swap_frame_count < uwp_swap_view_limit;
}

static unsigned char* uwp_swap_bytes(uint64_t offset) {
    UwpPhysical* physical = &uwp_physical[offset / PAGE];
    if (!physical->frame) {
        UwpPage* page = uwp_page(physical->owner);
        if (!uwp_swap_section || !uwp_swap_view_room(page)) {
            uwp_fail("swap frame capacity", physical->owner);
        }
        uint64_t frame = 0;
        while (frame < uwp_swap_view_limit && uwp_swap_frames[frame])
            ++frame;
        if (frame == uwp_swap_view_limit)
            uwp_fail("swap frame bookkeeping", physical->owner);
        unsigned char* data = uwp_swap_alias + frame * PAGE;
        if (!VirtualAllocFromApp(data, PAGE, MEM_COMMIT, PAGE_READWRITE)) {
            uwp_fail("swap frame commit", physical->owner);
        }
        if (!uwp_swap_transfer(offset, data, 0))
            uwp_fail("swap frame read", physical->owner);
        if (physical->hash_valid) {
            ++uwp_swap_hash_checks;
            if (uwp_swap_hash(data) != physical->hash) {
                ++uwp_swap_hash_errors;
                uwp_fail("swap integrity check", physical->owner);
            }
            physical->hash_valid = 0;
        }
        uwp_swap_frames[frame] = 1;
        ++uwp_swap_frame_count;
        physical->frame = (unsigned)frame + 1;
        uwp_swap_in += PAGE;

        uwp_swap_drop_pool_record(offset);
    }
    return uwp_swap_alias + (physical->frame - 1) * PAGE;
}

static int uwp_swap_write_backing(uint64_t offset, uint64_t within, const void* source,
                                  uint64_t size) {
    UwpPhysical* physical = &uwp_physical[offset / PAGE];
    unsigned char* data =
        physical->frame ? (uwp_swap_alias + (physical->frame - 1) * PAGE) : uwp_swap_copy;
    if (!physical->frame && !uwp_swap_transfer(offset, data, 0))
        return 0;
    memcpy(data + within, source, (size_t)size);
    physical->hash_valid = 0;
    return physical->frame || uwp_swap_transfer(offset, data, 1);
}

static int uwp_swap_read_backing(uint64_t offset, uint64_t within, void* dest, uint64_t size) {
    UwpPhysical* physical = &uwp_physical[offset / PAGE];
    const unsigned char* data =
        physical->frame ? (uwp_swap_alias + (physical->frame - 1) * PAGE) : uwp_swap_copy;
    if (!physical->frame && !uwp_swap_transfer(offset, uwp_swap_copy, 0))
        return 0;
    memcpy(dest, data + within, (size_t)size);
    return 1;
}

static int uwp_swap_page_in(UwpPage* page) {
    UwpPhysical* physical = &uwp_physical[page->physical / PAGE];
    uwp_swap_bytes(page->physical);
    unsigned before = page->view_mask;
    for (unsigned i = 0; i < 4; ++i) {
        if ((page->view_mask & (1u << i)) || !page->protection[i])
            continue;
        void* view = NULL;
        for (unsigned attempt = 0;; ++attempt) {
            view = MapViewOfFile3FromApp(
                uwp_swap_section, GetCurrentProcess(), (void*)(page->address + i * 4096),
                (physical->frame - 1) * PAGE + i * 4096, 4096, MEM_REPLACE_PLACEHOLDER,
                win_prot(page->protection[i]), NULL, 0);
            if (view) {
                break;
            }

            if (attempt == 7 || !uwp_swap_page_out_locked(UINT64_C(16) * 1024 * 1024, page, 1, 0)) {
                uwp_fail("swap page-in", page->address + i * 4096);
            }
        }

        if (uwp_swap_accept_view(view, page->address + i * 4096, 4096, page->address) == NULL) {
            UnmapViewOfFile2(GetCurrentProcess(), (void*)(page->address + i * 4096),
                             MEM_PRESERVE_PLACEHOLDER);
            return 0;
        }
        page->view_mask |= 1u << i;
    }
    if (!before && page->view_mask) {
        page->kind = 3;
        ++uwp_swap_views;
    }
    if (before != page->view_mask)
        ++uwp_commit_epoch;
    return before != page->view_mask;
}

static int uwp_swap_protect(UwpPage* page, unsigned index, int prot) {
    unsigned bit = 1u << index;
    uintptr_t address = page->address + index * 4096;
    if (page->view_mask & bit) {
        DWORD old;
        if (!VirtualProtectFromApp((void*)address, 4096, win_prot(prot), &old)) {
            if (!UnmapViewOfFile2(GetCurrentProcess(), (void*)address, MEM_PRESERVE_PLACEHOLDER)) {
                return 0;
            }
            uwp_swap_forget_view(address, 4096);
            page->view_mask &= ~bit;
            if (!page->view_mask) {
                page->kind = 4;
                --uwp_swap_views;
            }
            unsigned frame = uwp_physical[page->physical / PAGE].frame;
            if (prot && frame) {

                void* view =
                    MapViewOfFile3FromApp(uwp_swap_section, GetCurrentProcess(), (void*)address,
                                          (frame - 1) * PAGE + index * 4096, 4096,
                                          MEM_REPLACE_PLACEHOLDER, win_prot(prot), NULL, 0);
                if (!view) {
                    uwp_fail("swap protection remap", address);
                }
                if (uwp_swap_accept_view(view, address, 4096, page->address) == NULL) {
                    return 0;
                }
                page->view_mask |= bit;
            }
        }
    }
    page->protection[index] = prot;
    ++uwp_commit_epoch;
    return 1;
}

static int uwp_swap_disk_blocked(void) {
    return uwp_swap_disk_full || uwp_swap_store_next >= uwp_swap_disk_budget;
}

static uint64_t uwp_swap_page_out_locked(uint64_t bytes, UwpPage* exclude, int include_views,
                                         int ignore_recent) {
    if (!uwp_swap_open())
        return 0;
    if (bytes)
        ++uwp_swap_passes;

    if (uwp_swap_disk_blocked() && !include_views && !uwp_pool_available() &&
        !(uwp_swap_disk_growable && !uwp_swap_disk_full &&
          uwp_swap_disk_budget < uwp_swap_disk_ceiling)) {
        const uint64_t now = GetTickCount64();
        static uint64_t last_blocked_report;
        if (now - last_blocked_report >= 1000) {
            last_blocked_report = now;
            printf("UWP swap: nothing can be written (file %" PRIu64 " MiB of %" PRIu64
                   " MiB budget, full=%d); sweeping for views only\n",
                   uwp_swap_store_next >> 20, uwp_swap_disk_budget >> 20, (int)uwp_swap_disk_full);
            memory_breadcrumb("swap: disk full, private pages cannot be paged out");
        }
        return 0;
    }

    if (uwp_swap_disk_full && GetTickCount64() - uwp_swap_full_tick > UINT64_C(10000)) {
        uwp_swap_disk_full = 0;
        ++uwp_swap_full_rearms;
        if (uwp_swap_full_rearms <= 8) {
            printf("UWP swap: retrying the device after a full-disk write (attempt %" PRIu64
                   ", %" PRIu64 " MiB stored)\n",
                   uwp_swap_full_rearms, uwp_swap_store_next >> 20);
        }
    }
    uint64_t released = 0;
    const size_t max_scan = uwp_count * 2;
    for (size_t scanned = 0; scanned < max_scan && released < bytes; ++scanned) {

        if (uwp_swap_cursor >= uwp_count)
            uwp_swap_cursor = 0;
        UwpPage* page = &uwp_pages[uwp_swap_cursor++];
        ++uwp_scan_pages;
        if (uwp_swap_sweep_deadline && !(uwp_scan_pages & 8191) &&
            GetTickCount64() > uwp_swap_sweep_deadline) {

            ++uwp_swap_sweep_timeouts;
            memory_breadcrumb("swap: sweep hit its timeout");
            printf("UWP swap: sweep stopped at its %" PRIu64 " ms timeout (released %" PRIu64
                   " MiB of the %" PRIu64 " MiB asked; budget=%" PRIu64 "/%" PRIu64 " MiB)\n",
                   uwp_swap_sweep_timeout_ms(), released >> 20, bytes >> 20,
                   uwp_swap_store_next >> 20, uwp_swap_disk_budget >> 20);
            break;
        }
        if (page == exclude)
            continue;
        if (!page->mapped) {
            ++uwp_scan_unmapped;
            continue;
        }
        if (!page->pageable) {
            ++uwp_scan_unpageable;
            continue;
        }
        if (page->kind != 1 && page->kind != 3) {
            ++uwp_scan_kind;
            continue;
        }
        if (page->kind == 1 && !page->committed) {
            ++uwp_scan_uncommitted;
            continue;
        }
        if (page->kind == 3 && !include_views) {
            ++uwp_scan_noview;
            continue;
        }
        UwpPhysical* physical = &uwp_physical[page->physical / PAGE];
        if ((physical->pinned || physical->leases)) {
            ++uwp_scan_pinned;
            continue;
        }
        if (physical->references != 1) {
            ++uwp_scan_shared;
            continue;
        }
        if (physical->section) {
            ++uwp_scan_section;
            continue;
        }
        if (physical->recent) {
            physical->recent = 0;
            ++uwp_scan_recent_cleared;
            if (!ignore_recent) {
                continue;
            }
            ++uwp_swap_urgent_pages;
        }
        if (page->kind == 3 && !uwp_swap_live_page(page)) {

            ++uwp_swap_view_guard_rejects;
            ++uwp_scan_viewguard;
            continue;
        }
        if (page->kind == 3) {
            physical->hash_valid = 0;
            if (!uwp_swap_unmap_view(page)) {
                continue;
            }
            released += PAGE;
            uwp_swap_out += PAGE;
            continue;
        }
        if (uwp_swap_disk_full || uwp_swap_store_next >= uwp_swap_disk_budget) {

            if (uwp_swap_disk_full || !uwp_swap_disk_growable ||
                uwp_swap_disk_budget >= uwp_swap_disk_ceiling || uwp_scan_write_failed) {

                int blank = uwp_swap_blank_enabled();
                for (unsigned i = 0; i < 4 && blank; ++i) {
                    if (!(page->protection[i] & PROT_READ) || (page->protection[i] & PROT_EXEC)) {
                        blank = 0;
                    }
                }
                const size_t zero_key = (size_t)(page->physical / PAGE);
                if (blank && uwp_swap_slot && zero_key < uwp_swap_slot_count &&
                    uwp_swap_blank((void*)page->address)) {
                    if (uwp_swap_slot[zero_key] != UWP_SWAP_SLOT_ZERO)
                        ++uwp_swap_store_zero;
                    uwp_swap_drop_pool_record(page->physical);
                    uwp_swap_slot[zero_key] = UWP_SWAP_SLOT_ZERO;
                    ++uwp_swap_store_pages;
                    uwp_swap_blank_bytes += PAGE;
                    ++uwp_scan_zero_paged;
                    ++uwp_swap_zero_paged;
                    if (!VirtualFree((void*)page->address, PAGE,
                                     MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
                        uwp_fail("swap private page release", page->address);
                    }
                    page->placed = 0;
                    page->kind = 4;
                    page->committed = 0;
                    uwp_swap_split(page);
                    physical->disk = 1;
                    released += PAGE;
                    uwp_swap_out += PAGE;
                    continue;
                }

                if (!uwp_pool_available()) {
                    ++uwp_scan_atbudget;
                    uwp_scan_atbudget_bytes += PAGE;
                    continue;
                }
                ++uwp_scan_pool_fallback;
                goto write_private_page;
            }
            uwp_swap_disk_budget += uwp_swap_disk_step;
            if (uwp_swap_disk_budget > uwp_swap_disk_ceiling) {
                uwp_swap_disk_budget = uwp_swap_disk_ceiling;
            }
            ++uwp_swap_disk_raised;
            if (uwp_swap_disk_raised <= 8) {
                printf("UWP swap: disk budget raised to %" PRIu64 " MiB (ceiling %" PRIu64
                       " MiB) for a waiting allocation\n",
                       uwp_swap_disk_budget >> 20, uwp_swap_disk_ceiling >> 20);
            }
        }
    write_private_page:;
        int readable = 1;
        for (unsigned i = 0; i < 4; ++i) {
            if (!(page->protection[i] & PROT_READ) || (page->protection[i] & PROT_EXEC)) {
                readable = 0;
            }
        }
        if (!readable) {
            ++uwp_scan_unreadable;
            continue;
        }
        DWORD old;
        for (unsigned i = 0; i < 4; ++i) {
            if (!VirtualProtectFromApp((void*)(page->address + i * 4096), 4096, PAGE_READONLY,
                                       &old)) {
                uwp_fail("swap write protection", page->address);
            }
        }
        physical->hash = uwp_swap_hash((void*)page->address);
        physical->hash_valid = 1;
        if (!uwp_swap_transfer(page->physical, (void*)page->address, 1)) {

            if (uwp_swap_transfer_error == ERROR_DISK_QUOTA_EXCEEDED) {

                ++uwp_scan_atbudget;
                uwp_scan_atbudget_bytes += PAGE;
                uwp_apply_protection(page);
                continue;
            }
            ++uwp_swap_write_failures;
            ++uwp_scan_write_failed;
            uwp_scan_write_failed_bytes += PAGE;
            if (uwp_swap_transfer_error == ERROR_DISK_FULL) {
                uwp_swap_disk_full = 1;
                uwp_swap_full_tick = GetTickCount64();
            } else
                physical->hash_valid = 0;
            uwp_apply_protection(page);
            continue;
        }
        if (!VirtualFree((void*)page->address, PAGE, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            uwp_fail("swap private page release", page->address);
        }
        page->placed = 0;
        page->kind = 4;
        page->committed = 0;
        uwp_swap_split(page);
        if (!physical->disk) {
            physical->disk = 1;

            if (uwp_swap_slot[(size_t)(page->physical / PAGE)] != UWP_SWAP_SLOT_ZERO) {
                ++uwp_swap_disk_pages;
            }
        }
        released += PAGE;
        uwp_swap_out += PAGE;
    }
    if (bytes && !released)
        ++uwp_swap_dry;
    return released;
}

static void uwp_swap_buckets(uint64_t* resident, uint64_t* pinned, uint64_t* section,
                             uint64_t* shared, uint64_t* unreadable) {
    uint64_t res = 0, pin = 0, sect = 0, sh = 0, unread = 0;
    for (size_t i = 0; i < uwp_count; ++i) {
        const UwpPage* page = &uwp_pages[i];
        if (!page->mapped || !page->pageable)
            continue;
        const UwpPhysical* physical = &uwp_physical[page->physical / PAGE];
        if ((physical->pinned || physical->leases)) {
            pin += PAGE;
            continue;
        }
        if (page->kind != 1 || !page->committed)
            continue;
        if (physical->section) {
            sect += PAGE;
            continue;
        }
        if (physical->references != 1) {
            sh += PAGE;
            continue;
        }
        int readable = 1;
        for (unsigned k = 0; k < 4; ++k) {
            if (!(page->protection[k] & PROT_READ) || (page->protection[k] & PROT_EXEC)) {
                readable = 0;
            }
        }
        if (!readable) {
            unread += PAGE;
            continue;
        }
        res += PAGE;
    }
    *resident = res;
    *pinned = pin;
    *section = sect;
    *shared = sh;
    *unreadable = unread;
}

static uint64_t uwp_swap_reclaim(uint64_t bytes, int include_views, int ignore_recent) {
    read_lock();
    AcquireSRWLockExclusive(&uwp_commit_lock);

    uwp_swap_lock_tick = GetTickCount64();
    uwp_swap_lock_what = include_views ? "sweep+views" : "sweep";
    ++uwp_swap_lock_count;
    uint64_t result = uwp_swap_page_out_locked(bytes, NULL, include_views, ignore_recent);
    if (ignore_recent && result)
        ++uwp_swap_urgent_sweeps;
    uwp_swap_lock_what = "-";
    ReleaseSRWLockExclusive(&uwp_commit_lock);
    read_unlock();
    return result;
}

void runtime_memory_pager_state(void) {
    const uint64_t now = GetTickCount64();
    const char* what = uwp_swap_lock_what;
    const uint64_t held = (what && what[0] != '-') ? now - uwp_swap_lock_tick : 0;

    const int running = uwp_swap_request_bytes != 0;
    const uint64_t asked = running ? uwp_swap_request_bytes : uwp_swap_last_request_bytes;
    const uint64_t since =
        running ? now - uwp_swap_request_tick
                : (uwp_swap_last_request_done ? now - uwp_swap_last_request_done : UINT64_C(0));
    printf("UWP swap: state at the stall: lock=%s held=%" PRIu64 " ms sweeps=%" PRIu64
           " request=%" PRIu64 " MiB %s %" PRIu64 " ms ago; out=%" PRIu64 " in=%" PRIu64
           " MiB disk=%" PRIu64 " MiB budget=%" PRIu64 " MiB\n",
           what, held, uwp_swap_lock_count, asked >> 20, running ? "still running for" : "finished",
           since, uwp_swap_out >> 20, uwp_swap_in >> 20, uwp_swap_store_next >> 20,
           uwp_swap_disk_budget >> 20);
    fflush(stdout);
}

uint64_t runtime_memory_page_out(uint64_t bytes) {

    uwp_swap_request_bytes = bytes;
    uwp_swap_request_tick = GetTickCount64();
    static uint64_t empty_tick;
    static int empty_blocked;
    const uint64_t now = GetTickCount64();
    if (empty_blocked && now - empty_tick < UINT64_C(500)) {

        uwp_swap_last_request_bytes = bytes;
        uwp_swap_last_request_done = now;
        uwp_swap_request_bytes = 0;
        return 0;
    }
    uint64_t total = 0;
    uwp_swap_scan_reset();

    uwp_swap_disk_growable = 1;

    uwp_swap_sweep_deadline = now + uwp_swap_sweep_timeout_ms();
    memory_operation("guest memory pager: sweeping", bytes, 0);
    for (int sweep = 0; sweep < 3 && total < bytes; ++sweep) {
        const uint64_t got = uwp_swap_reclaim(bytes - total, 1, sweep > 0);
        if (!got) {
            if (sweep > 0)
                break;
            continue;
        }
        total += got;
    }
    uwp_swap_disk_growable = 0;
    uwp_swap_sweep_deadline = 0;
    memory_operation_done();
    uwp_swap_last_request_bytes = bytes;
    uwp_swap_last_request_done = GetTickCount64();
    uwp_swap_request_bytes = 0;
    if (total) {
        printf("UWP swap: paged out %" PRIu64 " MiB on request (%" PRIu64 " MiB of it urgent)\n",
               total >> 20, (uwp_swap_urgent_pages * PAGE) >> 20);
        return total;
    }

    empty_tick = now;

    empty_blocked = uwp_swap_disk_full || uwp_scan_atbudget > 0;
    static unsigned uwp_swap_empty_requests;
    if (uwp_swap_empty_requests < 8) {
        ++uwp_swap_empty_requests;
        uint64_t b_resident = 0, b_pinned = 0, b_section = 0, b_shared = 0, b_unreadable = 0;
        read_lock();
        AcquireSRWLockExclusive(&uwp_commit_lock);
        uwp_swap_buckets(&b_resident, &b_pinned, &b_section, &b_shared, &b_unreadable);
        ReleaseSRWLockExclusive(&uwp_commit_lock);
        read_unlock();
        printf("UWP swap: request %" PRIu64 " MiB got nothing; scanned=%" PRIu64
               " unmapped=%" PRIu64 " unpageable=%" PRIu64 " kind=%" PRIu64 " uncommitted=%" PRIu64
               " views-off=%" PRIu64 " pinned=%" PRIu64 " shared=%" PRIu64 " section=%" PRIu64
               " at-budget=%" PRIu64 " (%" PRIu64 " MiB) unreadable=%" PRIu64 " view-guard=%" PRIu64
               " recent=%" PRIu64 " write-failed=%" PRIu64 "; disk=%" PRIu64 "/%" PRIu64
               " MiB full=%d write failures=%" PRIu64 "; page table: resident=%" PRIu64
               " pinned=%" PRIu64 " section=%" PRIu64 " shared=%" PRIu64 " unreadable=%" PRIu64
               " MiB\n",
               bytes >> 20, uwp_scan_pages, uwp_scan_unmapped, uwp_scan_unpageable, uwp_scan_kind,
               uwp_scan_uncommitted, uwp_scan_noview, uwp_scan_pinned, uwp_scan_shared,
               uwp_scan_section, uwp_scan_atbudget, uwp_scan_atbudget_bytes >> 20,
               uwp_scan_unreadable, uwp_scan_viewguard, uwp_scan_recent_cleared,
               uwp_scan_write_failed, (uwp_swap_disk_pages * PAGE) >> 20,
               uwp_swap_disk_budget >> 20, (int)uwp_swap_disk_full, uwp_swap_write_failures,
               b_resident >> 20, b_pinned >> 20, b_section >> 20, b_shared >> 20,
               b_unreadable >> 20);
    }
    return total;
}

void runtime_memory_swap_stats(RuntimeSwapStats* out) {
    if (!out)
        return;
    out->disk_used = uwp_swap_store_next;
    out->disk_budget = uwp_swap_disk_budget;
    out->payload = uwp_swap_store_payload;
    out->pages = uwp_swap_store_pages;
    out->blank_pages = uwp_swap_store_zero;
    out->blank_bytes = uwp_swap_blank_bytes;
    out->raw_bytes = uwp_swap_raw_bytes;
    out->compressed_bytes = uwp_swap_compressed_bytes;
    out->device_free = uwp_swap_free_space();
    out->reserve = uwp_swap_reserve;
    out->atbudget_bytes = uwp_scan_atbudget_bytes;
    out->write_failed_bytes = uwp_scan_write_failed_bytes;
    out->copy_out_pages = uwp_swap_copy_out_pages;
    out->copy_in_pages = uwp_swap_copy_in_pages;
    out->copy_in_failures = uwp_swap_copy_in_failures;
    out->write_failures = uwp_swap_write_failures;
    out->zero_paged_bytes = uwp_swap_zero_paged * PAGE;
    out->pool_capacity = uwp_pool_size;
    out->pool_committed = uwp_pool_commit;
    out->pool_peak_committed = uwp_pool_peak_commit;
    out->pool_pages = uwp_pool_pages;
    out->pool_page_bytes = uwp_pool_pages * PAGE;

    out->pool_saved = swap_pool_net_saved(out->pool_page_bytes, uwp_pool_commit);
    out->pool_pages_in = uwp_pool_pages_in;
    out->pool_refused = uwp_pool_refused;
}

void runtime_memory_resident_breakdown(uint64_t* out, size_t count) {
    if (!out)
        return;
    for (size_t i = 0; i < count; ++i)
        out[i] = 0;
    read_lock();
    AcquireSRWLockExclusive(&uwp_commit_lock);
    for (size_t i = 0; i < uwp_count; ++i) {
        const UwpPage* page = &uwp_pages[i];
        if (!page->mapped)
            continue;
        switch (page->kind) {
        case 1:
            if (!page->committed)
                break;
            out[0] += PAGE;
            if (page->pageable)
                out[1] += PAGE;
            else
                out[2] += PAGE;
            break;
        case 2:
            out[3] += PAGE;
            break;
        case 3:
            out[4] += PAGE;
            break;
        case 4:
            out[5] += PAGE;
            break;
        default:
            break;
        }
    }
    ReleaseSRWLockExclusive(&uwp_commit_lock);
    read_unlock();
}

uint64_t runtime_memory_resident_bytes(void) {
    read_lock();
    AcquireSRWLockExclusive(&uwp_commit_lock);
    uint64_t resident = 0;
    for (size_t i = 0; i < uwp_count; ++i) {
        const UwpPage* page = &uwp_pages[i];
        if (!page->mapped)
            continue;
        if (page->kind == 1) {
            if (page->committed)
                resident += PAGE;
        } else {
            resident += PAGE;
        }
    }
    ReleaseSRWLockExclusive(&uwp_commit_lock);
    read_unlock();
    return resident;
}

uint64_t runtime_memory_pinned_bytes(void) {
    read_lock();
    AcquireSRWLockExclusive(&uwp_commit_lock);
    uint64_t pinned = 0;
    for (size_t i = 0; i < uwp_count; ++i) {
        const UwpPage* page = &uwp_pages[i];
        if (!page->mapped || !page->pageable)
            continue;
        if (uwp_physical[page->physical / PAGE].pinned)
            pinned += PAGE;
    }
    ReleaseSRWLockExclusive(&uwp_commit_lock);
    read_unlock();
    return pinned;
}

static void uwp_swap_report(void) {
    read_lock();
    AcquireSRWLockExclusive(&uwp_commit_lock);

    uint64_t resident = 0, pinned = 0, section = 0, shared = 0, unreadable = 0;
    uwp_swap_buckets(&resident, &pinned, &section, &shared, &unreadable);
    uint64_t out = uwp_swap_out, in = uwp_swap_in;

    uint64_t disk = uwp_swap_store_next / PAGE, views = uwp_swap_views;
    uint64_t checks = uwp_swap_hash_checks, errors = uwp_swap_hash_errors;
    uint64_t evictions = uwp_swap_view_evictions;
    ReleaseSRWLockExclusive(&uwp_commit_lock);
    read_unlock();
    const char* profile = getenv("BB_IO_STATS");

    const uint64_t pinned_mb = pinned >> 20;
    static uint64_t last_pinned_mb;
    const uint64_t moved =
        pinned_mb > last_pinned_mb ? pinned_mb - last_pinned_mb : last_pinned_mb - pinned_mb;
    if (!out && !uwp_swap_dry && moved < 64 && !(profile && *profile == '1'))
        return;
    last_pinned_mb = pinned_mb;
    printf("UWP swap: out=%" PRIu64 " in=%" PRIu64 " disk=%" PRIu64 " views=%" PRIu64
           " MiB; checks=%" PRIu64 " corrupt=%" PRIu64 " window evictions=%" PRIu64 "\n",
           out >> 20, in >> 20, (disk * PAGE) >> 20, (views * PAGE) >> 20, checks, errors,
           evictions);
    printf("UWP swap: pageable private=%" PRIu64 " pinned=%" PRIu64 " MiB; page-out passes=%" PRIu64
           " with nothing to give=%" PRIu64 "\n",
           resident >> 20, pinned_mb, uwp_swap_passes, uwp_swap_dry);
    printf("UWP swap: not paged - pinned policy=%" PRIu64 " section=%" PRIu64 " shared=%" PRIu64
           " exec/unreadable=%" PRIu64 " MiB\n",
           pinned_mb, section >> 20, shared >> 20, unreadable >> 20);

    printf("UWP swap: disk pages=%" PRIu64 " MiB of a %" PRIu64 " MiB budget (ceiling %" PRIu64
           " MiB, raised %" PRIu64 "); %" PRIu64 " pages stored (%" PRIu64 " raw, %" PRIu64
           " blank, %" PRIu64 " MiB of payload, %" PRIu64 "%% of raw [blank %" PRIu64
           " MiB 0 B, raw %" PRIu64 " MiB, compressed %" PRIu64 " MiB]); write failures=%" PRIu64
           "; codec fallbacks=%" PRIu64 "; copy-out=%" PRIu64 " copy-in=%" PRIu64
           " (failed %" PRIu64 "); urgent sweeps=%" PRIu64 " (%" PRIu64 " MiB)%s\n",
           (disk * PAGE) >> 20, uwp_swap_disk_budget >> 20, uwp_swap_disk_ceiling >> 20,
           (uint64_t)uwp_swap_disk_raised, uwp_swap_store_pages, uwp_swap_store_raw,
           uwp_swap_store_zero, uwp_swap_store_payload >> 20,
           uwp_swap_store_pages ? (uwp_swap_store_payload * 100) / (uwp_swap_store_pages * PAGE)
                                : UINT64_C(0),
           uwp_swap_blank_bytes >> 20, uwp_swap_raw_bytes >> 20, uwp_swap_compressed_bytes >> 20,
           uwp_swap_write_failures, uwp_swap_codec_fallbacks, uwp_swap_copy_out_pages,
           uwp_swap_copy_in_pages, uwp_swap_copy_in_failures, uwp_swap_urgent_sweeps,
           (uwp_swap_urgent_pages * PAGE) >> 20,
           uwp_swap_disk_full ? " (device full: private pages stay resident)" : "");

    if (uwp_pool_blobs) {
        const uint64_t held = uwp_pool_pages * PAGE;
        printf("UWP swap pool: %" PRIu64 " pages = %" PRIu64 " MiB of guest memory in %" PRIu64
               " MiB committed (capacity %" PRIu64 " MiB, peak %" PRIu64 " MiB) = %" PRIu64
               " MiB kept out of the app budget; %" PRIu64 " page-ins, %" PRIu64
               " refused, %" PRIu64 " decommit failures (last error %lu), %" PRIu64
               " MiB actually decommitted\n",
               uwp_pool_pages, held >> 20, uwp_pool_commit >> 20, uwp_pool_size >> 20,
               uwp_pool_peak_commit >> 20, swap_pool_net_saved(held, uwp_pool_commit) >> 20,
               uwp_pool_pages_in, uwp_pool_refused, uwp_pool_decommit_failures,
               uwp_pool_last_decommit_error, uwp_pool_decommitted >> 20);
    }

    const uint64_t free_now = uwp_swap_free_space();
    if (free_now != 0 || uwp_swap_reserve_hits) {
        printf("UWP swap: device free=%" PRIu64 " MiB (reserve %" PRIu64
               " MiB kept for the game's own writes, %" PRIu64 " record(s) refused by it)\n",
               free_now >> 20, uwp_swap_reserve >> 20, uwp_swap_reserve_hits);
    }
}
