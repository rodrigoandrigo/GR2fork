#pragma once

#include <chrono>
#include <memory>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include "common/types.h"

namespace D3D12 {
class MemoryBudget {
  public:
    MemoryBudget();
    ~MemoryBudget();
    struct Sample {
        u64 process_usage{}, process_limit{}, system_available{}, system_total{};
        u64 local_usage{}, local_budget{}, nonlocal_usage{}, nonlocal_budget{};
        u32 pressure{};
        bool unified{}, background{}, local_valid{}, nonlocal_valid{};
    };
    struct Statistics {
        Sample sample;
        u64 retained{}, target{}, reclaimed{}, trims{}, pinned{};
    };
    void Initialize(ID3D12Device* device);
    bool Poll(u64 retained, bool force = false);
    void SetBackground(bool value) { background = value; }
    bool Background() const { return background; }
    static u64 Target(const Sample& sample, u64 retained);
    Statistics& Stats() { return statistics; }
    const Statistics& Stats() const { return statistics; }

  private:
    struct Events;
    std::unique_ptr<Events> events;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
    bool unified{}, background{}, initialized{};
    std::chrono::steady_clock::time_point sampled{};
    Statistics statistics{};
};
} // namespace D3D12
