// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>
#include "lazyCounters.h"
#include "lazyRegions.h"
#include "pagerStore.h"

namespace Core::Uwp {

class GuestMemory final : private Xbox::Lazy::HostOps {
public:
    static constexpr uint64_t PageSize = 16 * 1024;
    static constexpr uint64_t WindowSize = Xbox::Lazy::Regions::kWindow;

    GuestMemory(uint64_t size, uint64_t direct_size, uintptr_t canonical);
    ~GuestMemory() override;

    bool OpenStore(const wchar_t* path);
    bool Map(uintptr_t address, uint64_t size, uint64_t physical, int protection);
    bool Unmap(uintptr_t address, uint64_t size);
    bool ProtectRange(uintptr_t address, uint64_t size, int protection);
    bool Fault(uintptr_t address, int access, bool& failed);
    bool Copy(void* destination, const void* source, uint64_t size);
    bool Discard(uint64_t physical, uint64_t size);
    std::vector<uint64_t> Acquire(uintptr_t address, uint64_t size, bool populate);
    void Release(const std::vector<uint64_t>& pins);
    uint64_t Reclaim(uint64_t bytes, bool urgent);
    void Start();
    void Stop();
    void Report(void (*log)(const char*));

private:
    struct VirtualPage {
        uintptr_t address{};
        uint64_t physical{};
        VirtualPage* previous{};
        VirtualPage* next{};
        std::array<uint8_t, 4> protection{3, 3, 3, 3};
        bool placed{};
        bool resident{};
        bool view{};
        bool canonical{};
    };

    struct PhysicalPage {
        VirtualPage* first{};
        VirtualPage* owner{};
        HANDLE section{};
        bool discard_pending{};
        bool discarded{true};
        bool discard_queued{};
        uint32_t next_discard{UINT32_MAX};
    };

    struct FaultRequest {
        uintptr_t address{};
        int access{};
        FaultRequest* next{};
        bool done{};
        bool result{};
        bool failed{};
        DWORD error{};
        uint64_t retry_at{};
    };

    struct SweptWindow {
        uint64_t window{};
        uint64_t since{};
    };

    static uint64_t Logical(uint64_t physical);
    static uint64_t Physical(uint64_t logical);
    static DWORD WindowsProtection(uint8_t protection);
    static bool ValidRange(uint64_t address, uint64_t size);
    static bool IsolatePlaceholder(uintptr_t address, uint64_t size);

    VirtualPage* Find(uintptr_t address);
    VirtualPage& Add(uintptr_t address, uint64_t physical, bool canonical);
    void Erase(VirtualPage& page);
    VirtualPage& Owner(uint64_t physical);
    bool ReservePrivate(VirtualPage& page);
    bool ApplyProtection(VirtualPage& page);
    bool ReleaseMapping(VirtualPage& page);
    bool Promote(PhysicalPage& physical);
    HANDLE AcquireSection(uint64_t physical);
    void ReleaseSection(uint64_t physical);
    void* MapScratch(HANDLE section);
    bool AllocationFailed(const char* api, uint64_t address, HANDLE section);
    void PruneViews();
    bool MapAlias(VirtualPage& page);
    bool Rehome(VirtualPage& page);
    bool Ensure(VirtualPage& page);
    bool FaultLocked(uintptr_t address, int access, bool& failed);
    bool ReadWindow(uint64_t physical, void* destination);
    bool CopyPhysical(uint64_t physical, void* data, uint64_t size, bool write);
    bool DiscardPage(uint64_t physical);
    void QueueDiscard(uint64_t physical);
    void ProcessPendingWindow(uint64_t window) noexcept;
    bool RefreshProtection(uint64_t physical);
    bool HasRestrictedAlias(uint64_t physical) const;
    uint64_t Resident() const;
    void FaultLoop(std::stop_token stop);
    void PagerLoop(std::stop_token stop);
    void UpdatePressure();
    bool QueueFault(uintptr_t address, int access, bool& failed);

    bool Commit(uint64_t address, uint64_t size) override;
    void Decommit(uint64_t address, uint64_t size, bool view) override;
    bool Protect(uint64_t address, uint64_t size, Xbox::Lazy::Mode mode) override;
    bool CommitFromStore(uint64_t window, uint64_t address, uint64_t size, uint32_t slot) override;
    void FreeSlot(uint32_t slot) override;
    bool Spill(uint64_t window, uint64_t address, uint64_t size, uint32_t& slot) override;
    bool Tight(uint64_t size) override;

    std::recursive_mutex mutex;
    std::shared_mutex copy_mutex;
    const uint64_t size;
    const uint64_t direct_size;
    const uintptr_t canonical;
    Xbox::Lazy::Regions regions;
    Xbox::PagerStore store;
    std::vector<PhysicalPage> physical_pages;
    std::vector<HANDLE> window_sections;
    std::vector<HANDLE> free_sections;
    HANDLE scratch_section{};
    void* scratch_address{};
    void* scratch_view{};
    uint64_t sections_created{};
    uint64_t sections_reused{};
    uint64_t sections_live{};
    uint64_t scratch_maps{};
    uint64_t scratch_reuses{};
    uint64_t views_pruned{};
    uint64_t prune_hand{};
    uint64_t fault_retries{};
    const char* failure_api{"none"};
    uint64_t failure_address{};
    HANDLE failure_section{};
    DWORD failure_error{};
    uint64_t failure_count{};
    uint64_t last_failure_report{};
    uint64_t sections_closed{};
    uint64_t peak_views{};
    uint64_t previous_views{};
    uint64_t previous_created{};
    uint64_t previous_reused{};
    std::map<uintptr_t, VirtualPage> pages;
    std::map<uint64_t, uint64_t> protected_windows;
    std::deque<SweptWindow> swept;
    std::vector<uint32_t> discard_windows;
    uint64_t pending_discard_count{};
    uint64_t release_calls{};
    uint64_t discard_visits{};
    std::array<uint8_t, WindowSize> transfer{};
    uint64_t resident_bytes{};
    uint64_t view_count{};
    uint64_t view_limit{6000};
    uint64_t hand{};
    uint64_t commit_epoch{};
    uint64_t app_usage{};
    uint64_t app_limit{};
    uint64_t app_target{};
    uint64_t budget{};
    uint64_t configured_budget{};
    bool budget_initialized{};
    const char* last_operation{"none"};
    uintptr_t last_address{};
    DWORD last_protection{};
    std::mutex fault_mutex;
    std::condition_variable fault_wake;
    std::condition_variable fault_done;
    FaultRequest* fault_first{};
    FaultRequest* fault_last{};
    bool accepting_faults{};
    std::jthread fault_thread;
    std::jthread pager_thread;
    std::jthread diagnostic_thread;
};

} // namespace Core::Uwp
