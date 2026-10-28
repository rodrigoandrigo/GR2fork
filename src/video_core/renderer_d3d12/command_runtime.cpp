#include "video_core/renderer_d3d12/command_runtime.h"
#include "video_core/renderer_d3d12/image_buffer_alias.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <winrt/base.h>
#include "common/logging/log.h"
#include "video_core/renderer_d3d12/cmask_address.h"
#include "video_core/renderer_d3d12/color_clear.h"
#include "video_core/renderer_d3d12/profile.h"
#include "video_core/renderer_d3d12/resource_bindings.h"
#include "video_core/renderer_d3d12/temporal_jitter.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/texture_cache/cpu_tiling.h"

namespace D3D12 {
namespace {
Profile runtime_profile;
std::atomic<u64> runtime_identity{1};
} // namespace
CommandRuntime::CommandRuntime() = default;
u64 CommandRuntime::NextRuntimeIdentity() {
    return runtime_identity.fetch_add(1, std::memory_order_relaxed);
}
CommandRuntime::~CommandRuntime() {
    try {
        if (failed) {
            if (!recovery_abandoned)
                device.DrainAfterFailure();
            for (auto& context : contexts) {
                context->failed = true;
                context->recovery_abandoned = recovery_abandoned;
            }
        } else {
            DrainContexts();
        }
        if (!failed && device.DirectQueue())
            device.WaitForGpu();
        if (!failed)
            ReadGpuTiming();
        if (gpu_timing.submissions)
            std::fprintf(stderr, "NATIVE_GPU final_average_us=%.3f submissions=%llu\n",
                         double(gpu_timing.ticks) * 1000000.0 / double(gpu_timing.frequency) /
                             double(gpu_timing.submissions),
                         static_cast<unsigned long long>(gpu_timing.submissions));
    } catch (...) {
        device.DrainAfterFailure();
    }
    contexts.clear();
}
void CommandRuntime::StopAfterFailure() {
    auto* owner = coordinator ? coordinator : this;
    std::scoped_lock lock{owner->mutex};
    owner->failed = true;
    for (auto& context : owner->contexts)
        context->failed = true;
}
CommandRuntime::RecoveryCheckpoint CommandRuntime::CreateRecoveryCheckpoint() {
    std::scoped_lock lock{mutex};
    if (coordinator)
        throw std::runtime_error("D3D12 recovery requires coordinator access");
    if (image_mapping_count.load())
        return {};
    Flush();
    RecoveryCheckpoint checkpoint{};
    checkpoint.owner = this;
    checkpoint.identity = recovery_identity;
    checkpoint.device = device.Device();
    checkpoint.submitted_lists = device.SubmittedLists();
    checkpoint.gds = gds;
    checkpoint.strmout_filled = StrmoutFilledSize();
    checkpoint.occlusion_total = occlusion_total;
    checkpoint.occlusion_enabled = occlusion_enabled;
    return checkpoint;
}
bool CommandRuntime::CanRecover(const RecoveryCheckpoint& checkpoint) {
    std::scoped_lock lock{mutex};
    return !coordinator && !image_mapping_count.load() && checkpoint.owner == this &&
           checkpoint.identity == recovery_identity && checkpoint.device == device.Device() &&
           checkpoint.submitted_lists == device.SubmittedLists();
}
void CommandRuntime::AbandonForRecovery(const RecoveryCheckpoint& checkpoint) {
    std::scoped_lock lock{mutex};
    if (!CanRecover(checkpoint))
        throw std::runtime_error("D3D12 recovery cannot replay submitted guest commands");
    failed = recovery_abandoned = true;
    for (auto& context : contexts)
        context->failed = context->recovery_abandoned = true;
}
void CommandRuntime::RestoreRecoveryCheckpoint(const RecoveryCheckpoint& checkpoint) {
    std::scoped_lock lock{mutex};
    if (!checkpoint.owner || !checkpoint.device || !device.Device() || failed || coordinator ||
        device.SubmittedLists() || !contexts.empty())
        throw std::runtime_error("D3D12 recovery requires a fresh command runtime");
    gds = checkpoint.gds;
    strmout_filled = checkpoint.strmout_filled;
    occlusion_total = checkpoint.occlusion_total;
    occlusion_enabled = checkpoint.occlusion_enabled;
}
const CommandRuntime::GpuTimingStatistics& CommandRuntime::GpuTimings() {
    auto lock = ProfileSample(runtime_profile, "runtime_lock", 512,
                              [&] { return std::unique_lock{mutex}; });
    ReadGpuTiming();
    if (!contexts.empty()) {
        gpu_timing = {};
        for (auto& context : contexts) {
            const auto& timing = context->GpuTimings();
            gpu_timing.submissions += timing.submissions;
            gpu_timing.ticks += timing.ticks;
            gpu_timing.frequency = timing.frequency;
        }
    }
    return gpu_timing;
}
void CommandRuntime::ReadGpuTiming() {
    if (!timestamp_fence || device.CompletedValue() < timestamp_fence)
        return;
    void* data{};
    const D3D12_RANGE range{0, 2 * sizeof(u64)};
    winrt::check_hresult(timestamp_readback->Map(0, &range, &data));
    std::array<u64, 2> ticks{};
    std::memcpy(ticks.data(), data, sizeof(ticks));
    const D3D12_RANGE empty{0, 0};
    timestamp_readback->Unmap(0, &empty);
    gpu_timing.ticks += ticks[1] - ticks[0];
    ++gpu_timing.submissions;
    timestamp_fence = 0;
    if (gpu_timing.submissions % 128 == 0)
        std::fprintf(stderr, "NATIVE_GPU average_us=%.3f submissions=%llu\n",
                     double(gpu_timing.ticks) * 1000000.0 / double(gpu_timing.frequency) /
                         double(gpu_timing.submissions),
                     static_cast<unsigned long long>(gpu_timing.submissions));
}
void CommandRuntime::BeginGpuTiming() {
    ReadGpuTiming();
    timestamp_recording = timestamp_heap && !timestamp_fence;
    timestamp_recorded = false;
    if (timestamp_recording)
        commands->EndQuery(timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
}
void CommandRuntime::EndGpuTiming() {
    if (!timestamp_recording)
        return;
    commands->EndQuery(timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    commands->ResolveQueryData(timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                               timestamp_readback.Get(), 0);
    timestamp_recording = false;
    timestamp_recorded = true;
}

std::shared_ptr<VideoCore::ResidentFrame> CommandRuntime::CapturePresentation(
    const VideoCore::ImageInfo& info) {
    auto lock = ProfileSample(runtime_profile, "runtime_lock", 512,
                              [&] { return std::unique_lock{mutex}; });
    ProfileScope capture{runtime_profile, "capture_resident"};
    MaintainMemory();
    if (!contexts.empty()) {
        const ResourceCache::Range requested{info.guest_address, info.guest_size};
        DrainContexts({&requested, 1}, false);
        for (auto& context : contexts)
            if (context->images.FindPresentation(info)) {
                context->CompleteContext();
                return context->CapturePresentation(info);
            }
        return {};
    }
    if (failed || image_mapping_count.load())
        return {};
    BeginImages();
    auto* image = images.FindPresentation(info);
    if (!image)
        return {};
    auto description = image->Native()->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    u64 bytes{};
    device.Device()->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr,
                                           &bytes);
    if (bytes > 128ULL * 1024 * 1024)
        return {};
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = bytes;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> snapshot;
    winrt::check_hresult(device.Device()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_SHARED, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&snapshot)));
    auto frame = std::make_shared<VideoCore::ResidentFrame>();
    HANDLE handle{};
    winrt::check_hresult(device.Device()->CreateSharedHandle(snapshot.Get(), nullptr, GENERIC_ALL,
                                                             nullptr, &handle));
    frame->handle = std::shared_ptr<void>(handle, [](void* value) { CloseHandle(value); });
    frame->bytes = bytes;
    frame->row_pitch = footprint.Footprint.RowPitch;
    winrt::check_hresult(allocator->Reset());
    winrt::check_hresult(commands->Reset(allocator.Get(), nullptr));
    BeginGpuTiming();
    RecordingGuard recording{*this};
    const auto previous = image->state;
    images.Barrier(*image, commands.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
    source.pResource = image->Native();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.pResource = snapshot.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    if (runtime_profile.enabled)
        runtime_profile.Record("capture_copy_bytes", 128, 0, bytes);
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    images.Barrier(*image, commands.Get(), previous);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {snapshot.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON};
    commands->ResourceBarrier(1, &barrier);
    EndGpuTiming();
    winrt::check_hresult(commands->Close());
    ProfileSample(runtime_profile, "runtime_submit_wait", 128,
                  [&] { device.Wait(SubmitCommands()); });
    recording.completed = true;
    return frame;
}
u32 CommandRuntime::StrmoutFilledSize() {
    std::scoped_lock lock{mutex};
    if (last_context) {
        last_context->CompleteContext();
        return last_context->strmout_filled;
    }
    return strmout_filled;
}
u64 CommandRuntime::Flush() {
    auto lock = ProfileSample(runtime_profile, "runtime_lock", 512,
                              [&] { return std::unique_lock{mutex}; });
    if (failed)
        throw std::runtime_error(
            "D3D12 command runtime requires recreation after recording failure");
    DrainContexts();
    for (auto& context : contexts)
        if (!context->failed)
            context->Flush();
    SynchronizeBuffers({}, false, true);
    SynchronizeImages({}, false, true);
    device.WaitForGpu();
    return device.LastSignaledValue();
}
void CommandRuntime::Initialize(ResourceCache::Read read, ResourceCache::Write write,
                                ResourceCache::Resolve resolve, ResourceCache::Overlaps overlaps) {
    if (!device.Device())
        device.Initialize();
    if (!coordinator)
        memory_budget.Initialize(device.Device());
    transfer_pool = std::make_shared<TransferPool>();
    transfer_pool->Initialize(device);
    persistent_pipelines.Initialize("pipelines", device.Device());
    LOG_INFO(Core, "[UWP stage] native driver pipeline disk cache: {}",
             persistent_pipelines.Enabled() ? "enabled" : "disabled");
    NativeTemporalJitter().Reset();
    shaders.Initialize(device.Device(), &resources);
    this->read = read;
    this->write = write;
    resources.Initialize(device, read, write, std::move(resolve), std::move(overlaps),
                         transfer_pool);
    inputs.Initialize(
        device, read, write, {},
        [this](VAddr a, u64 an, VAddr b, u64 bn) { return resources.Aliases(a, an, b, bn); },
        transfer_pool);
    images.Initialize(
        device, std::move(read), std::move(write),
        [this](VAddr address, u64 size) { return resources.Canonicalize(address, size); }, &shaders,
        transfer_pool, &resources);
    resource_descriptors.Initialize(device.Device(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 512,
                                    true);
    sampler_descriptors.Initialize(device.Device(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 128, true);
    color_descriptors.Initialize(device.Device(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8, false);
    depth_descriptors.Initialize(device.Device(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, false);
    static_cast<void>(resource_descriptors.Allocate(512));
    static_cast<void>(sampler_descriptors.Allocate(128));
    static_cast<void>(color_descriptors.Allocate(8));
    static_cast<void>(depth_descriptors.Allocate(2));
    if (std::getenv("GAME_PROFILE") &&
        SUCCEEDED(device.DirectQueue()->GetTimestampFrequency(&gpu_timing.frequency)) &&
        gpu_timing.frequency) {
        D3D12_QUERY_HEAP_DESC query{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0};
        D3D12_RESOURCE_DESC readback{};
        readback.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readback.Width = 2 * sizeof(u64);
        readback.Height = readback.DepthOrArraySize = readback.MipLevels = 1;
        readback.SampleDesc.Count = 1;
        readback.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        if (FAILED(device.Device()->CreateQueryHeap(&query, IID_PPV_ARGS(&timestamp_heap))) ||
            FAILED(device.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &readback,
                                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                            IID_PPV_ARGS(&timestamp_readback)))) {
            timestamp_heap.Reset();
            timestamp_readback.Reset();
            gpu_timing.frequency = 0;
            LOG_WARNING(Core, "Native D3D12 GPU timestamps unavailable");
        }
    }
    device.CreateDirectCommands(allocator, commands);
    EndGpuTiming();
    winrt::check_hresult(commands->Close());
}
struct CommandRuntime::CpuImageAccess {
    std::unique_lock<std::recursive_mutex> lock;
    explicit CpuImageAccess(CommandRuntime& runtime) : lock{runtime.mutex} {}
};
struct CommandRuntime::ImageMappingAccess {
    CommandRuntime& runtime;
    explicit ImageMappingAccess(CommandRuntime& runtime) : runtime{runtime} {
        std::scoped_lock transition{runtime.image_mapping_mutex};
        if (runtime.image_mapping_count.load() == 0)
            runtime.image_mapping_ready.store(false);
        if (runtime.image_mapping_count.fetch_add(1) == 0) {
            try {
                std::scoped_lock lock{runtime.mutex};
                runtime.SynchronizeImages({}, true, true);
                runtime.image_mapping_ready.store(true);
            } catch (...) {
                --runtime.image_mapping_count;
                throw;
            }
        }
    }
    ~ImageMappingAccess() {
        --runtime.image_mapping_count;
    }
};
void CommandRuntime::SetImageResidency(ImageWatches callback) {
    auto lock = ProfileSample(runtime_profile, "runtime_lock", 512,
                              [&] { return std::unique_lock{mutex}; });
    DrainContexts();
    for (auto& context : contexts)
        context->SetImageResidency(callback ? ImageWatches{[this](auto) { UpdateImageWatches(); }}
                                            : ImageWatches{});
    if (image_watches)
        SynchronizeImages({}, true, true);
    SynchronizeBuffers({}, true, true);
    inputs.ConfigureResidency(bool(callback) && coordinator);
    resources.ConfigureResidency(bool(callback) && coordinator, [this](VAddr address, u64 size) {
        const ResourceCache::Range range{address, size};
        SynchronizeBuffers({&range, 1}, true);
    });
    image_watches = std::move(callback);
    BeginImages();
    UpdateImageWatches();
}
std::vector<ImageCache::Watch> CommandRuntime::ResourceWatches() const {
    auto watches = images.Watches();
    for (const auto& watch : inputs.Watches())
        watches.push_back({watch.address, watch.size, false});
    for (const auto& watch : resources.Watches())
        watches.push_back({watch.address, watch.size, watch.dirty});
    return watches;
}
void CommandRuntime::SynchronizeBuffers(std::span<const ResourceCache::Range> ranges,
                                        bool invalidate, bool all) {
    if (resources.RequestDownloads(ranges, all)) {
        if (buffer_commands_open)
            throw std::runtime_error("D3D12 buffer ownership must be resolved before recording");
        struct Scope {
            bool& value;
            ~Scope() {
                value = false;
            }
        } scope{synchronizing_images};
        synchronizing_images = true;
        std::vector<ResourceCache::Range> access;
        for (const auto& watch : resources.Watches())
            if (watch.dirty)
                access.push_back({watch.address, watch.size});
        const auto lease = acquire_memory ? acquire_memory(access) : nullptr;
        winrt::check_hresult(allocator->Reset());
        winrt::check_hresult(commands->Reset(allocator.Get(), nullptr));
        RecordingGuard recording{*this};
        resources.PrepareDownload(commands.Get());
        winrt::check_hresult(commands->Close());
        ProfileSample(runtime_profile, "runtime_submit_wait", 128,
                      [&] { device.Wait(SubmitCommands()); });
        resources.CommitDownloads(bool(lease));
        recording.completed = true;
    }
    if (invalidate) {
        resources.InvalidateCpu(ranges, all);
        inputs.InvalidateCpu(ranges, all);
    }
    UpdateImageWatches();
}
void CommandRuntime::UpdateImageWatches() {
    if (!contexts.empty() && image_watches) {
        auto watches = ResourceWatches();
        for (auto& context : contexts) {
            auto next = context->ResourceWatches();
            watches.insert(watches.end(), next.begin(), next.end());
        }
        image_watches(watches);
        return;
    }
    if (image_watches)
        image_watches(ResourceWatches());
}
void CommandRuntime::BeginImages() {
    const bool enabled = bool(image_watches) && image_mapping_count.load() == 0;
    if (!enabled && image_watches)
        SynchronizeImages({}, true, true);
    const auto generation = resources.MappingGeneration();
    if (generation != image_mapping_generation) {
        if (images.HasDirtyImages())
            SynchronizeImages({}, true, true);
        images.InvalidateCpu({}, true);
        image_mapping_generation = generation;
    }
    images.ConfigureResidency(enabled, [this](VAddr address, u64 size) {
        const ResourceCache::Range range{address, size};
        SynchronizeImages({&range, 1}, false);
    });
    images.BeginBatch();
}
void CommandRuntime::SynchronizeImages(std::span<const ResourceCache::Range> ranges,
                                       bool invalidate, bool all) {
    if (synchronizing_images)
        throw std::runtime_error("Recursive D3D12 image synchronization");
    if (images.RequestDownloads(ranges, all)) {
        struct Scope {
            bool& value;
            ~Scope() {
                value = false;
            }
        } scope{synchronizing_images};
        synchronizing_images = true;
        std::vector<ResourceCache::Range> access;
        for (const auto& watch : images.Watches())
            if (watch.dirty)
                access.push_back({watch.address, watch.size});
        const auto memory_access = acquire_memory ? acquire_memory(access) : nullptr;
        winrt::check_hresult(allocator->Reset());
        winrt::check_hresult(commands->Reset(allocator.Get(), nullptr));
        BeginGpuTiming();
        RecordingGuard recording{*this};
        images.PrepareDownloads(commands.Get());
        EndGpuTiming();
        winrt::check_hresult(commands->Close());
        ProfileSample(runtime_profile, "runtime_submit_wait", 128,
                      [&] { device.Wait(SubmitCommands()); });
        images.CommitDownloads();
        recording.completed = true;
    }
    if (invalidate)
        images.InvalidateCpu(ranges, all);
    UpdateImageWatches();
}
std::shared_ptr<void> CommandRuntime::AcquireCpuAccess(VAddr address, u64 size, bool write) {
    std::unique_lock parent_lock{mutex};
    const ResourceCache::Range requested{address, size};
    DrainContexts({&requested, 1}, false);
    for (auto& context : contexts)
        if (!context->completing && !context->recording_context && !context->synchronizing_images) {
            const auto watches = context->ResourceWatches();
            if (context->completion && std::ranges::any_of(watches, [&](const auto& watch) {
                    return watch.dirty &&
                           resources.Aliases(address, size, watch.address, watch.size);
                }))
                context->CompleteContext();
            static_cast<void>(context->AcquireCpuAccess(address, size, write));
        }
    const bool internal_access =
        buffer_commands_open || recording_context || synchronizing_images || completing ||
        std::ranges::any_of(contexts, [](const auto& context) {
            return context->recording_context || context->buffer_commands_open ||
                   context->synchronizing_images || context->completing;
        });
    if (image_mapping_count.load() && image_mapping_ready.load() && !internal_access)
        return AcquireMemoryMappingAccess();
    auto access = std::make_shared<CpuImageAccess>(*this);
    if (failed)
        throw std::runtime_error("CPU access to failed D3D12 image owner");
    if (buffer_commands_open || recording_context || synchronizing_images || completing)
        return access;
    const ResourceCache::Range range{address, size};
    SynchronizeBuffers({&range, 1}, write);
    SynchronizeImages({&range, 1}, write);
    return access;
}
std::shared_ptr<void> CommandRuntime::AcquireMemoryMappingAccess() {
    auto lock = ProfileSample(runtime_profile, "runtime_lock", 512,
                              [&] { return std::unique_lock{mutex}; });
    DrainContexts();
    for (auto& context : contexts) {
        context->SynchronizeBuffers({}, true, true);
        context->resources.InvalidateMappings();
        context->inputs.InvalidateMappings();
        context->SynchronizeImages({}, true, true);
    }
    SynchronizeBuffers({}, true, true);
    resources.InvalidateMappings();
    inputs.InvalidateMappings();
    return std::make_shared<ImageMappingAccess>(*this);
}
void CommandRuntime::CompleteImages(std::span<const ResourceCache::Range> cpu_ranges) {
    images.CommitDownloads();
    images.InvalidateCpu(cpu_ranges);
    inputs.InvalidateCpu(cpu_ranges);
    for (const auto& watch : images.Watches())
        if (watch.dirty) {
            const ResourceCache::Range range{watch.address, watch.size};
            inputs.InvalidateCpu({&range, 1});
        }
    UpdateImageWatches();
}
u64 CommandRuntime::SubmitCommands(u64 fence) {
    if (!fence)
        fence = device.Submit(commands.Get());
    if (timestamp_recorded) {
        timestamp_fence = fence;
        timestamp_recorded = false;
    }
    resources.RetireUploads(fence);
    inputs.RetireUploads(fence);
    images.RetireUploads(fence);
    for (auto& input : input_resources)
        transfer_pool->Retire(std::move(input), fence);
    input_resources.clear();
    return fence;
}
void CommandRuntime::MaterializeColorClear(const AmdGpu::ColorBuffer& buffer,
                                           const VideoCore::ImageInfo& info) {
    if (!buffer.info.fast_clear || !buffer.cmask_base_address)
        return;
    auto lock = ProfileSample(runtime_profile, "runtime_lock", 512,
                              [&] { return std::unique_lock{mutex}; });
    MaintainMemory();
    if (!contexts.empty()) {
        const std::array<ResourceCache::Range, 2> ranges{
            {{info.guest_address, info.guest_size}, {buffer.CmaskAddress(), CmaskBytes(buffer)}}};
        auto& context = NextContext(ranges);
        struct Scope {
            bool& active;
            ~Scope() {
                active = false;
            }
        } scope{context.recording_context};
        context.recording_context = true;
        context.MaterializeColorClear(buffer, info);
        return;
    }
    DrainContexts();
    if (failed)
        throw std::runtime_error(
            "D3D12 command runtime requires recreation after recording failure");
    if (info.guest_size > 64 * 1024 * 1024)
        throw std::runtime_error("D3D12 CMASK exceeds color budget");
    device.WaitForGpu();
    const std::array<ResourceCache::Range, 2> ranges{
        {{info.guest_address, info.guest_size}, {buffer.CmaskAddress(), CmaskBytes(buffer)}}};
    SynchronizeImages(ranges, true);
    const auto memory_access = acquire_memory ? acquire_memory(ranges) : nullptr;
    MaterializeColorClearImpl(buffer, info);
}
void CommandRuntime::MaterializeColorClearImpl(const AmdGpu::ColorBuffer& buffer,
                                               const VideoCore::ImageInfo& info) {
    if (!buffer.info.fast_clear || !buffer.cmask_base_address)
        return;
    const u64 slice_bytes = (u64(buffer.cmask_slice.tile_max) + 1) * 128;
    const u64 metadata_bytes = CmaskBytes(buffer);
    if (!info.guest_address || !info.guest_size || info.guest_size > 64 * 1024 * 1024 ||
        metadata_bytes > 64 * 1024 * 1024 || !info.size.width || !info.size.height ||
        info.size.width > info.pitch || info.size.height > info.mips_layout[0].height ||
        buffer.BaseSlice() >= buffer.NumSlices() ||
        info.guest_address > UINT64_MAX - info.guest_size ||
        buffer.CmaskAddress() > UINT64_MAX - metadata_bytes)
        throw std::runtime_error("Invalid D3D12 CMASK storage or view");
    std::vector<u8> metadata(metadata_bytes);
    read(buffer.CmaskAddress(), metadata);
    const auto original_metadata = metadata;
    struct Tile {
        u32 layer, x, y;
    };
    std::vector<Tile> tiles;
    std::optional<CmaskAddress> address;
    const u32 coverage_log2 = buffer.attrib.num_samples_log2;
    if (buffer.info.compression &&
        (!coverage_log2 || coverage_log2 > 3 || buffer.attrib.num_fragments_log2 != coverage_log2))
        throw std::runtime_error("Unsupported D3D12 CMASK sample/fragment configuration");
    const u32 expanded_code = buffer.info.compression ? (12u | coverage_log2) : 15u;
    for (u32 layer = buffer.BaseSlice(); layer < buffer.NumSlices(); ++layer) {
        const auto slice = std::span{metadata}.subspan(layer * slice_bytes, slice_bytes);
        if (std::ranges::all_of(slice, [expanded_code](u8 value) {
                return value == (expanded_code | (expanded_code << 4));
            }))
            continue;
        if (!address) {
            if (buffer.info.dcc_enable || buffer.info.cmask_addr_type)
                throw std::runtime_error("D3D12 CMASK requires non-DCC non-TC-compatible color");
            address.emplace(buffer.Pitch(), buffer.Height(),
                            info.alt_tile ? AmdGpu::GetAltPipeConfig(buffer.GetTileMode())
                                          : AmdGpu::GetPipeConfig(buffer.GetTileMode()),
                            buffer.info.cmask_is_linear);
            if (address->slice_bytes != slice_bytes)
                throw std::runtime_error("D3D12 CMASK slice does not match descriptor");
        }
        for (u32 y = 0; y < info.size.height; y += 8)
            for (u32 x = 0; x < info.size.width; x += 8) {
                const auto [byte, shift] = address->At(layer, x, y);
                const u32 code = (metadata.at(byte) >> shift) & 15;
                const u32 clear_code = buffer.info.compression ? coverage_log2 : 0u;
                if (code != clear_code && code != expanded_code)
                    throw std::runtime_error("Unsupported D3D12 CMASK color/FMASK tile state");
                if (code == clear_code) {
                    tiles.push_back({layer, x, y});
                    metadata[byte] = (metadata[byte] & ~(15u << shift)) | (expanded_code << shift);
                }
            }
    }
    if (tiles.empty())
        return;
    const auto clear = DecodeColorClear(buffer);
    if (!clear || clear->pixel_bytes * 8 != info.num_bits ||
        clear->data_format != u32(buffer.GetDataFmt()) ||
        clear->number_format != u32(buffer.GetNumberFmt()))
        throw std::runtime_error("Unsupported D3D12 CMASK clear format");
    if (resources.Aliases(info.guest_address, info.guest_size, buffer.CmaskAddress(),
                          metadata_bytes))
        throw std::runtime_error("D3D12 CMASK metadata overlaps color storage");
    std::vector<u8> color(info.guest_size), linear(info.guest_size);
    read(info.guest_address, color);
    const auto original_color = color;
    VideoCore::ConvertTiling(color, linear, info, false);
    const auto& layout = info.mips_layout[0];
    for (const auto& tile : tiles)
        for (u32 sample = 0; sample < info.num_samples; ++sample)
            for (u32 y = tile.y; y < std::min(tile.y + 8, info.size.height); ++y)
                for (u32 x = tile.x; x < std::min(tile.x + 8, info.size.width); ++x) {
                    const u64 offset =
                        (((u64(tile.layer) * info.num_samples + sample) * layout.height + y) *
                             layout.pitch +
                         x) *
                        clear->pixel_bytes;
                    if (offset + clear->pixel_bytes > linear.size())
                        throw std::runtime_error("D3D12 CMASK texel exceeds color storage");
                    std::memcpy(linear.data() + offset, clear->pixel.data(), clear->pixel_bytes);
                }
    VideoCore::ConvertTiling(linear, color, info, true);
    const auto commit = [&](VAddr base, const std::vector<u8>& before,
                            const std::vector<u8>& after) {
        for (size_t offset = 0; offset < after.size();) {
            if (before[offset] == after[offset]) {
                ++offset;
                continue;
            }
            const size_t start = offset++;
            while (offset < after.size() && before[offset] != after[offset])
                ++offset;
            write(base + start, std::span{after}.subspan(start, offset - start));
        }
    };
    commit(info.guest_address, original_color, color);
    commit(buffer.CmaskAddress(), original_metadata, metadata);
}
void CommandRuntime::Dispatch(Shader::ShaderParams params, Shader::RuntimeInfo runtime,
                              std::array<u32, 3> groups, VAddr border_colors,
                              VAddr indirect_address) {
    auto lock = ProfileSample(runtime_profile, "runtime_lock", 512,
                              [&] { return std::unique_lock{mutex}; });
    MaintainMemory();
    if (!contexts.empty()) {
        const auto shader = shaders.Compile(params, runtime);
        std::vector<ResourceCache::Range> writes, reads;
        if (indirect_address)
            reads.push_back({indirect_address, 12});
        for (const auto& image : shader.info.images) {
            const auto info = ShaderImageLayout(image.GetSharp(shader.info), image);
            reads.push_back({info.guest_address, info.guest_size});
        }
        for (const auto& buffer : shader.info.buffers) {
            if (buffer.buffer_type != Shader::BufferType::Guest)
                continue;
            const auto sharp = buffer.GetSharp(shader.info);
            reads.push_back(
                {sharp.base_address, u64(sharp.num_records) * std::max<u32>(1, sharp.stride)});
            if (!buffer.is_written)
                continue;
            writes.push_back(
                {sharp.base_address, u64(sharp.num_records) * std::max<u32>(1, sharp.stride)});
        }
        for (const auto& image : shader.info.images) {
            if (!image.is_written)
                continue;
            const auto info = ShaderImageLayout(image.GetSharp(shader.info), image);
            writes.push_back({info.guest_address, info.guest_size});
        }
        auto& context = NextContext(writes, reads);
        struct Scope {
            bool& active;
            ~Scope() {
                active = false;
            }
        } scope{context.recording_context};
        context.recording_context = true;
        context.Dispatch(params, runtime, groups, border_colors, indirect_address);
        return;
    }
    if (failed)
        throw std::runtime_error(
            "D3D12 command runtime requires recreation after recording failure");
    if (runtime.sw_stage != Shader::SwStage::Compute)
        throw std::runtime_error("D3D12 dispatch requires a compute shader");
    const auto& local = runtime.hw.cs.workgroup_size;
    if (!local[0] || !local[1] || !local[2] || local[0] > 1024 || local[1] > 1024 ||
        local[2] > 64 || u64(local[0]) * local[1] * local[2] > 1024)
        throw std::runtime_error("Guest compute local size exceeds D3D12 limits");
    for (const auto count : groups) {
        if (!count)
            return;
        if (count > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION)
            throw std::runtime_error("Guest dispatch exceeds D3D12 workgroup limits");
    }
    auto shader = shaders.Compile(params, runtime);
    if (indirect_address && std::ranges::any_of(shader.info.buffers, [](const auto& buffer) {
            return buffer.buffer_type == Shader::BufferType::SharedMemory;
        }))
        throw GpuDrawFallback{};
    std::array<CompiledShader*, 1> stages{&shader};
    PrepareContext(stages);
    ResourceBindings bindings{device,
                              resources,
                              images,
                              stages,
                              {},
                              &resource_descriptors,
                              &sampler_descriptors,
                              coordinator != nullptr};
    std::vector<D3D12_ROOT_PARAMETER> parameters(2);
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 20};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants = {2, 0, 3};
    const auto add_table = [&](const auto& ranges) {
        const u32 slot = parameters.size();
        if (!ranges.empty()) {
            D3D12_ROOT_PARAMETER parameter{};
            parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameter.DescriptorTable = {static_cast<UINT>(ranges.size()), ranges.data()};
            parameters.push_back(parameter);
        }
        return slot;
    };
    const u32 resource_slot = add_table(bindings.ResourceRanges());
    const u32 sampler_slot = add_table(bindings.SamplerRanges());
    D3D12_ROOT_SIGNATURE_DESC root_description{};
    root_description.NumParameters = parameters.size();
    root_description.pParameters = parameters.data();
    Microsoft::WRL::ComPtr<ID3DBlob> root_blob, errors;
    winrt::check_hresult(D3D12SerializeRootSignature(
        &root_description, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, &errors));
    std::vector<u8> pipeline_key{0};
    pipeline_key.insert(pipeline_key.end(), shader.binary->dxil.begin(), shader.binary->dxil.end());
    const auto* root_bytes = static_cast<const u8*>(root_blob->GetBufferPointer());
    pipeline_key.insert(pipeline_key.end(), root_bytes, root_bytes + root_blob->GetBufferSize());
    auto& cached = GetPipeline(
        std::move(pipeline_key), root_blob.Get(),
        [&](auto* root, auto** state, D3D12_CACHED_PIPELINE_STATE cached) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
            description.pRootSignature = root;
            description.CachedPSO = cached;
            description.CS = {shader.binary->dxil.data(), shader.binary->dxil.size()};
            return device.Device()->CreateComputePipelineState(
                &description, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(state));
        });
    auto& root = cached.root;
    auto& pipeline = cached.state;
    resources.BeginBatch();
    BeginImages();
    const auto cpu_ranges = bindings.CpuRanges(border_colors);
    SynchronizeImages(cpu_ranges, true);
    SynchronizeBuffers(bindings.ImageRanges(), true);
    bindings.PrepareImages();
    auto access_ranges = bindings.AccessRanges(border_colors);
    if (indirect_address)
        access_ranges.push_back({indirect_address, 12});
    const auto memory_access = acquire_memory ? acquire_memory(access_ranges) : nullptr;
    resources.PrepareViews(bindings.GuestRanges());
    ResourceCache::View indirect_view{};
    if (indirect_address) {
        const ResourceCache::Range range{indirect_address, 12};
        SynchronizeImages({&range, 1}, true);
        resources.PrepareViews({&range, 1});
        indirect_view = resources.GetViews({&range, 1}).front();
        if (!indirect_view.buffer ||
            std::ranges::any_of(indirect_view.segments, [&](const auto& part) {
                return part.offset != indirect_view.offset + part.address - indirect_address;
            }))
            throw GpuDrawFallback{};
        gpu_commands.Initialize(device, shaders);
        gpu_commands.Prepare(1, 0);
    }
    winrt::check_hresult(allocator->Reset());
    winrt::check_hresult(commands->Reset(allocator.Get(), nullptr));
    buffer_commands_open = true;
    struct BufferScope {
        bool& open;
        ~BufferScope() {
            open = false;
        }
    } buffer_scope{buffer_commands_open};
    BeginGpuTiming();
    RecordingGuard recording{*this};
    if (indirect_address) {
        resources.Upload(*indirect_view.buffer, commands.Get());
        gpu_commands.RecordDispatch(commands.Get(), indirect_view);
    }
    Shader::PushData push{};
    const std::array<float, 24> planes{};
    std::vector<u8> shared_memory;
    if (std::ranges::any_of(shader.info.buffers, [](const auto& buffer) {
            return buffer.buffer_type == Shader::BufferType::SharedMemory;
        })) {
        const u64 group_count = u64(groups[0]) * groups[1] * groups[2];
        if (!runtime.hw.cs.shared_memory_size ||
            group_count > (16 * 1024 * 1024) / runtime.hw.cs.shared_memory_size)
            throw std::runtime_error("Guest shared memory storage exceeds D3D12 budget");
        shared_memory.resize(group_count * runtime.hw.cs.shared_memory_size);
    }
    bindings.Upload(commands.Get(), push, planes, border_colors, read, gds, shared_memory);
    std::array<u32, 20> constants{};
    commands->SetComputeRootSignature(root.Get());
    commands->SetPipelineState(pipeline.Get());
    static_assert(sizeof(push) <= sizeof(constants));
    std::memcpy(constants.data(), &push, sizeof(push));
    commands->SetComputeRoot32BitConstants(0, constants.size(), constants.data(), 0);
    commands->SetComputeRoot32BitConstants(1, groups.size(), groups.data(), 0);
    bindings.Bind(commands.Get(), false, resource_slot, sampler_slot);
    if (indirect_address) {
        gpu_commands.ExecuteDispatch(commands.Get(), root.Get());
        ++(coordinator ? coordinator : this)->async_statistics.gpu_dispatches;
    } else
        commands->Dispatch(groups[0], groups[1], groups[2]);
    resources.PrepareDownload(commands.Get());
    images.PrepareDownloads(commands.Get());
    EndGpuTiming();
    winrt::check_hresult(commands->Close());
    FinishRecording(memory_access, bindings.GdsBuffer(), bindings.CpuRanges(0, true));
    recording.completed = true;
}
void CommandRuntime::Resolve(const VideoCore::ImageInfo& source,
                             const VideoCore::ImageInfo& destination, u32 source_layer,
                             u32 destination_layer, u32 layers) {
    auto lock = ProfileSample(runtime_profile, "runtime_lock", 512,
                              [&] { return std::unique_lock{mutex}; });
    MaintainMemory();
    if (!contexts.empty()) {
        const ResourceCache::Range target{destination.guest_address, destination.guest_size};
        const ResourceCache::Range input{source.guest_address, source.guest_size};
        auto& context = NextContext({&target, 1}, {&input, 1});
        struct Scope {
            bool& active;
            ~Scope() {
                active = false;
            }
        } scope{context.recording_context};
        context.recording_context = true;
        context.Resolve(source, destination, source_layer, destination_layer, layers);
        return;
    }
    DrainContexts();
    if (failed)
        throw std::runtime_error(
            "D3D12 command runtime requires recreation after recording failure");
    BeginImages();
    const std::array<ResourceCache::Range, 2> access_ranges{
        {{source.guest_address, source.guest_size},
         {destination.guest_address, destination.guest_size}}};

    auto& input = images.GetImage(source, ConvertFormat(source.pixel_format),
                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, source_layer, layers);
    auto& output =
        images.GetImage(destination, ConvertFormat(destination.pixel_format),
                        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, destination_layer, layers);
    const auto memory_access = acquire_memory ? acquire_memory(access_ranges) : nullptr;
    winrt::check_hresult(allocator->Reset());
    winrt::check_hresult(commands->Reset(allocator.Get(), nullptr));
    BeginGpuTiming();
    RecordingGuard recording{*this};
    images.Resolve(input, output, commands.Get());
    images.PrepareDownloads(commands.Get());
    EndGpuTiming();
    winrt::check_hresult(commands->Close());
    ProfileSample(runtime_profile, "runtime_submit_wait", 128,
                  [&] { device.Wait(SubmitCommands()); });
    CompleteImages();
    recording.completed = true;
}
} // namespace D3D12
