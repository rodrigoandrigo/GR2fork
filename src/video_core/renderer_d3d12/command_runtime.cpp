#include "video_core/renderer_d3d12/command_runtime.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <winrt/base.h>
#include "common/logging/log.h"
#include "video_core/renderer_d3d12/cmask_address.h"
#include "video_core/renderer_d3d12/color_clear.h"
#include "video_core/renderer_d3d12/resource_bindings.h"
#include "video_core/renderer_d3d12/temporal_jitter.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/texture_cache/cpu_tiling.h"

namespace D3D12 {
std::shared_ptr<VideoCore::ResidentFrame> CommandRuntime::CapturePresentation(
    const VideoCore::ImageInfo& info) {
    std::scoped_lock lock{mutex};
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
    RecordingGuard recording{failed};
    const auto previous = image->state;
    images.Barrier(*image, commands.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
    source.pResource = image->Native();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.pResource = snapshot.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    images.Barrier(*image, commands.Get(), previous);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {snapshot.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON};
    commands->ResourceBarrier(1, &barrier);
    winrt::check_hresult(commands->Close());
    device.Wait(SubmitCommands());
    recording.completed = true;
    return frame;
}
u64 CommandRuntime::Flush() {
    std::scoped_lock lock{mutex};
    if (failed)
        throw std::runtime_error(
            "D3D12 command runtime requires recreation after recording failure");
    SynchronizeImages({}, false, true);
    device.WaitForGpu();
    return device.LastSignaledValue();
}
void CommandRuntime::Initialize(ResourceCache::Read read, ResourceCache::Write write,
                                ResourceCache::Resolve resolve, ResourceCache::Overlaps overlaps) {
    device.Initialize();
    transfer_pool = std::make_shared<TransferPool>();
    transfer_pool->Initialize(device);
    persistent_pipelines.Initialize("pipelines", device.Device());
    LOG_INFO(Core, "[UWP stage] native driver pipeline disk cache: {}",
             persistent_pipelines.Enabled() ? "enabled" : "disabled");
    NativeTemporalJitter().Reset();
    shaders.Initialize(device.Device(), &resources);
    this->read = read;
    this->write = write;
    resources.Initialize(device, read, write, std::move(resolve), std::move(overlaps), transfer_pool);
    images.Initialize(
        device, std::move(read), std::move(write),
        [this](VAddr address, u64 size) { return resources.Canonicalize(address, size); },
        &shaders, transfer_pool, &resources);
    device.CreateDirectCommands(allocator, commands);
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
    std::scoped_lock lock{mutex};
    if (image_watches)
        SynchronizeImages({}, true, true);
    image_watches = std::move(callback);
    BeginImages();
    UpdateImageWatches();
}
void CommandRuntime::UpdateImageWatches() {
    if (image_watches)
        image_watches(images.Watches());
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
        RecordingGuard recording{failed};
        images.PrepareDownloads(commands.Get());
        winrt::check_hresult(commands->Close());
        device.Wait(SubmitCommands());
        images.CommitDownloads();
        recording.completed = true;
    }
    if (invalidate)
        images.InvalidateCpu(ranges, all);
    UpdateImageWatches();
}
std::shared_ptr<void> CommandRuntime::AcquireCpuAccess(VAddr address, u64 size, bool write) {
    if (image_mapping_count.load() && image_mapping_ready.load())
        return AcquireMemoryMappingAccess();
    auto access = std::make_shared<CpuImageAccess>(*this);
    if (failed)
        throw std::runtime_error("CPU access to failed D3D12 image owner");
    const ResourceCache::Range range{address, size};
    SynchronizeImages({&range, 1}, write);
    return access;
}
std::shared_ptr<void> CommandRuntime::AcquireMemoryMappingAccess() {
    return std::make_shared<ImageMappingAccess>(*this);
}
void CommandRuntime::CompleteImages(std::span<const ResourceCache::Range> cpu_ranges) {
    images.CommitDownloads();
    images.InvalidateCpu(cpu_ranges);
    UpdateImageWatches();
}
u64 CommandRuntime::SubmitCommands() {
    const auto fence = device.Submit(commands.Get());
    resources.RetireUploads(fence);
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
    std::scoped_lock lock{mutex};
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
    for (u32 layer = buffer.BaseSlice(); layer < buffer.NumSlices(); ++layer) {
        const auto slice = std::span{metadata}.subspan(layer * slice_bytes, slice_bytes);
        if (std::ranges::all_of(slice, [](u8 value) { return value == 0xff; }))
            continue;
        if (!address) {
            if (buffer.info.compression || buffer.info.dcc_enable || buffer.info.cmask_addr_type)
                throw std::runtime_error(
                    "D3D12 CMASK requires uncompressed non-TC-compatible color");
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
                if (code != 0 && code != 15)
                    throw std::runtime_error("Unsupported D3D12 CMASK tile code");
                if (code == 0) {
                    tiles.push_back({layer, x, y});
                    metadata[byte] |= 15u << shift;
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
                              std::array<u32, 3> groups, VAddr border_colors) {
    std::scoped_lock lock{mutex};
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
    std::array<CompiledShader*, 1> stages{&shader};
    ResourceBindings bindings{device, resources, images, stages};
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
    auto& cached =
        GetPipeline(std::move(pipeline_key), root_blob.Get(), [&](auto* root, auto** state, D3D12_CACHED_PIPELINE_STATE cached) {
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
    bindings.PrepareImages();
    const auto access_ranges = bindings.AccessRanges(border_colors);
    const auto memory_access = acquire_memory ? acquire_memory(access_ranges) : nullptr;
    winrt::check_hresult(allocator->Reset());
    winrt::check_hresult(commands->Reset(allocator.Get(), nullptr));
    RecordingGuard recording{failed};
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
    commands->Dispatch(groups[0], groups[1], groups[2]);
    resources.PrepareDownload(commands.Get());
    images.PrepareDownloads(commands.Get());
    winrt::check_hresult(commands->Close());
    device.Wait(SubmitCommands());
    resources.CommitDownloads(bool(memory_access));
    bindings.CommitGds(gds);
    CompleteImages(bindings.CpuRanges(0, true));
    recording.completed = true;
}
void CommandRuntime::Resolve(const VideoCore::ImageInfo& source,
                             const VideoCore::ImageInfo& destination, u32 source_layer,
                             u32 destination_layer, u32 layers) {
    std::scoped_lock lock{mutex};
    if (failed)
        throw std::runtime_error(
            "D3D12 command runtime requires recreation after recording failure");
    BeginImages();
    const std::array<ResourceCache::Range, 2> access_ranges{{
        {source.guest_address, source.guest_size},
        {destination.guest_address, destination.guest_size}}};

    auto& input = images.GetImage(source, ConvertFormat(source.pixel_format),
                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, source_layer, layers);
    auto& output =
        images.GetImage(destination, ConvertFormat(destination.pixel_format),
                        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, destination_layer, layers);
    const auto memory_access = acquire_memory ? acquire_memory(access_ranges) : nullptr;
    winrt::check_hresult(allocator->Reset());
    winrt::check_hresult(commands->Reset(allocator.Get(), nullptr));
    RecordingGuard recording{failed};
    images.Resolve(input, output, commands.Get());
    images.PrepareDownloads(commands.Get());
    winrt::check_hresult(commands->Close());
    device.Wait(SubmitCommands());
    CompleteImages();
    recording.completed = true;
}
} // namespace D3D12
