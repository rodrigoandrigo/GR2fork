// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>

#include "common/alignment.h"
#ifdef SHADPS4_UWP
#include "core/address_space.h"
#include "video_core/renderer_vulkan/vk_resource_memory.h"
#endif
#include "common/assert.h"
#include "common/types.h"
#include "core/devtools/widget/gpu_memory.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_memory_recovery.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"

namespace Vulkan {

using VideoCore::MemoryType;

namespace {

constexpr u64 RING_IDLE_FRAMES = 300;
constexpr u64 LARGE_IDLE_FRAMES = 120;
constexpr u64 LARGE_MIN_GRANULARITY = 64_KB;
constexpr u64 BLOCK_SIZE = 16_MB;

u64 RoundAllocationSize(u64 size) {
    const u64 step = std::max(std::bit_floor(size) / 4, LARGE_MIN_GRANULARITY);
    return Common::AlignUp(size, step);
}

} // Anonymous namespace

StagingBufferPool::StagingBufferPool(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {}

StagingBufferPool::~StagingBufferPool() = default;

StagingBufferRef StagingBufferPool::Request(u64 size, MemoryType type, u64 alignment, bool deferred,
                                            bool unsynchronized) {
    AllocationRecoveryScope::StagingScope recovery_scope;
    Ring& ring = rings[u32(type)];
    if (deferred || size > BLOCK_SIZE) {
        return RequestLarge(size, type, deferred, unsynchronized);
    }
    return RequestFromRing(ring, size, alignment, type, unsynchronized);
}

StagingBufferRef StagingBufferPool::RequestFromRing(Ring& ring, u64 size, u64 alignment,
                                                    MemoryType type, bool unsynchronized) {
    Block* best{};
    u64 best_offset{};
    for (size_t i = 0; i < ring.blocks.size(); ++i) {
        const size_t index = (ring.current + i) % ring.blocks.size();
        Block* block = &ring.blocks[index];
        if (const auto offset = block->buffer->Reserve(size, alignment, false)) {
            ring.current = index;
            best = block;
            best_offset = *offset;
            break;
        }
    }

    if (!best) {
        scheduler.DrainPendingOperations();
#ifdef SHADPS4_UWP
        trim_pressure = Core::AddressSpace::MemoryHeadroom() < 640_MB;
#endif
    }
    if (!best && trim_pressure && !unsynchronized) {
        Block* reusable{};
        for (Block& block : ring.blocks) {
            const u64 tick = block.buffer->LastTick();
            if (tick < scheduler.CurrentTick() &&
                (!reusable || tick < reusable->buffer->LastTick()))
                reusable = &block;
        }
        if (reusable) {
            scheduler.Wait(reusable->buffer->LastTick());
            scheduler.DrainPendingOperations();
            if (const auto offset = reusable->buffer->Reserve(size, alignment, false)) {
                best = reusable;
                best_offset = *offset;
            } else {
                LOG_WARNING(Render_Vulkan,
                            "[StagingPressure] ring_reserve_failed_after_wait size={} alignment={} "
                            "type={} candidate_tick={} current_tick={} capacity={}",
                            size, alignment, u32(type), reusable->buffer->LastTick(),
                            scheduler.CurrentTick(), reusable->buffer->SizeBytes());
            }
        }
    }

    if (!best) {
        if (!ring.blocks.empty() && unsynchronized) {
            best = &ring.blocks[(ring.current + 1) % ring.blocks.size()];
            best_offset = 0;
        } else {
            if (trim_pressure)
                ReleaseCompleted(false);
            DiagnoseExpansion("ring", size, type, false, unsynchronized);
            best = &ring.blocks.emplace_back(Block{
                .buffer = std::make_unique<VideoCore::StreamBuffer>(instance, scheduler, type,
                                                                    BLOCK_SIZE),
                .last_used_frame = frame,
            });
            ring.current = ring.blocks.size() - 1;
#ifdef SHADPS4_UWP
            ResourceMemory::Name(reinterpret_cast<uintptr_t>(best->buffer->buffer.allocation), "StagingPool:Ring");
#endif
            best_offset = *best->buffer->Reserve(size, alignment, false);
        }
    }

    auto& buffer = *best->buffer;
    buffer.MarkUsed(scheduler.CurrentTick());
    best->last_used_frame = frame;

    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = best_offset,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data() + best_offset;
    }
    return ref;
}

StagingBufferRef StagingBufferPool::RequestLarge(u64 size, MemoryType type, bool deferred,
                                                 bool unsynchronized) {
    auto& cache = large_caches[u32(type)];
    const u64 max_size = std::max(size + size / 4, RoundAllocationSize(size));

    LargeBuffer* best{};
    for (LargeBuffer& entry : cache) {
        const u64 entry_size = entry.buffer->SizeBytes();
        if (entry.held || entry_size < size || (!trim_pressure && entry_size > max_size) ||
            (!scheduler.IsFree(std::max(entry.tick, entry.buffer->LastUseTick())) &&
             !unsynchronized)) {
            continue;
        }
        if (!best || entry_size < best->buffer->SizeBytes()) {
            best = &entry;
        }
    }

    if (!best) {
        scheduler.DrainPendingOperations();
#ifdef SHADPS4_UWP
        trim_pressure = Core::AddressSpace::MemoryHeadroom() < 640_MB;
#endif
    }
    if (!best && trim_pressure && !unsynchronized) {
        for (LargeBuffer& entry : cache) {
            const u64 tick = std::max(entry.tick, entry.buffer->LastUseTick());
            if (!entry.held && entry.buffer->SizeBytes() >= size &&
                tick < scheduler.CurrentTick() &&
                (!best || entry.buffer->SizeBytes() < best->buffer->SizeBytes()))
                best = &entry;
        }
        if (best) {
            scheduler.Wait(std::max(best->tick, best->buffer->LastUseTick()));
            scheduler.DrainPendingOperations();
        }
    }

    if (!best) {
        if (trim_pressure)
            ReleaseCompleted(false);
        DiagnoseExpansion("dedicated", size, type, deferred, unsynchronized);
        best = &cache.emplace_back(LargeBuffer{
            .buffer = std::make_unique<VideoCore::Buffer>(instance, 0, RoundAllocationSize(size),
                                                          type, "StagingBufferPool:Dedicated"),
            .tick = 0,
            .last_used_frame = frame,
            .held = false,
        });
    }

    best->tick = scheduler.CurrentTick();
    best->last_used_frame = frame;
    best->held = deferred;

    VideoCore::Buffer& buffer = *best->buffer;
    buffer.MarkUsed(scheduler.CurrentTick());
    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = 0,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data();
    }
    return ref;
}

void StagingBufferPool::DiagnoseExpansion(const char* path, u64 size, MemoryType type,
                                         bool deferred, bool unsynchronized) {
#ifdef SHADPS4_UWP
    const u64 headroom = Core::AddressSpace::MemoryHeadroom();
    if (headroom >= 640_MB)
        return;
    const u64 current = scheduler.CurrentTick();
    LOG_WARNING(Render_Vulkan,
                "[StagingPressure] expansion path={} requested_bytes={} allocation_bytes={} "
                "type={} deferred={} unsynchronized={} headroom_bytes={} current_tick={} "
                "known_gpu={} frame={} pressure={}",
                path, size, std::string_view(path) == "ring" ? BLOCK_SIZE : RoundAllocationSize(size),
                u32(type), deferred, unsynchronized, headroom, current,
                scheduler.GetWorkSemaphore()->KnownGpuTick(), frame, trim_pressure);
    for (size_t t = 0; t < NUM_TYPES; ++t) {
        u64 ring_bytes{}, current_bytes{}, pending_bytes{}, completed_bytes{}, held_bytes{};
        u64 large_bytes{}, too_small{}, candidates{};
        size_t rows{};
        auto describe = [&](const VideoCore::Buffer& buffer, u64 tick, bool held,
                            const char* kind) {
            const u64 bytes = buffer.SizeBytes();
            const bool complete = scheduler.IsFree(tick);
            if (held)
                held_bytes += bytes;
            else if (tick >= current)
                current_bytes += bytes;
            else if (!complete)
                pending_bytes += bytes;
            else
                completed_bytes += bytes;
            if (bytes < size)
                ++too_small;
            else if (!held && tick < current)
                ++candidates;
            if (rows++ < 12)
                LOG_WARNING(Render_Vulkan,
                            "[StagingPressure] entry type={} kind={} bytes={} tick={} held={} "
                            "gpu_done={} current_batch={}",
                            t, kind, bytes, tick, held, complete, tick >= current);
        };
        for (const Block& block : rings[t].blocks) {
            ring_bytes += block.buffer->SizeBytes();
            describe(*block.buffer, block.buffer->LastTick(), false, "ring");
        }
        for (const LargeBuffer& entry : large_caches[t]) {
            large_bytes += entry.buffer->SizeBytes();
            describe(*entry.buffer, std::max(entry.tick, entry.buffer->LastUseTick()), entry.held,
                     "dedicated");
        }
        LOG_WARNING(Render_Vulkan,
                    "[StagingPressure] totals type={} ring_count={} ring_bytes={} "
                    "dedicated_count={} dedicated_bytes={} held_bytes={} current_batch_bytes={} "
                    "pending_previous_bytes={} completed_bytes={} too_small_count={} "
                    "previous_candidates={} entries_not_shown={}",
                    t, rings[t].blocks.size(), ring_bytes, large_caches[t].size(), large_bytes,
                    held_bytes, current_bytes, pending_bytes, completed_bytes, too_small, candidates,
                    rows > 12 ? rows - 12 : 0);
    }
    ResourceMemory::Report("staging_expansion", false);
    Common::Log::Flush();
#endif
}

void StagingBufferPool::FreeDeferred(const StagingBufferRef& ref) {
    auto& cache = large_caches[u32(ref.type)];
    const auto it = std::ranges::find_if(
        cache, [&](const LargeBuffer& entry) { return entry.buffer.get() == ref.buffer; });
    ASSERT_MSG(it != cache.end() && it->held, "FreeDeferred on an allocation that is not held");
    it->held = false;
    it->tick = scheduler.CurrentTick();
    it->buffer->MarkUsed(it->tick);
    it->last_used_frame = frame;
}

void StagingBufferPool::TickFrame() {
    ++frame;
#ifdef SHADPS4_UWP
    const auto epoch = Core::AddressSpace::MemoryPressureEpoch();
    trim_pressure = epoch != pressure_epoch || Core::AddressSpace::MemoryHeadroom() < 400_MB;
    pressure_epoch = epoch;
#endif
    for (Ring& ring : rings) {
        TrimRing(ring);
    }
    for (auto& cache : large_caches) {
        TrimLarge(cache);
    }
    PublishStats();
}

u64 StagingBufferPool::ReleaseCompleted(bool aggressive) {
    u64 released{};
    for (auto& ring : rings) {
        const auto newest = std::ranges::max_element(ring.blocks, {}, &Block::last_used_frame);
        const auto* retained = newest == ring.blocks.end() ? nullptr : newest->buffer.get();
        for (size_t i = ring.blocks.size(); i-- > 0;) {
            const auto& buffer = *ring.blocks[i].buffer;
            if ((!aggressive && &buffer == retained) ||
                buffer.LastTick() >= scheduler.CurrentTick() ||
                !scheduler.IsFree(buffer.LastTick()))
                continue;
            released += buffer.SizeBytes();
            ring.blocks.erase(ring.blocks.begin() + i);
        }
        ring.current = 0;
    }
    for (auto& cache : large_caches) {
        for (size_t i = cache.size(); i-- > 0;) {
            const auto& entry = cache[i];
            const u64 tick = std::max(entry.tick, entry.buffer->LastUseTick());
            if (entry.held || tick >= scheduler.CurrentTick() || !scheduler.IsFree(tick) ||
                (!aggressive && frame - entry.last_used_frame < 2))
                continue;
            released += entry.buffer->SizeBytes();
            cache.erase(cache.begin() + i);
        }
    }
    return released;
}

void StagingBufferPool::TrimRing(Ring& ring) {
    auto& blocks = ring.blocks;
    const auto newest = std::ranges::max_element(blocks, {}, &Block::last_used_frame);
    const auto* retained = newest == blocks.end() ? nullptr : newest->buffer.get();
    for (size_t i = blocks.size(); i-- > 0;) {
        Block& block = blocks[i];
        const u64 age = frame - block.last_used_frame;
        if ((block.buffer.get() == retained && age < RING_IDLE_FRAMES) ||
            age < (trim_pressure ? 2 : RING_IDLE_FRAMES) ||
            !scheduler.IsFree(block.buffer->LastTick())) {
            continue;
        }
#ifdef SHADPS4_UWP
        ResourceMemory::Name(reinterpret_cast<uintptr_t>(block.buffer->buffer.allocation), "StagingPool:RetiredRing");
#endif
        scheduler.DeferOperation([retired = std::move(block.buffer)] {});
        blocks.erase(blocks.begin() + i);
        if (i < ring.current) {
            --ring.current;
        } else if (i == ring.current) {
            ring.current = 0;
        }
    }
    if (ring.current >= blocks.size()) {
        ring.current = 0;
    }
}

void StagingBufferPool::TrimLarge(std::vector<LargeBuffer>& cache) {
    u64 retained_bytes{};
    for (size_t i = cache.size(); i-- > 0;) {
        auto& entry = cache[i];
        const u64 age = frame - entry.last_used_frame;
        if (!entry.held && age < LARGE_IDLE_FRAMES &&
            entry.buffer->SizeBytes() <= BLOCK_SIZE - retained_bytes) {
            retained_bytes += entry.buffer->SizeBytes();
            continue;
        }
        if (entry.held || age < (trim_pressure ? 2 : LARGE_IDLE_FRAMES) ||
            !scheduler.IsFree(std::max(entry.tick, entry.buffer->LastUseTick()))) {
            continue;
        }
#ifdef SHADPS4_UWP
        ResourceMemory::Name(reinterpret_cast<uintptr_t>(entry.buffer->buffer.allocation), "StagingPool:RetiredLarge");
#endif
        scheduler.DeferOperation([retired = std::move(entry.buffer)] {});
        cache.erase(cache.begin() + i);
    }
}

void StagingBufferPool::PublishStats() const {
    using namespace Core::Devtools::Widget;
    if (!GpuMemoryViewer::IsEnabled()) {
        return;
    }
    static constexpr std::array<const char*, NUM_TYPES> names = {"DeviceLocal", "HostUncached",
                                                                 "HostCached"};
    GpuMemoryGroup group{.name = "Staging"};
    for (size_t i = 0; i < NUM_TYPES; ++i) {
        u64 bytes = 0;
        for (const Block& block : rings[i].blocks) {
            bytes += block.buffer->SizeBytes();
        }
        for (const LargeBuffer& entry : large_caches[i]) {
            bytes += entry.buffer->SizeBytes();
        }
        if (bytes != 0) {
            group.rows.push_back({.name = names[i], .bytes = bytes});
        }
    }
    GpuMemoryViewer::Publish(std::move(group));
}

} // namespace Vulkan
