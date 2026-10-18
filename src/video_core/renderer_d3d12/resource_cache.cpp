#include "video_core/renderer_d3d12/resource_cache.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <winrt/base.h>

namespace D3D12 {
namespace {
std::vector<ResourceCache::Buffer::WriteTarget>
UniqueRanges(std::vector<ResourceCache::Buffer::WriteTarget> ranges) {
    std::ranges::sort(ranges, [](const auto& a, const auto& b) {
        return a.offset != b.offset ? a.offset < b.offset : a.size > b.size;
    });
    std::vector<ResourceCache::Buffer::WriteTarget> result;
    u64 end{};
    for (auto range : ranges) {
        const u64 next_end = range.offset + range.size;
        if (next_end <= end)
            continue;
        const u64 skip = end > range.offset ? end - range.offset : 0;
        range.address += skip;
        range.offset += skip;
        range.size -= skip;
        result.push_back(range);
        end = next_end;
    }
    return result;
}
} // namespace
void ResourceCache::Initialize(D3D12DeviceContext& device, Read reader, Write writer,
                               Resolve resolver, Overlaps overlap) {
    context = &device;
    read = std::move(reader);
    write = std::move(writer);
    resolve = std::move(resolver);
    overlaps = std::move(overlap);
    allocator.Initialize(device.Device(), 256ULL * 1024 * 1024);
}
void ResourceCache::BeginBatch() {
    context->WaitForGpu();
    ++batch;
    for (auto& [key, buffer] : buffers) {
        buffer->targets.clear();
        buffer->sources.clear();
    }
}
ResourceCache::Identity ResourceCache::Canonicalize(VAddr address, u64 size) const {
    if (address > UINT64_MAX - size)
        throw std::runtime_error("Guest resource range overflow");
    if (resolve && address && size)
        if (const auto physical = resolve(address, size))
            return {*physical, true};
    return {address, false};
}
bool ResourceCache::Aliases(VAddr first, u64 first_size, VAddr second, u64 second_size) const {
    if (!first_size || !second_size || !first || !second)
        return false;
    if (first > UINT64_MAX - first_size || second > UINT64_MAX - second_size)
        throw std::runtime_error("Guest alias range overflow");
    if (first < second + second_size && second < first + first_size)
        return true;
    if (overlaps)
        return overlaps(first, first_size, second, second_size);
    const auto a = Canonicalize(first, first_size);
    const auto b = Canonicalize(second, second_size);
    return a.physical == b.physical && a.address < b.address + second_size &&
           b.address < a.address + first_size;
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
    if (resolve_backing)
        return GetBackingViews(ranges);
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
std::vector<ResourceCache::View> ResourceCache::GetBackingViews(std::span<const Range> ranges) {
    struct Segment {
        VAddr address;
        u64 size, offset;
    };
    std::vector<std::vector<Segment>> backing(ranges.size());
    for (u32 i = 0; i < ranges.size(); ++i) {
        const auto& range = ranges[i];
        if (range.size > 16ULL * 1024 * 1024 || range.address > UINT64_MAX - range.size)
            throw std::runtime_error("Invalid D3D12 guest buffer view");
        if (!range.address || !range.size)
            continue;
        u64 offset{};
        for (const auto& part : resolve_backing(range.address, range.size)) {
            if (!part.size || part.size > range.size - offset ||
                part.address > UINT64_MAX - part.size)
                throw std::runtime_error("Invalid D3D12 buffer backing segment");
            backing[i].push_back({part.address, part.size, offset});
            offset += part.size;
        }
        if (offset != range.size)
            throw std::runtime_error("Incomplete D3D12 buffer backing");
    }
    std::vector<std::vector<std::pair<u32, s64>>> edges(ranges.size());
    for (u32 i = 0; i < ranges.size(); ++i) {
        for (u32 j = 0; j < i; ++j) {
            std::optional<s64> delta;
            bool compatible = true;
            for (const auto& a : backing[i]) {
                for (const auto& b : backing[j]) {
                    const auto begin = std::max(a.address, b.address);
                    const auto end = std::min(a.address + a.size, b.address + b.size);
                    if (begin >= end)
                        continue;
                    const s64 difference =
                        s64(a.offset + begin - a.address) - s64(b.offset + begin - b.address);
                    if (delta && *delta != difference)
                        compatible = false;
                    delta = difference;
                }
            }
            if (delta && compatible) {
                for (const auto& a : backing[i]) {
                    for (const auto& b : backing[j]) {
                        const s64 a_begin = a.offset;
                        const s64 b_begin = *delta + s64(b.offset);
                        const s64 begin = std::max(a_begin, b_begin);
                        if (begin < std::min(a_begin + s64(a.size), b_begin + s64(b.size)) &&
                            a.address + u64(begin - a_begin) != b.address + u64(begin - b_begin))
                            compatible = false;
                    }
                }
            }
            if (delta && compatible) {
                edges[i].emplace_back(j, *delta);
                edges[j].emplace_back(i, -*delta);
            }
        }
    }
    std::vector<View> views(ranges.size());
    std::vector<std::optional<s64>> origins(ranges.size());
    for (u32 root = 0; root < ranges.size(); ++root) {
        if (backing[root].empty() || origins[root])
            continue;
        origins[root] = backing[root][0].address & 3;
        std::vector<u32> group{root};
        s64 begin = *origins[root];
        s64 end = begin + ranges[root].size;
        for (u32 position = 0; position < group.size(); ++position) {
            const u32 current = group[position];
            for (const auto& [next, delta] : edges[current]) {
                const s64 origin = *origins[current] + delta;
                if (origins[next]) {
                    if (*origins[next] != origin)
                        throw std::runtime_error(
                            "D3D12 physical aliases cannot share a linear buffer allocation");
                    continue;
                }
                origins[next] = origin;
                group.push_back(next);
                begin = std::min(begin, origin);
                end = std::max(end, origin + s64(ranges[next].size));
            }
        }
        begin &= ~s64{3};
        const u64 bytes = (u64(end - begin) + 3) & ~u64{3};
        if (bytes > 16ULL * 1024 * 1024)
            throw std::runtime_error("Shared D3D12 buffer aliases exceed allocation budget");
        for (u32 i = 0; i < group.size(); ++i) {
            for (u32 j = 0; j < i; ++j) {
                for (const auto& a : backing[group[i]]) {
                    for (const auto& b : backing[group[j]]) {
                        const s64 a_begin = *origins[group[i]] + a.offset;
                        const s64 b_begin = *origins[group[j]] + b.offset;
                        const s64 overlap = std::max(a_begin, b_begin);
                        if (overlap < std::min(a_begin + s64(a.size), b_begin + s64(b.size)) &&
                            a.address + u64(overlap - a_begin) !=
                                b.address + u64(overlap - b_begin))
                            throw std::runtime_error(
                                "D3D12 physical aliases require nonlinear buffer addressing");
                    }
                }
            }
        }
        auto& buffer = GetBuffer(ranges[root].address, bytes, false, true);
        for (const auto index : group) {
            const auto& range = ranges[index];
            const u64 offset = *origins[index] - begin;
            buffer.sources.push_back({range.address, offset, range.size});
            views[index] = {&buffer, offset, range.size, range.address};
        }
    }
    return views;
}
ResourceCache::Buffer& ResourceCache::GetBuffer(VAddr address, u64 size, bool physical,
                                                bool gathered) {
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
        if (!gathered && buffer->sources.empty() && buffer->physical == physical &&
            buffer->batch == batch && address < buffer->address + buffer->size &&
            buffer->address < address + size)
            throw std::runtime_error("Overlapping D3D12 buffer views require a shared allocation");
    EnsureSpace(0, true);
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
    const auto allocation = context->Device()->GetResourceAllocationInfo(0, 1, &description);
    if (!allocation.SizeInBytes || allocation.SizeInBytes == UINT64_MAX)
        throw std::runtime_error("Unsupported D3D12 buffer allocation");
    EnsureSpace(allocation.SizeInBytes);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> native;
    winrt::check_hresult(context->Device()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&native)));
    result->native = std::move(native);
    result->allocation_bytes =
        context->Device()->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
    auto& output = *result;
    buffers.emplace(key, std::move(result));
    native_bytes += output.allocation_bytes;
    return output;
}
void ResourceCache::EnsureSpace(u64 required, bool inserting) {
    const auto exceeds_budget = [&] {
        const auto stats = allocator.Stats();
        return stats.live_bytes > stats.budget_bytes ||
               native_bytes > stats.budget_bytes - stats.live_bytes ||
               required > stats.budget_bytes - stats.live_bytes - native_bytes;
    };
    while ((inserting && buffers.size() >= 128) || exceeds_budget()) {
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
}
void ResourceCache::EnsureStaging(Buffer& buffer, bool download) {
    auto& resource = download ? buffer.readback : buffer.upload;
    if (resource.Get())
        return;
    const u64 size = (buffer.size + 3) & ~u64{3};
    EnsureSpace((size + 65535) & ~u64{65535});
    winrt::check_hresult(allocator.CreateBuffer(
        size, download ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_UPLOAD,
        download ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ, resource));
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
    else if (!buffer.sources.empty())
        for (const auto& source : UniqueRanges(buffer.sources))
            read(source.address, std::span{data}.subspan(source.offset, source.size));
    else if (buffer.physical)
        std::memcpy(data.data(), reinterpret_cast<const void*>(buffer.address), data.size());
    else
        read(buffer.address, data);
    if (data != buffer.snapshot || Changed(buffer)) {
        EnsureStaging(buffer, false);
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
        EnsureStaging(*buffer, true);
        Barrier(*buffer, commands, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->CopyBufferRegion(buffer->readback.Get(), 0, buffer->native.Get(), 0,
                                   buffer->size);
    }
    for (auto& [key, buffer] : buffers)
        Barrier(*buffer, commands, D3D12_RESOURCE_STATE_COMMON);
}
void ResourceCache::CommitDownloads(bool mapping_stable) {
    for (auto& [key, buffer] : buffers) {
        if (!buffer->written)
            continue;
        if (!mapping_stable && Changed(*buffer, true))
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
            for (const auto& target : UniqueRanges(buffer->targets))
                write(target.address,
                      {buffer->snapshot.data() + target.offset, static_cast<size_t>(target.size)});
        buffer->written = false;
    }
}
bool ResourceCache::Changed(const Buffer& buffer, bool mappings_only) {
    if (buffer.host || buffer.physical)
        return false;
    std::scoped_lock lock{invalidation_mutex};
    if (buffer.epoch < (mappings_only ? mapping_floor : invalidation_floor))
        return true;
    for (const auto& item : invalidations) {
        if (item.epoch <= buffer.epoch || (mappings_only && !item.mapping))
            continue;
        if (buffer.sources.empty()) {
            if (item.address < buffer.address + buffer.size &&
                buffer.address < item.address + item.size)
                return true;
        } else {
            for (const auto& source : buffer.sources)
                if (item.address < source.address + source.size &&
                    source.address < item.address + item.size)
                    return true;
        }
    }
    return false;
}
bool ResourceCache::InvalidateMemory(VAddr address, u64 size, bool) {
    return InvalidateRange(address, size, false);
}
bool ResourceCache::InvalidateRange(VAddr address, u64 size, bool mapping) {
    if (!size || size > std::numeric_limits<VAddr>::max() - address)
        return false;
    std::scoped_lock lock{invalidation_mutex};
    invalidations.push_back({address, size, ++epoch, mapping});
    if (invalidations.size() > 4096) {
        invalidation_floor = invalidations[2047].epoch;
        for (u32 i = 0; i < 2048; ++i)
            if (invalidations[i].mapping)
                mapping_floor = invalidations[i].epoch;
        invalidations.erase(invalidations.begin(), invalidations.begin() + 2048);
    }
    return true;
}
void ResourceCache::MapMemory(VAddr address, u64 size) { InvalidateRange(address, size, true); }
void ResourceCache::RegisterMemory(VAddr address, u64 size) {
    InvalidateRange(address, size, true);
}
void ResourceCache::UnmapMemory(VAddr address, u64 size) { InvalidateRange(address, size, true); }
} // namespace D3D12
