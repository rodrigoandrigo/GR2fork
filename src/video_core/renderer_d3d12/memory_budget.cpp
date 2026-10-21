#include "video_core/renderer_d3d12/memory_budget.h"

#include <algorithm>
#include <atomic>
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>

namespace D3D12 {
namespace {
constexpr u64 MiB = 1024 * 1024;
u64 AvailableTarget(u64 retained, u64 usage, u64 limit, u64 reserve) {
    const u64 available = limit > usage ? limit - usage : 0;
    if (usage <= limit && available >= reserve) {
        const u64 growth = std::min(available - reserve, 2048 * MiB);
        return retained > UINT64_MAX - growth ? UINT64_MAX : retained + growth;
    }
    const u64 excess = usage > limit ? usage - limit : 0;
    const u64 shortage = reserve - available;
    const u64 deficit = excess > UINT64_MAX - shortage ? UINT64_MAX : excess + shortage;
    return retained > deficit ? retained - deficit : 0;
}
} // namespace
struct MemoryBudget::Events {
    struct Signal {
        std::atomic_bool changed{};
        std::atomic<u64> limit{UINT64_MAX};
    };
    std::shared_ptr<Signal> signal{std::make_shared<Signal>()};
    winrt::Windows::System::MemoryManager::AppMemoryUsageIncreased_revoker increased;
    winrt::Windows::System::MemoryManager::AppMemoryUsageDecreased_revoker decreased;
    winrt::Windows::System::MemoryManager::AppMemoryUsageLimitChanging_revoker limit;
};
MemoryBudget::MemoryBudget() = default;
MemoryBudget::~MemoryBudget() = default;
void MemoryBudget::Initialize(ID3D12Device* device) {
    if (initialized)
        return;
    initialized = true;
    events = std::make_unique<Events>();
    const std::weak_ptr<Events::Signal> weak = events->signal;
    try {
        using winrt::Windows::System::MemoryManager;
        const auto changed = [weak](const auto&, const auto&) {
            if (const auto signal = weak.lock())
                signal->changed.store(true);
        };
        events->increased = MemoryManager::AppMemoryUsageIncreased(winrt::auto_revoke, changed);
        events->decreased = MemoryManager::AppMemoryUsageDecreased(winrt::auto_revoke, changed);
        events->limit = MemoryManager::AppMemoryUsageLimitChanging(
            winrt::auto_revoke, [weak](const auto&, const auto& args) {
                if (const auto signal = weak.lock()) {
                    signal->limit.store(args.NewLimit());
                    signal->changed.store(true);
                }
            });
    } catch (const winrt::hresult_error&) {
    }
    D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture,
                                              sizeof(architecture))))
        unified = architecture.UMA;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter));
}
u64 MemoryBudget::Target(const Sample& sample, u64 retained) {
    u64 target = sample.background ? 64 * MiB : 2048 * MiB;
    if (sample.process_limit) {
        const u64 reserve = std::clamp(sample.process_limit / 16, 64 * MiB, 256 * MiB);
        target = std::min(
            target, AvailableTarget(retained, sample.process_usage, sample.process_limit, reserve));
    }
    if (sample.system_total)
        target = std::min(
            target, AvailableTarget(retained,
                                    sample.system_total -
                                        std::min(sample.system_total, sample.system_available),
                                    sample.system_total,
                                    std::clamp(sample.system_total / 32, 128 * MiB, 512 * MiB)));
    if (sample.local_valid || sample.local_budget)
        target = std::min(target, AvailableTarget(retained, sample.local_usage, sample.local_budget,
                                                  std::min(sample.local_budget / 8, 256 * MiB)));
    if (!sample.unified && (sample.nonlocal_valid || sample.nonlocal_budget))
        target = std::min(target,
                          AvailableTarget(retained, sample.nonlocal_usage, sample.nonlocal_budget,
                                          std::min(sample.nonlocal_budget / 8, 128 * MiB)));
    if (sample.pressure >= 2)
        target = std::min(target, retained / (sample.pressure >= 3 ? 2 : 4) *
                                      (sample.pressure >= 3 ? 1 : 3));
    return target;
}
bool MemoryBudget::Poll(u64 retained, bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (events && events->signal->changed.exchange(false))
        force = true;
    if (!force && now - sampled < std::chrono::milliseconds{250}) {
        if (retained > statistics.target &&
            (!statistics.pinned ||
             (retained > statistics.retained && retained - statistics.retained > MiB))) {
            statistics.retained = retained;
            return true;
        }
        return false;
    }
    const bool first = sampled == std::chrono::steady_clock::time_point{};
    sampled = now;
    Sample sample{};
    sample.unified = unified;
    sample.background = background;
    MEMORYSTATUSEX system{sizeof(system)};
    if (GlobalMemoryStatusEx(&system)) {
        sample.system_total = system.ullTotalPhys;
        sample.system_available = system.ullAvailPhys;
    }
    try {
        using winrt::Windows::System::MemoryManager;
        sample.process_usage = MemoryManager::AppMemoryUsage();
        sample.process_limit = MemoryManager::AppMemoryUsageLimit();
        sample.pressure = static_cast<u32>(MemoryManager::AppMemoryUsageLevel());
    } catch (const winrt::hresult_error&) {
    }
    if (events) {
        const u64 pending_limit = events->signal->limit.exchange(UINT64_MAX);
        if (pending_limit != UINT64_MAX)
            sample.process_limit = sample.process_limit
                                       ? std::min(sample.process_limit, pending_limit)
                                       : pending_limit;
    }
    if (adapter) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            sample.local_valid = true;
            sample.local_usage = info.CurrentUsage;
            sample.local_budget = info.Budget;
        }
        if (!unified && SUCCEEDED(adapter->QueryVideoMemoryInfo(
                            0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &info))) {
            sample.nonlocal_valid = true;
            sample.nonlocal_usage = info.CurrentUsage;
            sample.nonlocal_budget = info.Budget;
        }
    }
    u64 target = Target(sample, retained);
    if (!first && target > statistics.target)
        target = std::min(target, statistics.target + 16 * MiB);
    statistics.sample = sample;
    statistics.retained = retained;
    statistics.target = target;
    return true;
}
} // namespace D3D12
