// SPDX-License-Identifier: GPL-2.0-only
// Adapted from Test-project-arena-8ac58bdb-test-project, xbox/platform.

typedef struct {
    uintptr_t owner;
    unsigned char* saved;
    HANDLE section;
    unsigned references, frame, leases;
    uint64_t hash;
    unsigned char pinned, recent, disk, hash_valid, discard_pending, executable, section_executable;
} UwpPhysical;

typedef struct {
    uintptr_t address;
    uint64_t physical;
    int mapped, kind, pageable, split, committed, isolated, placed;
    unsigned view_mask;
    int protection[4];
} UwpPage;

static UwpPhysical* uwp_physical;
static UwpPage* uwp_pages;
static size_t uwp_count, uwp_capacity;
static SRWLOCK uwp_commit_lock = SRWLOCK_INIT;
static uint64_t uwp_faults, uwp_aliases, uwp_snapshots, uwp_moves, uwp_commit_epoch;

static void uwp_fail(const char* what, uintptr_t address);
static UwpPage* uwp_page(uintptr_t address);
static int uwp_promote(UwpPhysical* physical, uint64_t offset);

static int uwp_pin_private_pages(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* env = getenv("SHADPS4_UWP_PIN_PRIVATE");
        cached = env && *env && *env != '0';
    }
    return cached;
}

static DWORD win_prot(int prot) {
    if (prot & PROT_EXEC)
        return PAGE_EXECUTE_READ;
    if (prot & PROT_WRITE)
        return PAGE_READWRITE;
    return (prot & PROT_READ) ? PAGE_READONLY : PAGE_NOACCESS;
}

static DWORD uwp_low_prot(int prot) {
    return win_prot(prot);
}

static void uwp_apply_protection(UwpPage* page) {
    for (unsigned i = 0; i < 4; ++i) {
        DWORD old;
        if (!VirtualProtectFromApp((void*)(page->address + i * 4096), 4096,
                                   win_prot(page->protection[i]), &old)) {
            uwp_fail("page protection", page->address + i * 4096);
        }
    }
}

#include "guest_swap.inc"

static size_t uwp_index(uintptr_t address) {
    size_t lo = 0, hi = uwp_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (uwp_pages[mid].address < address)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static UwpPage* uwp_page(uintptr_t address) {
    uintptr_t base = address & ~(uintptr_t)(PAGE - 1);
    size_t i = uwp_index(base);
    return (i < uwp_count && uwp_pages[i].address == base) ? &uwp_pages[i] : NULL;
}

void runtime_memory_reserve(void) {
    if (uwp_physical)
        return;
    uwp_physical = calloc((POOL_SIZE + FLEX_SPAN) / PAGE, sizeof(*uwp_physical));
    if (!uwp_physical)
        uwp_fail("physical page bookkeeping", 0);
    puts("UWP memory: reserving guest windows on demand; 16 KiB commit-on-touch pages");
}

static int pool(void) {
    runtime_memory_reserve();
    return 0;
}

static uintptr_t uwp_available(uintptr_t address, uint64_t size) {
    for (uintptr_t at = address; at < address + size;) {
        UwpPage* own = uwp_page(at);
        if (own) {
            at = own->address + PAGE;
            continue;
        }
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((void*)at, &info, sizeof(info)))
            return 0;
        uintptr_t end = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (info.State != MEM_FREE)
            return end;
        at = end;
    }
    return address;
}

static int uwp_reserve(uintptr_t address, uint64_t size) {
    if (!address || size > UINTPTR_MAX - address)
        return -1;
    uintptr_t end = align_up(address + size, 65536);
    for (uintptr_t at = address & ~(uintptr_t)65535; at < end;) {
        if (uwp_page(at)) {
            at += 65536;
            continue;
        }
        uintptr_t run = at;
        while (run < end && !uwp_page(run))
            run += 65536;
        MEMORY_BASIC_INFORMATION existing;
        if ((!VirtualQuery((void*)(at < address ? address : at), &existing, sizeof(existing)) ||
             existing.State != MEM_RESERVE) &&
            !VirtualAlloc2FromApp(NULL, (void*)at, run - at, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                  PAGE_NOACCESS, NULL, 0)) {
            return -1;
        }
        size_t count = (run - at) / PAGE;
        if (uwp_count + count > uwp_capacity) {
            size_t capacity = uwp_capacity ? uwp_capacity : 256;
            while (capacity < uwp_count + count)
                capacity *= 2;
            UwpPage* next = realloc(uwp_pages, capacity * sizeof(*next));
            if (!next)
                uwp_fail("virtual page bookkeeping", at);
            uwp_pages = next;
            uwp_capacity = capacity;
        }
        size_t i = uwp_index(at);
        memmove(uwp_pages + i + count, uwp_pages + i, (uwp_count - i) * sizeof(*uwp_pages));
        for (size_t n = 0; n < count; ++n) {
            uwp_pages[i + n] = (UwpPage){.address = at + n * PAGE};
        }
        uwp_count += count;
        at = run;
    }
    return 0;
}

static void uwp_isolate(UwpPage* page) {

    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((void*)page->address, &info, sizeof(info)) || info.State != MEM_RESERVE) {
        uwp_fail("placeholder query", page->address);
    }
    uintptr_t base = (uintptr_t)info.AllocationBase;
    uintptr_t end = (uintptr_t)info.BaseAddress + info.RegionSize;
    if (base < page->address &&
        !VirtualFree((void*)base, page->address - base, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
        uwp_fail("splitting placeholder", page->address);
    }
    if (end > page->address + PAGE &&
        !VirtualFree((void*)page->address, PAGE, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
        uwp_fail("splitting placeholder", page->address);
    }
    page->isolated = 1;
}

static int uwp_commit(UwpPage* page) {
    if (page->kind == 4 || page->kind == 3)
        return uwp_swap_page_in(page);
    if (page->kind != 1)
        return 0;
    if (page->committed)
        return 0;
    if (!page->placed) {
        uwp_isolate(page);
        if (!VirtualAlloc2FromApp(NULL, (void*)page->address, PAGE,
                                  MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0)) {
            uwp_fail("private page reservation", page->address);
        }
        page->placed = 1;
    }
    for (unsigned attempt = 0;
         !VirtualAllocFromApp((void*)page->address, PAGE, MEM_COMMIT, PAGE_READWRITE); ++attempt) {

        if (attempt == 7 || !uwp_swap_page_out_locked(UINT64_C(16) << 20, page, 1, 0)) {
            uwp_fail("commit-on-touch", page->address);
        }
    }
    page->committed = 1;
    UwpPhysical* physical = &uwp_physical[page->physical / PAGE];
    if (physical->saved) {
        memcpy((void*)page->address, physical->saved, PAGE);
        free(physical->saved);
        physical->saved = NULL;
        ++uwp_moves;
    }
    uwp_apply_protection(page);
    ++uwp_faults;
    ++uwp_commit_epoch;
    return 1;
}

int runtime_memory_handle_commit_fault_at(uintptr_t address, int access, int guest_code) {
    static __thread uintptr_t last_retry_subpage;
    static __thread uint64_t last_retry_epoch;
    read_lock();
    AcquireSRWLockExclusive(&uwp_commit_lock);
    UwpPage* page = uwp_page(address);
    int handled = 0;
    if (page && page->mapped && (page->kind == 1 || page->kind == 3 || page->kind == 4)) {
        UwpPhysical* physical = &uwp_physical[page->physical / PAGE];
        physical->recent = 1;
        unsigned index = (unsigned)((address - page->address) / 4096);
        int wanted = page->protection[index];

        if ((wanted & PROT_EXEC) ||
            (guest_code && (page->kind == 3 || page->kind == 4 || uwp_pin_private_pages()))) {
            physical->pinned = 1;
        }
        int permitted = (access == 1)   ? (wanted & PROT_WRITE)
                        : (access == 8) ? (wanted & PROT_EXEC)
                                        : (wanted & (PROT_READ | PROT_WRITE | PROT_EXEC));
        if (permitted) {
            if (physical->pinned && (page->kind == 3 || page->kind == 4)) {
                if (uwp_promote(physical, page->physical)) {
                    uwp_fail("pinned page promotion", page->address);
                }
                ++uwp_commit_epoch;
                handled = 1;
            } else {
                handled = uwp_commit(page);
            }
            if (!handled && (wanted & (PROT_EXEC | PROT_WRITE)) == (PROT_EXEC | PROT_WRITE)) {
                MEMORY_BASIC_INFORMATION info;
                DWORD old;
                uintptr_t subpage = address & ~(uintptr_t)4095;
                if (VirtualQuery((void*)subpage, &info, sizeof(info)) && info.State == MEM_COMMIT) {
                    DWORD target = (access == 8) ? PAGE_EXECUTE_READ : PAGE_READWRITE;
                    if (info.Protect == target) {
                        if (last_retry_subpage != subpage || last_retry_epoch != uwp_commit_epoch) {
                            last_retry_subpage = subpage;
                            last_retry_epoch = uwp_commit_epoch;
                            handled = 1;
                        }
                    } else if (info.Protect == PAGE_EXECUTE_READ ||
                               info.Protect == PAGE_READWRITE) {
                        handled = VirtualProtectFromApp((void*)subpage, 4096, target, &old);
                        if (handled) {
                            ++uwp_commit_epoch;
                            if (access == 8) {
                                FlushInstructionCache(GetCurrentProcess(), (void*)subpage, 4096);
                            }
                        }
                    }
                }
            } else if (!handled) {
                MEMORY_BASIC_INFORMATION info;
                DWORD old;
                uintptr_t subpage = address & ~(uintptr_t)4095;
                if (VirtualQuery((void*)subpage, &info, sizeof(info)) && info.State == MEM_COMMIT) {
                    if (info.Protect != win_prot(wanted)) {
                        handled =
                            VirtualProtectFromApp((void*)subpage, 4096, win_prot(wanted), &old);
                        if (handled)
                            ++uwp_commit_epoch;
                    } else if (last_retry_subpage != subpage ||
                               last_retry_epoch != uwp_commit_epoch) {

                        last_retry_subpage = subpage;
                        last_retry_epoch = uwp_commit_epoch;
                        handled = 1;
                    }
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&uwp_commit_lock);
    read_unlock();
    return handled;
}

int runtime_memory_handle_commit_fault(uintptr_t address, int access) {
    return runtime_memory_handle_commit_fault_at(address, access, 0);
}

static int uwp_detach(UwpPage* page, int preserve) {
    if (!page->mapped)
        return 0;
    UwpPhysical* physical = &uwp_physical[page->physical / PAGE];
    if (page->kind == 1) {
        MEMORY_BASIC_INFORMATION info;
        DWORD old;
        if (preserve && VirtualQuery((void*)page->address, &info, sizeof(info)) &&
            info.State == MEM_COMMIT) {
            unsigned char* saved = malloc(PAGE);
            if (!saved)
                uwp_fail("direct-page snapshot", page->address);
            if (!VirtualProtectFromApp((void*)page->address, PAGE, PAGE_READONLY, &old)) {
                uwp_fail("snapshot protection", page->address);
            }
            memcpy(saved, (void*)page->address, PAGE);
            free(physical->saved);
            physical->saved = saved;
            ++uwp_snapshots;
        }
        if (page->placed &&
            !VirtualFree((void*)page->address, PAGE, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            uwp_fail("private page release", page->address);
        }
        page->placed = 0;
    } else if (page->kind == 3 || page->kind == 4) {
        physical->hash_valid = 0;
        if (!uwp_swap_unmap_view(page))
            uwp_fail("preserve swap page", page->address);
    } else if (page->kind == 2) {
        if (!UnmapViewOfFile2(GetCurrentProcess(), (void*)page->address,
                              MEM_PRESERVE_PLACEHOLDER)) {
            uwp_fail("alias page unmap", page->address);
        }
    }
    if (physical->owner == page->address) {
        physical->owner = 0;
        if (physical->references > 1) {
            for (size_t i = 0; i < uwp_count; ++i) {
                if (uwp_pages[i].mapped && &uwp_pages[i] != page &&
                    uwp_pages[i].physical == page->physical) {
                    physical->owner = uwp_pages[i].address;
                    break;
                }
            }
        }
    }
    if (physical->references)
        --physical->references;
    page->mapped = page->kind = page->committed = 0;
    memset(page->protection, 0, sizeof(page->protection));
    return 0;
}

static int uwp_promote(UwpPhysical* physical, uint64_t offset) {
    if (physical->section && (!physical->executable || physical->section_executable))
        return 0;
    HANDLE old_section = physical->section;
    HANDLE section =
        physical->executable
            ? CreateFileMapping2(INVALID_HANDLE_VALUE, NULL, FILE_MAP_ALL_ACCESS,
                                 PAGE_EXECUTE_READWRITE, SEC_COMMIT, PAGE, NULL, NULL, 0)
            : CreateFileMappingFromApp(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_COMMIT,
                                       PAGE, NULL);
    if (!section) {
        fprintf(stderr, "UWP memory: physical alias section failed, error %lu\n", GetLastError());
        return -1;
    }
    void* alias = MapViewOfFile3FromApp(section, GetCurrentProcess(), NULL, 0, PAGE, 0,
                                        PAGE_READWRITE, NULL, 0);
    if (!alias) {
        CloseHandle(section);
        return -1;
    }
    UwpPage* owner = uwp_page(physical->owner);
    physical->section = section;
    physical->section_executable = physical->executable;
    temporary_section = old_section;
    temporary_promotion = alias;
    DWORD old;
    if (old_section) {
        void* previous = MapViewOfFile3FromApp(old_section, GetCurrentProcess(), NULL, 0, PAGE, 0,
                                               PAGE_READONLY, NULL, 0);
        if (!previous)
            uwp_fail("executable alias source", offset);
        memcpy(alias, previous, PAGE);
        UnmapViewOfFile(previous);
        for (size_t i = 0; i < uwp_count; ++i) {
            UwpPage* page = &uwp_pages[i];
            if (!page->mapped || page->physical != offset)
                continue;
            if (!UnmapViewOfFile2(GetCurrentProcess(), (void*)page->address,
                                  MEM_PRESERVE_PLACEHOLDER))
                uwp_fail("executable alias unmap", page->address);
            if (!MapViewOfFile3(section, GetCurrentProcess(), (void*)page->address, 0, PAGE,
                                MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, NULL, 0))
                uwp_fail("executable alias upgrade", page->address);
            uwp_apply_protection(page);
        }
        CloseHandle(old_section);
        temporary_section = NULL;
    } else if (owner) {
        if (owner->kind == 3 || owner->kind == 4) {
            if (!uwp_swap_unmap_view(owner))
                uwp_fail("preserve promoted page", owner->address);
            if (!uwp_swap_transfer(owner->physical, alias, 0)) {
                uwp_fail("promoted page read", owner->address);
            }
            if (physical->hash_valid) {
                ++uwp_swap_hash_checks;
                const uint64_t read = uwp_swap_hash(alias);
                if (read != physical->hash) {
                    ++uwp_swap_hash_errors;
                    uwp_swap_describe_corruption((uint64_t)(physical - uwp_physical) * PAGE,
                                                 physical->hash, read);
                    uwp_fail("promoted page integrity", owner->address);
                }
            }
        } else {
            uwp_commit(owner);
            if (!VirtualProtectFromApp((void*)owner->address, PAGE, PAGE_READONLY, &old)) {
                uwp_fail("alias promotion read", owner->address);
            }
            memcpy(alias, (void*)owner->address, PAGE);
            if (!VirtualFree((void*)owner->address, PAGE, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
                uwp_fail("alias promotion release", owner->address);
            }
            owner->placed = 0;
        }
        uwp_swap_coalesce(owner);
        void* mapped =
            physical->section_executable
                ? MapViewOfFile3(section, GetCurrentProcess(), (void*)owner->address, 0, PAGE,
                                 MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, NULL, 0)
                : MapViewOfFile3FromApp(section, GetCurrentProcess(), (void*)owner->address, 0,
                                        PAGE, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
        if (!mapped) {
            uwp_fail("canonical alias map", owner->address);
        }
        uwp_apply_protection(owner);
        owner->kind = 2;
        owner->committed = 0;
    }
    if (!owner && physical->saved) {
        memcpy(alias, physical->saved, PAGE);
        free(physical->saved);
        physical->saved = NULL;
    }
    UnmapViewOfFile(alias);
    temporary_promotion = NULL;
    physical->section = section;
    if (physical->disk) {
        physical->disk = 0;
        --uwp_swap_disk_pages;
    }
    physical->hash_valid = 0;
    (void)offset;
    ++uwp_aliases;
    return 0;
}

static int host_map(uintptr_t address, uint64_t size, int prot, int reserved, uint64_t offset) {
    if (uwp_reserve(address, size))
        return -1;

    int pageable = mapping_pageable && !(prot & PROT_EXEC);
    for (uint64_t n = 0; n < size; n += PAGE) {
        UwpPage* page = uwp_page(address + n);
        if (page->mapped)
            uwp_detach(page, 1);
        if (reserved)
            continue;
        UwpPhysical* physical = &uwp_physical[(offset + n) / PAGE];
        if (physical->discard_pending && offset + n >= direct_size) {
            if (physical->section) {
                void* view = MapViewOfFile3FromApp(physical->section, GetCurrentProcess(), NULL, 0,
                                                   PAGE, 0, PAGE_READWRITE, NULL, 0);
                if (!view)
                    uwp_fail("reset flexible alias", page->address);
                memset(view, 0, PAGE);
                UnmapViewOfFile(view);
            } else if (physical->saved) {
                memset(physical->saved, 0, PAGE);
            }
        }
        physical->discard_pending = 0;
        if (prot & PROT_EXEC)
            physical->pinned = physical->executable = 1;
        if (physical->references && uwp_promote(physical, offset + n)) {
            uwp_fail("alias section resource limit", page->address);
        }
        if (physical->section || !physical->disk)
            uwp_swap_coalesce(page);
        if (physical->section || physical->disk)
            uwp_isolate(page);
        if (physical->section) {
            void* mapped =
                physical->section_executable
                    ? MapViewOfFile3(physical->section, GetCurrentProcess(), (void*)page->address,
                                     0, PAGE, MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, NULL,
                                     0)
                    : MapViewOfFile3FromApp(physical->section, GetCurrentProcess(),
                                            (void*)page->address, 0, PAGE, MEM_REPLACE_PLACEHOLDER,
                                            PAGE_READWRITE, NULL, 0);
            if (!mapped) {
                uwp_fail("physical alias map", page->address);
            }
            page->kind = 2;
        } else if (physical->disk) {
            page->kind = 4;
            uwp_swap_split(page);
        } else {
            page->kind = 1;
        }
        page->mapped = 1;
        page->physical = offset + n;
        page->pageable = pageable;
        page->committed = 0;
        for (unsigned i = 0; i < 4; ++i)
            page->protection[i] = prot;
        if (page->kind == 2)
            uwp_apply_protection(page);
        if (!physical->owner)
            physical->owner = page->address;
        ++physical->references;
    }
    return 0;
}

static int host_unmap(uintptr_t address, uint64_t size) {
    for (uintptr_t at = address; at < address + size; at += PAGE) {
        UwpPage* page = uwp_page(at);
        if (page)
            uwp_detach(page, 1);
    }
    return 0;
}

static int host_protect(uintptr_t address, uint64_t size, int prot) {
    AcquireSRWLockExclusive(&uwp_commit_lock);
    int result = 0;
    const uintptr_t last = (address + size + 4095) & ~(uintptr_t)4095;
    for (uintptr_t at = address & ~(uintptr_t)4095; at < last;) {
        UwpPage* page = uwp_page(at);
        if (!page || !page->mapped) {
            at += 4096;
            continue;
        }
        if (prot & PROT_EXEC) {
            UwpPhysical* physical = &uwp_physical[page->physical / PAGE];
            physical->pinned = physical->executable = 1;
            if (!physical->section_executable &&
                (physical->section || page->kind == 3 || page->kind == 4) &&
                uwp_promote(physical, page->physical))
                uwp_fail("executable protection promotion", page->address);
        }
        if (page->kind == 3 || page->kind == 4) {
            if (!uwp_swap_protect(page, (unsigned)((at - page->address) / 4096), prot)) {
                result = -1;
                break;
            }
            at += 4096;
            continue;
        }
        uintptr_t end = (page->address + PAGE < last) ? (page->address + PAGE) : last;
        MEMORY_BASIC_INFORMATION info;
        DWORD old;
        if (!VirtualQuery((void*)at, &info, sizeof(info))) {
            result = -1;
            break;
        }
        if (info.State == MEM_COMMIT &&
            !VirtualProtectFromApp((void*)at, end - at, win_prot(prot), &old)) {
            result = -1;
            break;
        }
        for (uintptr_t sub = at; sub < end; sub += 4096) {
            page->protection[(sub - page->address) / 4096] = prot;
        }
        ++uwp_commit_epoch;
        at = end;
    }
    ReleaseSRWLockExclusive(&uwp_commit_lock);
    return result;
}

static void host_discard(uint64_t offset, uint64_t size) {
    if (!uwp_physical)
        return;
    for (uint64_t at = offset; at < offset + size; at += PAGE) {
        UwpPhysical* physical = &uwp_physical[at / PAGE];
        free(physical->saved);
        physical->saved = NULL;
        if (physical->section) {
            CloseHandle(physical->section);
            physical->section = NULL;
        }
        physical->owner = 0;
        physical->references = 0;
        if (physical->disk) {
            physical->disk = 0;
            --uwp_swap_disk_pages;
        }

        if (uwp_swap_slot && at / PAGE < uwp_swap_slot_count) {
            uwp_swap_drop_pool_record(at);
            uwp_swap_slot[at / PAGE] = 0;
        }
        physical->pinned = physical->recent = physical->hash_valid = physical->discard_pending =
            physical->executable = physical->section_executable = 0;
    }
}

static int uwp_backing_write(uintptr_t address, const void* source, uint64_t size) {
    AcquireSRWLockExclusive(&uwp_commit_lock);
    int result = 1;
    while (size) {
        UwpPage* page = uwp_page(address);
        if (!page || !page->mapped) {
            result = 0;
            break;
        }
        uint64_t offset = address - page->address;
        uint64_t n = PAGE - offset;
        if (n > size)
            n = size;
        UwpPhysical* physical = &uwp_physical[page->physical / PAGE];
        physical->recent = 1;
        if (physical->disk && !physical->section) {
            if (!uwp_swap_write_backing(page->physical, offset, source, n)) {
                result = 0;
                break;
            }
        } else if (physical->section) {
            void* view = MapViewOfFile3FromApp(physical->section, GetCurrentProcess(), NULL, 0,
                                               PAGE, 0, PAGE_READWRITE, NULL, 0);
            if (!view) {
                result = 0;
                break;
            }
            memcpy((unsigned char*)view + offset, source, (size_t)n);
            UnmapViewOfFile(view);
        } else {
            uwp_commit(page);
            for (uint64_t copied = 0; copied < n;) {
                uintptr_t at = address + copied;
                uintptr_t subpage = at & ~(uintptr_t)4095;
                uint64_t bytes = 4096 - (at - subpage);
                if (bytes > n - copied)
                    bytes = n - copied;
                MEMORY_BASIC_INFORMATION info;
                DWORD old;
                if (!VirtualQuery((void*)subpage, &info, sizeof(info))) {
                    uwp_fail("host copy protection query", subpage);
                }
                int change =
                    info.Protect != PAGE_READWRITE && info.Protect != PAGE_EXECUTE_READWRITE;
                if (change && !VirtualProtectFromApp((void*)subpage, 4096, PAGE_READWRITE, &old)) {
                    result = 0;
                    break;
                }
                memcpy((void*)at, (const unsigned char*)source + copied, (size_t)bytes);
                if (change && !VirtualProtectFromApp((void*)subpage, 4096, info.Protect, &old)) {
                    uwp_fail("host copy protection restore", subpage);
                }
                copied += bytes;
            }
            if (!result)
                break;
        }
        address += (uintptr_t)n;
        source = (const unsigned char*)source + n;
        size -= n;
    }
    ReleaseSRWLockExclusive(&uwp_commit_lock);
    return result;
}

static int uwp_backing_read(uintptr_t address, void* dest, uint64_t size) {
    AcquireSRWLockExclusive(&uwp_commit_lock);
    int result = 1;
    while (size) {
        UwpPage* page = uwp_page(address);
        if (!page || !page->mapped) {
            result = 0;
            break;
        }
        uint64_t offset = address - page->address;
        uint64_t n = PAGE - offset;
        if (n > size)
            n = size;
        UwpPhysical* physical = &uwp_physical[page->physical / PAGE];
        if (physical->disk && !physical->section) {
            if (!uwp_swap_read_backing(page->physical, offset, dest, n)) {
                result = 0;
                break;
            }
        } else if (physical->section) {
            void* view = MapViewOfFile3FromApp(physical->section, GetCurrentProcess(), NULL, 0,
                                               PAGE, 0, PAGE_READONLY, NULL, 0);
            if (!view) {
                result = 0;
                break;
            }
            memcpy(dest, (const unsigned char*)view + offset, (size_t)n);
            UnmapViewOfFile(view);
        } else if (page->kind == 1 && !page->committed) {
            if (physical->saved)
                memcpy(dest, physical->saved + offset, (size_t)n);
            else
                memset(dest, 0, (size_t)n);
        } else {
            uwp_commit(page);
            for (uint64_t copied = 0; copied < n;) {
                uintptr_t at = address + copied;
                uintptr_t subpage = at & ~(uintptr_t)4095;
                uint64_t bytes = 4096 - (at - subpage);
                if (bytes > n - copied)
                    bytes = n - copied;
                MEMORY_BASIC_INFORMATION info;
                DWORD old;
                if (!VirtualQuery((void*)subpage, &info, sizeof(info))) {
                    uwp_fail("host read protection query", subpage);
                }
                int change = (info.Protect == PAGE_NOACCESS);
                if (change && !VirtualProtectFromApp((void*)subpage, 4096, PAGE_READONLY, &old)) {
                    result = 0;
                    break;
                }
                memcpy((unsigned char*)dest + copied, (const void*)at, (size_t)bytes);
                if (change && !VirtualProtectFromApp((void*)subpage, 4096, info.Protect, &old)) {
                    uwp_fail("host read protection restore", subpage);
                }
                copied += bytes;
            }
            if (!result)
                break;
        }
        address += (uintptr_t)n;
        dest = (unsigned char*)dest + n;
        size -= n;
    }
    ReleaseSRWLockExclusive(&uwp_commit_lock);
    return result;
}

static void uwp_memory_report_internal(void) {
    uwp_swap_report();
    printf("UWP memory: commit faults=%" PRIu64 ", promoted alias pages=%" PRIu64
           ", snapshots=%" PRIu64 ", restored=%" PRIu64 ", virtual pages=%zu\n",
           uwp_faults, uwp_aliases, uwp_snapshots, uwp_moves, uwp_count);
}
