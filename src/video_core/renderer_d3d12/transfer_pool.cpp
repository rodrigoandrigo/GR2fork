#include "video_core/renderer_d3d12/transfer_pool.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <winrt/base.h>

namespace D3D12 {
namespace {
constexpr size_t ResourceLimit = 1024;
}
TransferPool::~TransferPool() {
    if (!device || entries.empty())
        return;
    u64 last{};
    for (const auto& entry : entries)
        last = std::max(last, entry.fence);
    try {
        device->Wait(last);
    } catch (...) {
    }
}
void TransferPool::Initialize(D3D12DeviceContext& context, u64 limit) {
    if (device || !limit)
        throw std::runtime_error("Invalid D3D12 transfer pool initialization");
    device = &context;
    budget = limit;
    entries.reserve(ResourceLimit);
    owned.reserve(ResourceLimit);
    allocator.Initialize(context.Device(), budget);
}
void TransferPool::Evict(size_t index) {
    owned.erase(entries[index].resource.Get());
    entries.erase(entries.begin() + index);
}
D3D12Resource TransferPool::Acquire(u64 size, D3D12_HEAP_TYPE heap) {
    if (!device || !size || size > budget || size > UINT64_MAX - 65535 ||
        (heap != D3D12_HEAP_TYPE_UPLOAD && heap != D3D12_HEAP_TYPE_READBACK))
        throw std::runtime_error("Invalid D3D12 transfer buffer request");
    size = (size + 65535) & ~u64{65535};
    if (size > budget)
        throw std::runtime_error("D3D12 transfer buffer exceeds aligned pool budget");
    for (;;) {
        const auto completed = device->CompletedValue();
        size_t best = entries.size();
        for (size_t i = 0; i < entries.size(); ++i)
            if (entries[i].heap == heap && entries[i].fence <= completed &&
                entries[i].resource.AllocationBytes() == size &&
                (best == entries.size() ||
                 entries[i].resource.AllocationBytes() < entries[best].resource.AllocationBytes()))
                best = i;
        if (best != entries.size()) {
            auto resource = std::move(entries[best].resource);
            entries.erase(entries.begin() + best);
            ++statistics.reuses;
            return resource;
        }
        const auto current = allocator.Stats();
        if (current.live_bytes <= budget - size && current.live_resources < ResourceLimit) {
            D3D12Resource resource;
            winrt::check_hresult(allocator.CreateBuffer(size, heap,
                                                        heap == D3D12_HEAP_TYPE_UPLOAD
                                                            ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                            : D3D12_RESOURCE_STATE_COPY_DEST,
                                                        resource));
            owned.insert(resource.Get());
            ++statistics.allocations;
            return resource;
        }
        const auto free = std::ranges::find_if(
            entries, [&](const auto& entry) { return entry.fence <= completed; });
        if (free != entries.end()) {
            Evict(size_t(free - entries.begin()));
            continue;
        }
        if (entries.empty())
            throw std::runtime_error("D3D12 transfer pool exhausted by active CPU/GPU owners");
        const auto oldest = std::ranges::min_element(entries, {}, &Entry::fence);
        ++statistics.waits;
        device->Wait(oldest->fence);
    }
}
void TransferPool::Retire(D3D12Resource&& resource, u64 fence) {
    if (!resource.Get())
        return;
    if (!owned.contains(resource.Get()) || fence > device->LastSignaledValue() ||
        entries.size() >= ResourceLimit)
        throw std::runtime_error("Invalid D3D12 transfer buffer retirement");
    D3D12_HEAP_PROPERTIES properties{};
    D3D12_HEAP_FLAGS flags{};
    winrt::check_hresult(resource->GetHeapProperties(&properties, &flags));
    if (properties.Type != D3D12_HEAP_TYPE_UPLOAD && properties.Type != D3D12_HEAP_TYPE_READBACK)
        throw std::runtime_error("Invalid D3D12 transfer heap");
    entries.push_back({std::move(resource), fence, properties.Type});
    ++statistics.retirements;
}
} // namespace D3D12
