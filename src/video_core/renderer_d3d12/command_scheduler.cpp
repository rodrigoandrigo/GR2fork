#include "video_core/renderer_d3d12/command_runtime.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <winrt/base.h>
#include "common/logging/log.h"
#include "video_core/renderer_d3d12/profile.h"

namespace D3D12 {
namespace {
Profile runtime_profile;
}
u64 CommandRuntime::LocalMemoryBytes() const {
    u64 bytes = resources.MemoryBytes() + inputs.MemoryBytes() + images.MemoryBytes() +
                shaders.MemoryBytes() + gpu_commands.MemoryBytes() + pipeline_bytes +
                pipelines.size() * 65536;
    if (transfer_pool)
        bytes += transfer_pool->ResourceStatistics().live_bytes;
    return bytes;
}
u64 CommandRuntime::MemoryBytes() const {
    u64 bytes = LocalMemoryBytes() + (external_memory ? external_memory() : 0);
    for (const auto& context : contexts)
        bytes += context->LocalMemoryBytes();
    return bytes;
}
void CommandRuntime::TrimLocalMemory(u64 target, bool writeback) {
    transfer_pool->TrimMemory(0);
    if (LocalMemoryBytes() <= target)
        return;
    shaders.TrimMemory(std::min(shaders.MemoryBytes(), target / 8));
    if (LocalMemoryBytes() > target) {
        pipelines.clear();
        pipeline_bytes = 0;
        gpu_commands.TrimMemory();
    }
    const u64 cache_bytes = resources.MemoryBytes() + inputs.MemoryBytes() + images.MemoryBytes();
    const u64 other = LocalMemoryBytes() - cache_bytes;
    const double ratio =
        cache_bytes ? double(target > other ? target - other : 0) / cache_bytes : 0;
    inputs.TrimMemory(u64(inputs.MemoryBytes() * std::min(1.0, ratio)), writeback);
    resources.TrimMemory(u64(resources.MemoryBytes() * std::min(1.0, ratio)), writeback);
    images.TrimMemory(u64(images.MemoryBytes() * std::min(1.0, ratio)), writeback);
    transfer_pool->TrimMemory(0);
}
void CommandRuntime::MaintainMemory(bool force) {
    auto lock = ProfileSample(runtime_profile, "scheduler_lock", 512,
                              [&] { return std::unique_lock{mutex}; });
    if (coordinator || failed || trimming_memory || !device.Device())
        return;
    const u64 before = MemoryBytes();
    if (!memory_budget.Poll(before, force))
        return;
    auto& statistics = memory_budget.Stats();
    if (before <= statistics.target) {
        statistics.pinned = 0;
        return;
    }
    struct Scope {
        bool& active;
        ~Scope() {
            active = false;
        }
    } scope{trimming_memory};
    trimming_memory = true;
    DrainContexts();
    device.WaitForGpu();
    if (trim_external_memory)
        trim_external_memory();
    for (u32 pass = 0; pass < 3; ++pass) {
        const u64 current = MemoryBytes();
        if (current <= statistics.target)
            break;
        const double ratio = double(statistics.target) / current;
        TrimLocalMemory(pass == 1 ? 0 : u64(LocalMemoryBytes() * ratio), pass == 2);
        for (auto& context : contexts)
            if (MemoryBytes() > statistics.target)
                context->TrimLocalMemory(pass == 1 ? 0 : u64(context->LocalMemoryBytes() * ratio),
                                         pass == 2);
    }
    UpdateImageWatches();
    const u64 after = MemoryBytes();
    statistics.retained = after;
    statistics.reclaimed += before > after ? before - after : 0;
    statistics.pinned = after > statistics.target ? after - statistics.target : 0;
    ++statistics.trims;
    if (memory_budget.Background() || statistics.sample.pressure >= 2)
        static_cast<void>(device.TryTrim());
    LOG_INFO(Render,
             "D3D12 memory: retained={} target={} reclaimed={} pinned={} "
             "process={}/{} local={}/{} nonlocal={}/{} pressure={} background={}",
             after, statistics.target, before > after ? before - after : 0, statistics.pinned,
             statistics.sample.process_usage, statistics.sample.process_limit,
             statistics.sample.local_usage, statistics.sample.local_budget,
             statistics.sample.nonlocal_usage, statistics.sample.nonlocal_budget,
             statistics.sample.pressure, statistics.sample.background);
}
void CommandRuntime::SetBackground(bool background) {
    auto lock = ProfileSample(runtime_profile, "scheduler_lock", 512,
                              [&] { return std::unique_lock{mutex}; });
    const bool changed = memory_budget.Background() != background;
    memory_budget.SetBackground(background);
    MaintainMemory(changed);
}
ResourceCache::UploadStatistics CommandRuntime::BufferStatistics(bool input) const {
    auto total = (input ? inputs : resources).GetUploadStatistics();
    for (const auto& context : contexts) {
        const auto next = context->BufferStatistics(input);
        total.bytes += next.bytes;
        total.full += next.full;
        total.partial += next.partial;
        total.hits += next.hits;
        total.cpu_bytes += next.cpu_bytes;
        total.downloads += next.downloads;
        total.download_bytes += next.download_bytes;
    }
    return total;
}
void CommandRuntime::EnableAsync(ResourceCache::ResolveBacking backing, AcquireMemory acquire) {
    auto lock = ProfileSample(runtime_profile, "scheduler_lock", 512,
                              [&] { return std::unique_lock{mutex}; });
    if (!contexts.empty() || coordinator || !backing || !acquire)
        throw std::runtime_error("Invalid native command scheduler configuration");
    Flush();
    for (u32 index = 0; index < 4; ++index) {
        auto context = std::unique_ptr<CommandRuntime>(new CommandRuntime{device});
        context->coordinator = this;
        context->Initialize(
            read, write,
            [this](VAddr address, u64 size) -> std::optional<VAddr> {
                const auto identity = resources.Canonicalize(address, size);
                return identity.physical ? std::optional<VAddr>{identity.address} : std::nullopt;
            },
            [this](VAddr a, u64 an, VAddr b, u64 bn) { return resources.Aliases(a, an, b, bn); });
        context->resources.SetBackingResolver(backing);
        context->inputs.SetBackingResolver(backing);
        context->inputs.SetAsyncRecording(true);
        context->resources.SetAsyncRecording(true);
        context->images.SetAsyncRecording(true, 16);
        auto* worker = context.get();
        context->SetMemoryAccess([this, worker,
                                  acquire](std::span<const ResourceCache::Range> ranges) {
            DrainContexts(ranges, false);
            for (auto& other : contexts) {
                if (other.get() == worker || other->completing || other->recording_context ||
                    other->synchronizing_images)
                    continue;
                const auto watches = other->ResourceWatches();
                const bool dirty = std::ranges::any_of(watches, [&](const auto& watch) {
                    return watch.dirty && std::ranges::any_of(ranges, [&](const auto& requested) {
                               return resources.Aliases(requested.address, requested.size,
                                                        watch.address, watch.size);
                           });
                });
                if (dirty && other->completion)
                    other->CompleteContext();
                other->SynchronizeBuffers(ranges, true);
                other->SynchronizeImages(ranges, true);
            }
            worker->pending_ranges.insert(worker->pending_ranges.end(), ranges.begin(),
                                          ranges.end());
            return acquire(ranges);
        });
        contexts.push_back(std::move(context));
        if (image_watches)
            worker->SetImageResidency([this](auto) { UpdateImageWatches(); });
    }
}
CommandRuntime& CommandRuntime::NextContext(std::span<const ResourceCache::Range> writes,
                                            std::span<const ResourceCache::Range> reads) {
    const auto select = [&](CommandRuntime& context) -> CommandRuntime& {
        context.pending_ranges.clear();
        context.write_ranges.assign(writes.begin(), writes.end());
        context.context_order = ++next_order;
        last_context = &context;
        return context;
    };
    for (const auto affinity : {reads, writes})
        for (auto& context : contexts) {
            const auto watches = context->ResourceWatches();
            if (!std::ranges::any_of(affinity, [&](const auto& requested) {
                    return std::ranges::any_of(watches, [&](const auto& watch) {
                        return resources.Aliases(requested.address, requested.size, watch.address,
                                                 watch.size);
                    });
                }))
                continue;
            if (context->completion) {
                ++async_statistics.hazard_waits;
                context->CompleteContext();
            }
            return select(*context);
        }
    for (auto& context : contexts)
        if (std::ranges::any_of(writes, [&](const auto& next) {
                return std::ranges::any_of(context->write_ranges, [&](const auto& previous) {
                    return resources.Aliases(next.address, next.size, previous.address,
                                             previous.size);
                });
            })) {
            if (context->completion) {
                ++async_statistics.hazard_waits;
                context->CompleteContext();
            }
            return select(*context);
        }
    for (auto& context : contexts)
        if (!context->completion) {
            return select(*context);
        }
    ++async_statistics.capacity_waits;
    SubmitReady();
    auto oldest = std::ranges::min_element(
        contexts, {}, [](const auto& context) { return context->context_order; });
    (*oldest)->CompleteContext();
    return select(**oldest);
}
void CommandRuntime::PrepareContext(std::span<CompiledShader*> stages) {
    if (!coordinator)
        return;
    const bool uses_gds = std::ranges::any_of(stages, [](const auto* shader) {
        return std::ranges::any_of(shader->info.buffers, [](const auto& buffer) {
            return buffer.buffer_type == Shader::BufferType::GdsBuffer;
        });
    });
    if (uses_gds) {
        coordinator->DrainContexts();
        gds = coordinator->gds;
    }
}
void CommandRuntime::FinishRecording(std::shared_ptr<void> lease, ResourceCache::Buffer* gds_buffer,
                                     std::vector<ResourceCache::Range> cpu_ranges,
                                     ResourceCache::Buffer* strmout_counter, bool mapping_stable) {
    const auto commit = [this, lease, gds_buffer, strmout_counter, mapping_stable,
                         cpu_ranges = std::move(cpu_ranges)] {
        resources.CommitDownloads(mapping_stable);
        CompleteOcclusion();
        if (strmout_counter) {
            std::memcpy(&strmout_filled, strmout_counter->host_data.data(), sizeof(strmout_filled));
        }
        if (gds_buffer) {
            std::memcpy(gds.data(), gds_buffer->host_data.data(), gds.size());
            if (coordinator)
                coordinator->gds = gds;
        }
        CompleteImages(cpu_ranges);
    };
    if (!coordinator) {
        device.Wait(SubmitCommands());
        commit();
        return;
    }
    pending_lease = std::move(lease);
    completion = commit;
    ready = true;
    ++coordinator->async_statistics.commands;
    u32 active{}, waiting{};
    for (const auto& context : coordinator->contexts) {
        active += bool(context->completion);
        waiting += context->ready;
    }
    coordinator->async_statistics.peak_in_flight =
        std::max(coordinator->async_statistics.peak_in_flight, active);
    if (waiting >= 2)
        coordinator->SubmitReady();
}
void CommandRuntime::SubmitReady() {
    auto lock = ProfileSample(runtime_profile, "scheduler_lock", 512,
                              [&] { return std::unique_lock{mutex}; });
    if (failed)
        throw std::runtime_error("D3D12 cannot submit commands after failure");
    std::vector<CommandRuntime*> pending;
    for (auto& context : contexts)
        if (context->ready)
            pending.push_back(context.get());
    if (pending.empty())
        return;
    std::ranges::sort(pending, {}, &CommandRuntime::context_order);
    std::vector<ID3D12CommandList*> lists;
    for (auto* context : pending)
        lists.push_back(context->commands.Get());
    for (auto* context : pending)
        context->ready = false;
    u64 fence{};
    try {
        fence = device.SubmitBatch(lists.data(), static_cast<UINT>(lists.size()));
    } catch (...) {
        StopAfterFailure();
        throw;
    }
    ++async_statistics.batches;
    for (auto* context : pending) {
        context->pending_fence = fence;
        context->ready = false;
        context->SubmitCommands(fence);
    }
}
void CommandRuntime::CompleteContext() {
    if (!completion)
        return;
    if (ready)
        coordinator->SubmitReady();
    if (!pending_fence)
        throw std::runtime_error("Native submission has no completion fence");
    try {
        ProfileSample(runtime_profile, "context_fence_wait", 128,
                      [&] { device.Wait(pending_fence); });
        completing = true;
        completion();
        completion = {};
        pending_lease.reset();
        pending_ranges.clear();
        pending_fence = 0;
        completing = false;
    } catch (...) {
        completing = false;
        StopAfterFailure();
        throw;
    }
}
void CommandRuntime::DrainContexts(std::span<const ResourceCache::Range> ranges, bool all) {
    auto lock = ProfileSample(runtime_profile, "scheduler_lock", 512,
                              [&] { return std::unique_lock{mutex}; });
    std::vector<CommandRuntime*> pending;
    for (auto& context : contexts) {
        if (!context->completion || context->completing || context->recording_context)
            continue;
        const bool overlaps =
            all || std::ranges::any_of(ranges, [&](const auto& requested) {
                return std::ranges::any_of(context->pending_ranges, [&](const auto& used) {
                    return resources.Aliases(requested.address, requested.size, used.address,
                                             used.size);
                });
            });
        if (overlaps)
            pending.push_back(context.get());
    }
    if (pending.empty())
        return;
    if (!all)
        ++async_statistics.hazard_waits;
    SubmitReady();
    std::ranges::sort(pending, {}, &CommandRuntime::context_order);
    for (auto* context : pending)
        context->CompleteContext();
}
std::span<u8, 65536> CommandRuntime::Gds() {
    auto lock = ProfileSample(runtime_profile, "scheduler_lock", 512,
                              [&] { return std::unique_lock{mutex}; });
    DrainContexts();
    return gds;
}
u64 CommandRuntime::OcclusionCounter(bool reset) {
    std::scoped_lock lock{mutex};
    DrainContexts();
    if (reset)
        occlusion_total = 0;
    occlusion_enabled = true;
    return occlusion_total;
}
void CommandRuntime::BeginOcclusion(bool enabled) {
    if (!enabled || !(coordinator ? coordinator : this)->occlusion_enabled)
        return;
    if (occlusion_recording)
        throw std::runtime_error("Nested native occlusion query");
    if (!occlusion_heap) {
        D3D12_QUERY_HEAP_DESC description{};
        description.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
        description.Count = 1;
        winrt::check_hresult(
            device.Device()->CreateQueryHeap(&description, IID_PPV_ARGS(&occlusion_heap)));
    }
    if (occlusion_queries % 4096 == 0)
        occlusion_readbacks.push_back(
            transfer_pool->Acquire(4096 * sizeof(u64), D3D12_HEAP_TYPE_READBACK));
    commands->BeginQuery(occlusion_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    occlusion_recording = true;
}
void CommandRuntime::EndOcclusion() {
    if (!occlusion_recording)
        return;
    commands->EndQuery(occlusion_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    commands->ResolveQueryData(occlusion_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, 1,
                               occlusion_readbacks.back().Get(),
                               u64(occlusion_queries % 4096) * sizeof(u64));
    ++occlusion_queries;
    occlusion_recording = false;
}
void CommandRuntime::CompleteOcclusion() {
    for (u32 block = 0; block < occlusion_readbacks.size(); ++block) {
        const u32 count = std::min(4096u, occlusion_queries - block * 4096);
        void* mapped{};
        D3D12_RANGE range{0, size_t(count) * sizeof(u64)};
        winrt::check_hresult(occlusion_readbacks[block]->Map(0, &range, &mapped));
        const auto* values = static_cast<const u64*>(mapped);
        for (u32 index = 0; index < count; ++index)
            (coordinator ? coordinator : this)->occlusion_total += values[index];
        D3D12_RANGE empty{};
        occlusion_readbacks[block]->Unmap(0, &empty);
        transfer_pool->Retire(std::move(occlusion_readbacks[block]), device.CompletedValue());
    }
    occlusion_readbacks.clear();
    occlusion_queries = 0;
}
} // namespace D3D12
