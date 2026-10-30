#include "video_core/renderer_d3d12/resource_cache.h"

#include <algorithm>
#ifdef SHADPS4_UWP
#include "core/address_space.h"
#endif
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <winrt/base.h>

namespace D3D12 {
namespace {
std::vector<ResourceCache::Buffer::WriteTarget> UniqueRanges(
    std::vector<ResourceCache::Buffer::WriteTarget> ranges) {
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
bool ResourceCache::BufferOverlaps(const Buffer& buffer, VAddr address, u64 size) const {
    if (buffer.host || !address || !size)
        return false;
    if (!buffer.sources.empty())
        return std::ranges::any_of(buffer.sources, [&](const auto& source) {
            return Aliases(address, size, source.address, source.size);
        });
    return !buffer.physical && Aliases(address, size, buffer.address, buffer.size);
}
void ResourceCache::ConfigureResidency(bool enabled, std::function<void(VAddr, u64)> callback) {
    persistent = enabled;
    synchronize = std::move(callback);
}
std::vector<ResourceCache::Watch> ResourceCache::Watches() const {
    std::vector<Watch> result;
    if (persistent)
        for (const auto& [key, buffer] : buffers)
            if (!buffer->host && buffer->resident)
                for (const auto& source : UniqueRanges(buffer->sources)) {
                    u64 cursor = source.offset;
                    const auto end = source.offset + source.size;
                    for (const auto& [offset, size] : buffer->dirty) {
                        if (offset >= end)
                            break;
                        if (cursor < offset)
                            result.push_back({source.address + cursor - source.offset,
                                              std::min(offset, end) - cursor, buffer->written});
                        cursor = std::min(end, std::max(cursor, offset + size));
                    }
                    if (cursor < end)
                        result.push_back({source.address + cursor - source.offset, end - cursor,
                                          buffer->written});
                }
    return result;
}
bool ResourceCache::RequestDownloads(std::span<const Range> ranges, bool all) {
    bool requested{};
    for (auto& [key, buffer] : buffers)
        if (!buffer->host && buffer->written &&
            (all || std::ranges::any_of(ranges, [&](const auto& range) {
                 return BufferOverlaps(*buffer, range.address, range.size);
             }))) {
            buffer->download_requested = true;
            requested = true;
        }
    return requested;
}
void ResourceCache::InvalidateCpu(std::span<const Range> ranges, bool all) {
    if (!persistent)
        return;
    for (auto& [key, buffer] : buffers) {
        if (buffer->host || !buffer->resident)
            continue;
        for (const auto& source : buffer->sources) {
            if (all) {
                buffer->dirty.emplace_back(source.offset, source.size);
                continue;
            }
            for (const auto& requested : ranges) {
                if (!requested.address || !requested.size)
                    continue;
                const u64 page_begin = requested.address & ~u64{4095};
                const u64 maximum = std::numeric_limits<u64>::max();
                if (requested.address > maximum - requested.size) {
                    buffer->dirty.emplace_back(source.offset, source.size);
                    continue;
                }
                const u64 end = requested.address + requested.size;
                const u64 page_end = end > maximum - 4095 ? maximum : (end + 4095) & ~u64{4095};
                const Range range{page_begin, page_end - page_begin};
                if (!Aliases(range.address, range.size, source.address, source.size))
                    continue;
                const auto requested_identity = Canonicalize(range.address, range.size);
                const auto source_identity = Canonicalize(source.address, source.size);
                const bool physical = requested_identity.physical && source_identity.physical;
                const auto requested_start = physical ? requested_identity.address : range.address;
                const auto source_start = physical ? source_identity.address : source.address;
                if (requested_start < source_start + source.size &&
                    source_start < requested_start + range.size) {
                    const auto first = std::max(source_start, requested_start);
                    const auto last =
                        std::min(source_start + source.size, requested_start + range.size);
                    buffer->dirty.emplace_back(source.offset + first - source_start, last - first);
                } else {
                    buffer->dirty.emplace_back(source.offset, source.size);
                }
            }
        }
        std::ranges::sort(buffer->dirty);
        std::vector<std::pair<u64, u64>> merged;
        for (const auto& [offset, size] : buffer->dirty) {
            if (!merged.empty() && offset <= merged.back().first + merged.back().second)
                merged.back().second =
                    std::max(merged.back().first + merged.back().second, offset + size) -
                    merged.back().first;
            else
                merged.emplace_back(offset, size);
        }
        buffer->dirty = std::move(merged);
    }
}
bool ResourceCache::CopyResident(VAddr address, u64 size, Buffer& destination, u64 offset,
                                 ID3D12GraphicsCommandList* commands) {
    if (!persistent)
        return false;
    const auto identity = Canonicalize(address, size);
    if (!identity.physical)
        return false;
    for (auto& [key, buffer] : buffers) {
        if (!buffer->resident || buffer->host || !buffer->dirty.empty())
            continue;
        for (const auto& source : buffer->sources) {
            const auto origin = Canonicalize(source.address, source.size);
            if (!origin.physical || identity.address < origin.address ||
                identity.address + size > origin.address + source.size)
                continue;
            if (!commands)
                return true;
            const auto old = buffer->state;
            Barrier(*buffer, commands, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = {destination.native.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                  destination.state, D3D12_RESOURCE_STATE_COPY_DEST};
            if (destination.state != D3D12_RESOURCE_STATE_COPY_DEST)
                commands->ResourceBarrier(1, &barrier);
            destination.state = D3D12_RESOURCE_STATE_COPY_DEST;
            commands->CopyBufferRegion(destination.native.Get(), offset, buffer->native.Get(),
                                       source.offset + identity.address - origin.address, size);
            Barrier(*buffer, commands, old);
            return true;
        }
    }
    return false;
}
void ResourceCache::InvalidateMappings() {
    for (auto& [key, buffer] : buffers) {
        if (buffer->host)
            continue;
        if (buffer->written)
            throw std::runtime_error("Mapping invalidation requires GPU buffer writeback first");
        buffer->resident = false;
        buffer->sources.clear();
        buffer->targets.clear();
        buffer->snapshot.clear();
        buffer->dirty.clear();
    }
}
void ResourceCache::PrepareViews(std::span<const Range> ranges) {
    if (!persistent)
        return;
    const auto views = GetViews(ranges);
    for (auto& [key, buffer] : buffers) {
        if (!buffer->resident || buffer->host || std::ranges::any_of(views, [&](const auto& view) {
                return view.buffer == buffer.get();
            }))
            continue;
        if (std::ranges::any_of(ranges, [&](const auto& range) {
                return BufferOverlaps(*buffer, range.address, range.size);
            })) {
            if (buffer->written && synchronize)
                for (const auto& source : buffer->sources)
                    synchronize(source.address, source.size);
            buffer->resident = false;
        }
    }
}
void ResourceCache::Initialize(D3D12DeviceContext& device, Read reader, Write writer,
                               Resolve resolver, Overlaps overlap,
                               std::shared_ptr<TransferPool> transfers) {
    context = &device;
    transfer_pool = std::move(transfers);
    if (!transfer_pool) {
        transfer_pool = std::make_shared<TransferPool>();
        transfer_pool->Initialize(device);
    }
    read = std::move(reader);
    write = std::move(writer);
    resolve = std::move(resolver);
    overlaps = std::move(overlap);
    allocator.Initialize(device.Device(), 256ULL * 1024 * 1024);
}
u64 ResourceCache::MemoryBytes() const {
    u64 bytes = native_bytes;
    for (const auto& [key, buffer] : buffers)
        bytes += buffer->snapshot.capacity() + buffer->host_data.capacity() +
                 buffer->dirty.capacity() * sizeof(buffer->dirty[0]) +
                 (buffer->sources.capacity() + buffer->targets.capacity()) *
                     sizeof(Buffer::WriteTarget) +
                 buffer->backing_layout.capacity() * sizeof(buffer->backing_layout[0]);
    return bytes;
}
void ResourceCache::TrimMemory(u64 target, bool writeback) {
    while (MemoryBytes() > target) {
        auto oldest = buffers.end();
        for (auto it = buffers.begin(); it != buffers.end(); ++it)
            if ((!it->second->written || (writeback && synchronize)) &&
                (oldest == buffers.end() || it->second->used < oldest->second->used))
                oldest = it;
        if (oldest == buffers.end())
            break;
        if (oldest->second->written) {
            const auto sources = oldest->second->sources;
            for (const auto& source : sources)
                synchronize(source.address, source.size);
            if (oldest->second->written)
                break;
        }
        native_bytes -= oldest->second->allocation_bytes;
        buffers.erase(oldest);
    }
}
void ResourceCache::BeginBatch() {
    if (!async_recording)
        context->WaitForGpu();
    ++batch;
    for (auto& [key, buffer] : buffers) {
        if (!buffer->written)
            buffer->targets.clear();
        if (!persistent)
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
bool ResourceCache::HasRepeatedBacking(VAddr address, u64 size) const {
    if (!resolve_backing || !address || !size)
        return false;
    const auto parts = resolve_backing(address, size);
    for (u32 i = 0; i < parts.size(); ++i)
        for (u32 j = 0; j < i; ++j)
            if (parts[i].address < parts[j].address + parts[j].size &&
                parts[j].address < parts[i].address + parts[i].size)
                return true;
    return false;
}
void ResourceCache::MarkWritten(const View& view) {
    if (!view.buffer)
        return;
    view.buffer->written = true;
    if (!view.buffer->host) {
        if (!view.segments.empty()) {
            view.buffer->targets.insert(view.buffer->targets.end(), view.segments.begin(),
                                        view.segments.end());
            return;
        }
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
    auto& buffer = GetBuffer(identity, data.size(), false, false, true);
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
                part.address > UINT64_MAX - part.size - 15)
                throw std::runtime_error("Invalid D3D12 buffer backing segment");
            backing[i].push_back({part.address, part.size, offset});
            offset += part.size;
        }
        if (offset != range.size)
            throw std::runtime_error("Incomplete D3D12 buffer backing");
    }
    std::vector<View> views(ranges.size());
    std::vector<bool> assigned(ranges.size());
    for (u32 root = 0; root < ranges.size(); ++root) {
        if (backing[root].empty() || assigned[root])
            continue;
        std::vector<u32> group{root};
        assigned[root] = true;
        for (u32 position = 0; position < group.size(); ++position) {
            for (u32 next = 0; next < ranges.size(); ++next) {
                if (assigned[next] || backing[next].empty())
                    continue;
                bool shared{};
                for (const auto& a : backing[group[position]])
                    for (const auto& b : backing[next])
                        shared |= a.address < b.address + b.size && b.address < a.address + a.size;
                if (shared) {
                    assigned[next] = true;
                    group.push_back(next);
                }
            }
        }
        std::vector<Range> physical;
        for (const u32 index : group)
            for (const auto& segment : backing[index])
                physical.push_back({segment.address & ~u64{15},
                                    ((segment.address + segment.size + 15) & ~u64{15}) -
                                        (segment.address & ~u64{15})});
        std::ranges::sort(physical, {}, &Range::address);
        std::vector<Segment> allocations;
        u64 bytes{};
        for (const auto& range : physical) {
            if (!allocations.empty() &&
                range.address <= allocations.back().address + allocations.back().size) {
                auto& last = allocations.back();
                const u64 end = std::max(last.address + last.size, range.address + range.size);
                bytes += end - last.address - last.size;
                last.size = end - last.address;
            } else {
                allocations.push_back({range.address, range.size, bytes});
                bytes += range.size;
            }
        }
        if (bytes > 16ULL * 1024 * 1024)
            throw std::runtime_error("Shared D3D12 buffer aliases exceed allocation budget");
        std::vector<std::pair<VAddr, u64>> layout;
        for (const auto& part : allocations)
            layout.emplace_back(part.address, part.size);
        const auto matches = [&](const auto& entry) {
            if (entry.second->backing_layout == layout)
                return true;
            return persistent && !entry.second->backing_layout.empty() &&
                   std::ranges::all_of(layout, [&](const auto& requested) {
                       return std::ranges::any_of(entry.second->backing_layout,
                                                  [&](const auto& cached) {
                                                      return requested.first >= cached.first &&
                                                             requested.first + requested.second <=
                                                                 cached.first + cached.second;
                                                  });
                   });
        };
        auto existing = std::ranges::find_if(
            buffers, [&](const auto& entry) { return entry.second->resident && matches(entry); });
        if (existing == buffers.end())
            existing = std::ranges::find_if(buffers, matches);
        if (persistent && existing != buffers.end()) {
            allocations.clear();
            bytes = 0;
            layout = existing->second->backing_layout;
            for (const auto& [address, size] : layout) {
                allocations.push_back({address, size, bytes});
                bytes += size;
            }
        }
        if (existing == buffers.end() && backing_identity == 0x0000ffffffffffffULL)
            throw std::runtime_error("D3D12 buffer backing identity exhausted");
        const auto identity = !persistent ? ranges[root].address
                              : existing == buffers.end()
                                  ? 0xfff7000000000000ULL + ++backing_identity
                                  : existing->second->address;
        auto& buffer = GetBuffer(identity, bytes, false, true);
        buffer.backing_layout = std::move(layout);
        for (const u32 index : group) {
            auto& view = views[index];
            view = {&buffer, 0, ranges[index].size, ranges[index].address};
            for (const auto& segment : backing[index]) {
                const auto allocation = std::ranges::find_if(allocations, [&](const auto& part) {
                    return segment.address >= part.address &&
                           segment.address + segment.size <= part.address + part.size;
                });
                const u64 offset = allocation->offset + segment.address - allocation->address;
                const Buffer::WriteTarget source{ranges[index].address + segment.offset, offset,
                                                 segment.size};
                if (buffer.resident) {
                    u64 cursor = source.offset;
                    const auto end = source.offset + source.size;
                    for (const auto& previous : UniqueRanges(buffer.sources)) {
                        if (previous.offset >= end)
                            break;
                        if (cursor < previous.offset)
                            buffer.dirty.emplace_back(cursor,
                                                      std::min(end, previous.offset) - cursor);
                        cursor = std::min(end, std::max(cursor, previous.offset + previous.size));
                    }
                    if (cursor < end)
                        buffer.dirty.emplace_back(cursor, end - cursor);
                    std::ranges::sort(buffer.dirty);
                }
                if (!std::ranges::any_of(buffer.sources, [&](const auto& previous) {
                        return previous.address == source.address &&
                               previous.offset == source.offset && previous.size == source.size;
                    }))
                    buffer.sources.push_back(source);
                view.segments.push_back(source);
            }
            const u64 first = view.segments.front().offset;
            if (std::ranges::all_of(view.segments, [&](const auto& segment) {
                    return segment.offset == first + segment.address - view.guest_address;
                }))
                view.offset = first;
        }
    }
    return views;
}
ResourceCache::Buffer& ResourceCache::GetBuffer(VAddr address, u64 size, bool physical,
                                                bool gathered, bool host) {
    if (!address || !size || size > 16ULL * 1024 * 1024 ||
        size > std::numeric_limits<VAddr>::max() - address)
        throw std::runtime_error("Invalid D3D12 guest buffer range");
    const auto key = std::tuple{host, physical, address, size};
    if (const auto found = buffers.find(key); found != buffers.end()) {
        found->second->used = ++age;
        found->second->batch = batch;
        return *found->second;
    }
    for (const auto& [existing, buffer] : buffers)
        if (!host && !buffer->host && !gathered && buffer->sources.empty() &&
            buffer->physical == physical && buffer->batch == batch &&
            address < buffer->address + buffer->size && buffer->address < address + size)
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
        if (oldest == buffers.end() && synchronize) {
            for (auto it = buffers.begin(); it != buffers.end(); ++it)
                if (it->second->batch != batch &&
                    (oldest == buffers.end() || it->second->used < oldest->second->used))
                    oldest = it;
            if (oldest != buffers.end()) {
                const auto sources = oldest->second->sources;
                for (const auto& source : sources)
                    synchronize(source.address, source.size);
            }
        }
        if (oldest == buffers.end() || oldest->second->written)
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
    resource =
        transfer_pool->Acquire(size, download ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_UPLOAD);
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
    if (persistent && !buffer.host && buffer.resident && !buffer.sources.empty() &&
        buffer.dirty.empty()) {
        ++upload_statistics.hits;
        Barrier(buffer, commands, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        return;
    }
    const bool incremental = persistent && !buffer.host && buffer.resident &&
                             buffer.snapshot.size() == buffer.size && !buffer.dirty.empty();
    std::vector<u8> data = incremental ? buffer.snapshot : std::vector<u8>(buffer.size);
    if (buffer.host)
        data = buffer.host_data;
    else if (!buffer.sources.empty()) {
        for (const auto& source : UniqueRanges(buffer.sources)) {
            if (!incremental) {
                read(source.address, std::span{data}.subspan(source.offset, source.size));
                upload_statistics.cpu_bytes += source.size;
                continue;
            }
            for (const auto& [offset, size] : buffer.dirty) {
                const auto first = std::max(offset, source.offset);
                const auto last = std::min(offset + size, source.offset + source.size);
                if (first < last) {
                    read(source.address + first - source.offset,
                         std::span{data}.subspan(first, last - first));
                    upload_statistics.cpu_bytes += last - first;
                }
            }
        }
    } else if (buffer.physical) {
#ifdef SHADPS4_UWP
        if (!Core::AddressSpace::CopyBacking(
                data.data(), reinterpret_cast<const void*>(buffer.address), data.size()))
#endif
            std::memcpy(data.data(), reinterpret_cast<const void*>(buffer.address), data.size());
        upload_statistics.cpu_bytes += data.size();
    } else {
        read(buffer.address, data);
        upload_statistics.cpu_bytes += data.size();
    }
    std::vector<std::pair<size_t, size_t>> changes;
    const auto add_change = [&](size_t first, size_t last) {
        if (data.size() == buffer.snapshot.size() && !Changed(buffer, true)) {
            while (first < last && data[first] == buffer.snapshot[first])
                ++first;
            while (last > first && data[last - 1] == buffer.snapshot[last - 1])
                --last;
        }
        if (first < last)
            changes.emplace_back(first, last);
    };
    if (incremental) {
        for (const auto& [offset, size] : buffer.dirty)
            add_change(offset, offset + size);
    } else if (data != buffer.snapshot || Changed(buffer)) {
        if (Changed(buffer))
            changes.emplace_back(0, data.size());
        else
            add_change(0, data.size());
    }
    if (!changes.empty()) {
        EnsureStaging(buffer, false);
        void* mapped;
        const D3D12_RANGE empty{0, 0};
        winrt::check_hresult(buffer.upload->Map(0, &empty, &mapped));
        for (const auto& [begin, end] : changes)
            std::memcpy(static_cast<u8*>(mapped) + begin, data.data() + begin, end - begin);
        const D3D12_RANGE contents{0, data.size()};
        buffer.upload->Unmap(0, &contents);
        Barrier(buffer, commands, D3D12_RESOURCE_STATE_COPY_DEST);
        for (const auto& [begin, end] : changes) {
            commands->CopyBufferRegion(buffer.native.Get(), begin, buffer.upload.Get(), begin,
                                       end - begin);
            upload_statistics.bytes += end - begin;
            if (begin == 0 && end == data.size())
                ++upload_statistics.full;
            else
                ++upload_statistics.partial;
        }
        buffer.snapshot = std::move(data);
    }
    {
        std::scoped_lock lock{invalidation_mutex};
        buffer.epoch = epoch;
    }
    buffer.resident = true;
    buffer.dirty.clear();
    Barrier(buffer, commands, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
void ResourceCache::PrepareDownload(ID3D12GraphicsCommandList* commands) {
    for (auto& [key, buffer] : buffers) {
        if (!buffer->written || (persistent && !buffer->host && !buffer->download_requested))
            continue;
        EnsureStaging(*buffer, true);
        Barrier(*buffer, commands, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->CopyBufferRegion(buffer->readback.Get(), 0, buffer->native.Get(), 0,
                                   buffer->size);
    }
    for (auto& [key, buffer] : buffers)
        Barrier(*buffer, commands, D3D12_RESOURCE_STATE_COMMON);
}
void ResourceCache::RetireUploads(u64 fence) {
    transfer_fence = fence;
    for (auto& [key, buffer] : buffers)
        transfer_pool->Retire(std::move(buffer->upload), fence);
}
void ResourceCache::CommitDownloads(bool mapping_stable) {
    const u64 fence = transfer_fence ? transfer_fence : context->LastSignaledValue();
    if (context->CompletedValue() < fence)
        throw std::runtime_error("D3D12 buffer readback requested before transfer fence");
    for (auto& [key, buffer] : buffers) {
        if (!buffer->written || !buffer->readback.Get())
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
        buffer->download_requested = false;
        ++upload_statistics.downloads;
        upload_statistics.download_bytes += buffer->size;
    }
    RetireUploads(fence);
    for (auto& [key, buffer] : buffers)
        transfer_pool->Retire(std::move(buffer->readback), fence);
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
    if (mapping)
        ++mapping_generation;
    if (invalidations.size() > 4096) {
        invalidation_floor = invalidations[2047].epoch;
        for (u32 i = 0; i < 2048; ++i)
            if (invalidations[i].mapping)
                mapping_floor = invalidations[i].epoch;
        invalidations.erase(invalidations.begin(), invalidations.begin() + 2048);
    }
    return true;
}
void ResourceCache::MapMemory(VAddr address, u64 size) {
    InvalidateRange(address, size, true);
}
void ResourceCache::RegisterMemory(VAddr address, u64 size) {
    InvalidateRange(address, size, true);
}
void ResourceCache::UnmapMemory(VAddr address, u64 size) {
    InvalidateRange(address, size, true);
}
} // namespace D3D12
