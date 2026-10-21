#pragma once

#include <unordered_set>
#include <vector>
#include "common/types.h"
#include "d3d12_device_context.h"
#include "d3d12_resource_allocator.h"

namespace D3D12 {
class TransferPool {
public:
    struct Statistics {
        u64 allocations{}, reuses{}, retirements{}, waits{};
    };
    ~TransferPool();
    void Initialize(D3D12DeviceContext& device, u64 budget = 256ULL * 1024 * 1024);
    D3D12Resource Acquire(u64 size, D3D12_HEAP_TYPE heap);
    void Retire(D3D12Resource&& resource, u64 fence);
    void TrimMemory(u64 target);
    const Statistics& GetStatistics() const {
        return statistics;
    }
    D3D12ResourceStats ResourceStatistics() const {
        return allocator.Stats();
    }

private:
    struct Entry {
        D3D12Resource resource;
        u64 fence;
        D3D12_HEAP_TYPE heap;
    };
    void Evict(size_t index);
    D3D12DeviceContext* device{};
    D3D12ResourceAllocator allocator;
    std::vector<Entry> entries;
    std::unordered_set<ID3D12Resource*> owned;
    Statistics statistics{};
    u64 budget{};
};
} // namespace D3D12
