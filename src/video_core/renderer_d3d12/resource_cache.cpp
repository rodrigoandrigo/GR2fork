#include "video_core/renderer_d3d12/resource_cache.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <winrt/base.h>

namespace D3D12 {
void ResourceCache::Initialize(D3D12DeviceContext& device, Read reader, Write writer,
                               Resolve resolver) {
    context = &device;
    read = std::move(reader);
    write = std::move(writer);
    resolve = std::move(resolver);
    allocator.Initialize(device.Device(), 256ULL * 1024 * 1024);
}
void ResourceCache::BeginBatch() {
    context->WaitForGpu();
    ++batch;
    for (auto& [key, buffer] : buffers)
        buffer->targets.clear();
}
ResourceCache::Identity ResourceCache::Canonicalize(VAddr address, u64 size) const {
    if (address > UINT64_MAX - size)
        throw std::runtime_error("Guest resource range overflow");
    if (resolve && address && size)
        if (const auto physical = resolve(address, size))
            return {*physical, true};
    return {address, false};
}
void ResourceCache::MarkWritten(const View& view) {
    if (!view.buffer)
        return;
    view.buffer->written = true;
    if (!view.buffer->host) {
        if (!view.guest_address || view.offset > view.buffer->size ||
            view.size > view.buffer->size - view.offset)
            throw std::runtime_error("Invalid guest writeback view");
        const auto found = std::ranges::find_if(view.buffer->targets, [&](const auto& target) {
            return target.address == view.guest_address && target.offset == view.offset &&
                   target.size == view.size;
        });
        if (found == view.buffer->targets.end())
            view.buffer->targets.push_back({view.guest_address, view.offset, view.size});
    }
}
ResourceCache::Buffer& ResourceCache::GetHostBuffer(VAddr identity, std::span<const u8> data) {
    auto& buffer = GetBuffer(identity, data.size());
    buffer.host = true;
    buffer.host_data.assign(data.begin(), data.end());
    return buffer;
}
std::vector<ResourceCache::View> ResourceCache::GetViews(std::span<const Range> ranges) {
    std::vector<Identity> identities;
    std::array<std::vector<Range>, 2> groups;
    for (const auto& range : ranges) {
        if (range.size > 16ULL * 1024 * 1024 || range.address > UINT64_MAX - range.size - 3)
            throw std::runtime_error("Invalid D3D12 guest buffer view");
        const auto identity = Canonicalize(range.address, range.size);
        identities.push_back(identity);
        if (!range.address || !range.size)
            continue;
        if (identity.address > UINT64_MAX - range.size - 3)
            throw std::runtime_error("Invalid physical D3D12 buffer view");
        const VAddr start = identity.address & ~u64{3};
        groups[identity.physical].push_back(
            {start, ((identity.address + range.size + 3) & ~u64{3}) - start});
    }
    std::vector<Buffer*> allocations;
    for (u32 physical = 0; physical < 2; ++physical) {
        std::ranges::sort(groups[physical], {}, &Range::address);
        std::vector<Range> merged;
        for (const auto& range : groups[physical]) {
            if (merged.empty() || range.address >= merged.back().address + merged.back().size)
                merged.push_back(range);
            else
                merged.back().size = std::max(merged.back().address + merged.back().size,
                                              range.address + range.size) -
                                     merged.back().address;
        }
        for (const auto& group : merged)
            allocations.push_back(&GetBuffer(group.address, group.size, physical != 0));
    }
    std::vector<View> result;
    for (u32 i = 0; i < ranges.size(); ++i) {
        const auto& range = ranges[i];
        const auto& identity = identities[i];
        View view{};
        for (auto* buffer : allocations)
            if (range.address && range.size && identity.physical == buffer->physical &&
                identity.address >= buffer->address &&
                identity.address + range.size <= buffer->address + buffer->size) {
                view = {buffer, identity.address - buffer->address, range.size, range.address};
                break;
            }
        result.push_back(view);
    }
    return result;
}
ResourceCache::Buffer& ResourceCache::GetBuffer(VAddr address, u64 size, bool physical) {
    if (!address || !size || size > 16ULL * 1024 * 1024 ||
        size > std::numeric_limits<VAddr>::max() - address)
        throw std::runtime_error("Invalid D3D12 guest buffer range");
    const auto key = std::tuple{physical, address, size};
    if (const auto found = buffers.find(key); found != buffers.end()) {
        found->second->used = ++age;
        found->second->batch = batch;
        return *found->second;
    }
    for (const auto& [existing, buffer] : buffers)
        if (buffer->physical == physical && buffer->batch == batch &&
            address < buffer->address + buffer->size && buffer->address < address + size)
            throw std::runtime_error("Overlapping D3D12 buffer views require a shared allocation");
    while (buffers.size() >= 128 ||
           allocator.Stats().live_bytes + native_bytes + size * 3 + 3 * 65536 >
               allocator.Stats().budget_bytes) {
        auto oldest = buffers.end();
        for (auto it = buffers.begin(); it != buffers.end(); ++it)
            if (it->second->batch != batch && !it->second->written &&
                (oldest == buffers.end() || it->second->used < oldest->second->used))
                oldest = it;
        if (oldest == buffers.end())
            throw std::runtime_error("D3D12 guest resource budget exhausted by pending writes");
        native_bytes -= oldest->second->allocation_bytes;
        buffers.erase(oldest);
    }
    auto result = std::make_unique<Buffer>();
    result->address = address;
    result->physical = physical;
    result->size = size;
    result->used = ++age;
    result->batch = batch;
    const u64 allocation_size = (size + 3) & ~u64{3};
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = allocation_size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> native;
    winrt::check_hresult(context->Device()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&native)));
    winrt::check_hresult(allocator.CreateBuffer(allocation_size, D3D12_HEAP_TYPE_UPLOAD,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, result->upload));
    winrt::check_hresult(allocator.CreateBuffer(allocation_size, D3D12_HEAP_TYPE_READBACK,
                                                D3D12_RESOURCE_STATE_COPY_DEST, result->readback));
    result->native = std::move(native);
    result->allocation_bytes =
        context->Device()->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
    auto& output = *result;
    buffers.emplace(key, std::move(result));
    native_bytes += output.allocation_bytes;
    return output;
}
void ResourceCache::Barrier(Buffer& buffer, ID3D12GraphicsCommandList* commands,
                            D3D12_RESOURCE_STATES state) {
    if (buffer.state == state) {
        if (state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.UAV.pResource = buffer.native.Get();
            commands->ResourceBarrier(1, &barrier);
        }
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = buffer.native.Get();
    barrier.Transition.StateBefore = buffer.state;
    barrier.Transition.StateAfter = state;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commands->ResourceBarrier(1, &barrier);
    buffer.state = state;
}
void ResourceCache::Upload(Buffer& buffer, ID3D12GraphicsCommandList* commands) {
    std::vector<u8> data(buffer.size);
    if (buffer.host)
        data = buffer.host_data;
    else if (buffer.physical)
        std::memcpy(data.data(), reinterpret_cast<const void*>(buffer.address), data.size());
    else
        read(buffer.address, data);
    if (data != buffer.snapshot || Changed(buffer)) {
        void* mapped;
        const D3D12_RANGE empty{0, 0};
        winrt::check_hresult(buffer.upload->Map(0, &empty, &mapped));
        std::memcpy(mapped, data.data(), data.size());
        const D3D12_RANGE contents{0, data.size()};
        buffer.upload->Unmap(0, &contents);
        Barrier(buffer, commands, D3D12_RESOURCE_STATE_COPY_DEST);
        commands->CopyBufferRegion(buffer.native.Get(), 0, buffer.upload.Get(), 0, data.size());
        buffer.snapshot = std::move(data);
    }
    {
        std::scoped_lock lock{invalidation_mutex};
        buffer.epoch = epoch;
    }
    Barrier(buffer, commands, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
void ResourceCache::PrepareDownload(ID3D12GraphicsCommandList* commands) {
    for (auto& [key, buffer] : buffers) {
        if (!buffer->written)
            continue;
        Barrier(*buffer, commands, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->CopyBufferRegion(buffer->readback.Get(), 0, buffer->native.Get(), 0,
                                   buffer->size);
    }
    for (auto& [key, buffer] : buffers)
        Barrier(*buffer, commands, D3D12_RESOURCE_STATE_COMMON);
}
void ResourceCache::CommitDownloads() {
    for (auto& [key, buffer] : buffers) {
        if (!buffer->written)
            continue;
        if (Changed(*buffer))
            throw std::runtime_error("Guest buffer mapping changed before D3D12 writeback");
        void* mapped;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(buffer->size)};
        winrt::check_hresult(buffer->readback->Map(0, &range, &mapped));
        buffer->snapshot.assign(static_cast<u8*>(mapped), static_cast<u8*>(mapped) + buffer->size);
        const D3D12_RANGE empty{0, 0};
        buffer->readback->Unmap(0, &empty);
        if (buffer->host)
            buffer->host_data = buffer->snapshot;
        else
            for (const auto& target : buffer->targets)
                write(target.address,
                      {buffer->snapshot.data() + target.offset, static_cast<size_t>(target.size)});
        buffer->written = false;
    }
}
bool ResourceCache::Changed(const Buffer& buffer) {
    if (buffer.host || buffer.physical)
        return false;
    std::scoped_lock lock{invalidation_mutex};
    if (buffer.epoch < invalidation_floor)
        return true;
    for (const auto& item : invalidations)
        if (item.epoch > buffer.epoch && item.address < buffer.address + buffer.size &&
            buffer.address < item.address + item.size)
            return true;
    return false;
}
bool ResourceCache::InvalidateMemory(VAddr address, u64 size, bool) {
    if (!size || size > std::numeric_limits<VAddr>::max() - address)
        return false;
    std::scoped_lock lock{invalidation_mutex};
    invalidations.push_back({address, size, ++epoch});
    if (invalidations.size() > 4096) {
        invalidation_floor = invalidations[2047].epoch;
        invalidations.erase(invalidations.begin(), invalidations.begin() + 2048);
    }
    return true;
}
void ResourceCache::MapMemory(VAddr address, u64 size) {
    InvalidateMemory(address, size);
}
void ResourceCache::RegisterMemory(VAddr address, u64 size) {
    InvalidateMemory(address, size);
}
void ResourceCache::UnmapMemory(VAddr address, u64 size) {
    InvalidateMemory(address, size);
}
} // namespace D3D12
