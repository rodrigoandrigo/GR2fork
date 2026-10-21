#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/amdgpu/regs_vertex.h"
#include "video_core/renderer_d3d12/image_cache.h"
#include "video_core/renderer_d3d12/memory_budget.h"
#include "video_core/renderer_d3d12/gpu_commands.h"
#include "video_core/renderer_d3d12/resource_bindings.h"
#include "video_core/renderer_d3d12/resource_cache.h"
#include "video_core/renderer_d3d12/shader_cache.h"

namespace D3D12 {
class CommandRuntime {
public:
    CommandRuntime() = default;
    ~CommandRuntime();
    using AcquireMemory =
        std::function<std::shared_ptr<void>(std::span<const ResourceCache::Range>)>;
    void EnableAsync(ResourceCache::ResolveBacking backing, AcquireMemory acquire);
    void SubmitReady();
    void MaintainMemory(bool force = false);
    void SetBackground(bool background);
    void SetExternalMemory(std::function<u64()> usage, std::function<void()> trim) {
        std::scoped_lock lock{mutex};
        external_memory = std::move(usage);
        trim_external_memory = std::move(trim);
    }
    u64 MemoryBytes() const;
    const MemoryBudget::Statistics& MemoryStatistics() const { return memory_budget.Stats(); }
    ResourceCache::UploadStatistics BufferStatistics(bool input = false) const;
    struct AsyncStatistics {
        u64 commands{}, batches{}, hazard_waits{}, capacity_waits{};
        u64 gpu_draw_batches{}, gpu_dispatches{}, gpu_index_expansions{};
        u32 peak_in_flight{};
    };
    const AsyncStatistics& AsyncStats() const { return async_statistics; }
    void SetMemoryAccess(AcquireMemory acquire) {
        acquire_memory = std::move(acquire);
    }
    struct DrawRequest {
        Shader::ShaderParams vertex, fragment;
        Shader::RuntimeInfo vertex_runtime, fragment_runtime;
        std::optional<Shader::ShaderParams> geometry;
        Shader::RuntimeInfo geometry_runtime{};
        std::optional<Shader::ShaderParams> hull, domain;
        Shader::RuntimeInfo hull_runtime{}, domain_runtime{};
        std::vector<u32> converted_indices;
        std::optional<GpuCommands::Arguments> indirect;
        std::optional<GpuCommands::Expansion> expansion;
        u32 source_index_count{};
        bool rectangles{};
        std::vector<VideoCore::ImageInfo> colors;
        std::vector<AmdGpu::ColorBuffer> color_clear_buffers;
        std::vector<u32> color_slots;
        std::vector<u32> color_base_slice;
        std::optional<VideoCore::ImageInfo> depth_image;
        VAddr depth_read_address{};
        VAddr stencil_read_address{};
        u32 depth_base_layer{};
        D3D12_DSV_FLAGS dsv_flags{D3D12_DSV_FLAG_NONE};
        u32 stencil_ref{};
        float depth_clear{1.f};
        u32 stencil_clear{};
        bool clear_depth{};
        bool clear_stencil{};
        bool depth_gated{};
        bool depth_bounds{};
        float bounds_min{0.f}, bounds_max{1.f};
        Shader::PushData push{};
        std::array<ResourceBindings::StreamOutSlot, 4> strmout{};
        std::array<float, 24> clip_planes{};
        D3D12_BLEND_DESC blend{};
        D3D12_RASTERIZER_DESC rasterizer{};
        D3D12_DEPTH_STENCIL_DESC depth{};
        std::array<D3D12_VIEWPORT, AmdGpu::NUM_VIEWPORTS> viewports{};
        std::array<D3D12_RECT, AmdGpu::NUM_VIEWPORTS> scissors{};
        u32 viewport_count{1};
        D3D_PRIMITIVE_TOPOLOGY topology{};
        D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type{};
        std::array<float, 4> blend_factor{};
        u32 count{}, instances{1}, first_vertex{}, first_instance{};
        VAddr index_address{};
        VAddr border_colors{};
        DXGI_FORMAT index_format{DXGI_FORMAT_R16_UINT};
        AmdGpu::IndexSwapMode index_swap{AmdGpu::IndexSwapMode::None};
        D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut{D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED};
        s32 base_vertex{};
        bool flip_y{};
        bool flip_x{};
        bool provoking_last{};
        bool point_fill{};
        bool point_keep_front{};
        bool line_expand{};
        bool line_keep_front{};
        float line_width{1.f};
        u16 line_pattern{};
        u16 line_pattern_repeat{};
        bool line_pattern_msb{};
        float line_pattern_scale{1.f};
        std::array<f32, AmdGpu::NUM_VIEWPORTS * 2> line_viewport_dims{};
        bool sample_shading{};
        bool alpha_to_coverage{};
        u32 samples{1};
        std::array<u32, 4> sample_masks{UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX};
        std::vector<D3D12_SAMPLE_POSITION> sample_positions;
    };
    void Initialize(ResourceCache::Read read, ResourceCache::Write write,
                    ResourceCache::Resolve resolve = {}, ResourceCache::Overlaps overlaps = {});
    void Dispatch(Shader::ShaderParams params, Shader::RuntimeInfo runtime,
                  std::array<u32, 3> groups, VAddr border_colors = 0, VAddr indirect_address = 0);
    void Draw(const DrawRequest& request);
    void DrawImpl(const DrawRequest& request);
    static u64 GsInvocations(D3D_PRIMITIVE_TOPOLOGY topology, u64 count);
    void Resolve(const VideoCore::ImageInfo& source, const VideoCore::ImageInfo& destination,
                 u32 source_layer = 0, u32 destination_layer = 0, u32 layers = 0);
    void MaterializeColorClear(const AmdGpu::ColorBuffer& buffer, const VideoCore::ImageInfo& info);
    std::shared_ptr<VideoCore::ResidentFrame> CapturePresentation(const VideoCore::ImageInfo& info);
    u64 Flush();
    ResourceCache& Resources() {
        return resources;
    }
    u32 StrmoutFilledSize() const {
        return last_context ? last_context->strmout_filled : strmout_filled;
    }
    std::span<u8, 65536> Gds();
    const TransferPool& Transfers() const {
        return *transfer_pool;
    }
    const PersistentCache::Statistics& ShaderDiskCacheStatistics() const {
        return shaders.PersistentStatistics();
    }
    const PersistentCache::Statistics& PipelineCacheStatistics() const {
        return persistent_pipelines.GetStatistics();
    }
    D3D12DeviceContext& Device() {
        return device;
    }

    using ImageWatches = std::function<void(std::span<const ImageCache::Watch>)>;
    void SetImageResidency(ImageWatches callback);
    std::shared_ptr<void> AcquireCpuAccess(VAddr address, u64 size, bool write);
    std::shared_ptr<void> AcquireMemoryMappingAccess();
    struct GpuTimingStatistics {
        u64 submissions{}, ticks{}, frequency{};
    };
    const GpuTimingStatistics& GpuTimings();
    const ImageCache::Statistics& ImageResidencyStatistics() const {
        residency_statistics = images.ResidencyStatistics();
        for (const auto& context : contexts) {
            const auto& next = context->images.ResidencyStatistics();
            residency_statistics.uploads += next.uploads;
            residency_statistics.resident_hits += next.resident_hits;
            residency_statistics.downloads += next.downloads;
            residency_statistics.cpu_invalidations += next.cpu_invalidations;
        }
        return residency_statistics;
    }

private:
    struct RecordingGuard {
        bool& failed;
        bool completed{};
        ~RecordingGuard() {
            failed |= !completed;
        }
    };
    void MaterializeColorClearImpl(const AmdGpu::ColorBuffer& buffer,
                                   const VideoCore::ImageInfo& info);
    bool failed{}, trimming_memory{};
    MemoryBudget memory_budget;
    std::function<u64()> external_memory;
    std::function<void()> trim_external_memory;
    u64 LocalMemoryBytes() const;
    void TrimLocalMemory(u64 target, bool writeback);
    struct Pipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> state;
    };
    Pipeline& GetPipeline(std::vector<u8> key, ID3DBlob* root,
                          const std::function<HRESULT(ID3D12RootSignature*, ID3D12PipelineState**,
                                                      D3D12_CACHED_PIPELINE_STATE)>& create);
    std::map<std::vector<u8>, Pipeline> pipelines;
    size_t pipeline_bytes{};
    PersistentCache persistent_pipelines;
    struct CpuImageAccess;
    struct ImageMappingAccess;
    ImageWatches image_watches;
    std::atomic<u32> image_mapping_count{};
    std::atomic<bool> image_mapping_ready{};
    std::recursive_mutex image_mapping_mutex;
    bool synchronizing_images{};
    u64 image_mapping_generation{};
    void BeginImages();
    void SynchronizeBuffers(std::span<const ResourceCache::Range> ranges, bool invalidate, bool all = false);
    std::vector<ImageCache::Watch> ResourceWatches() const;
    bool buffer_commands_open{};
    void UpdateImageWatches();
    void SynchronizeImages(std::span<const ResourceCache::Range> ranges, bool invalidate,
                           bool all = false);
    void CompleteImages(std::span<const ResourceCache::Range> cpu_ranges = {});
    u64 SubmitCommands(u64 fence = 0);
    explicit CommandRuntime(D3D12DeviceContext& shared) : device{shared} {}
    CommandRuntime& NextContext(std::span<const ResourceCache::Range> writes = {},
                                std::span<const ResourceCache::Range> reads = {});
    void DrainContexts(std::span<const ResourceCache::Range> ranges = {}, bool all = true);
    void CompleteContext();
    void PrepareContext(std::span<CompiledShader*> stages);
    void FinishRecording(std::shared_ptr<void> lease, ResourceCache::Buffer* gds_buffer,
                         std::vector<ResourceCache::Range> cpu_ranges);
    CommandRuntime* coordinator{};
    CommandRuntime* last_context{};
    std::vector<std::unique_ptr<CommandRuntime>> contexts;
    std::vector<ResourceCache::Range> pending_ranges, write_ranges;
    std::function<void()> completion;
    std::shared_ptr<void> pending_lease;
    bool ready{}, completing{}, recording_context{};
    u64 pending_fence{}, context_order{}, next_order{};
    AsyncStatistics async_statistics{};
    mutable ImageCache::Statistics residency_statistics{};
    void BeginGpuTiming();
    void EndGpuTiming();
    void ReadGpuTiming();
    GpuTimingStatistics gpu_timing{};
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> timestamp_heap;
    Microsoft::WRL::ComPtr<ID3D12Resource> timestamp_readback;
    u64 timestamp_fence{};
    bool timestamp_recording{}, timestamp_recorded{};
    D3D12DeviceContext owned_device;
    D3D12DeviceContext& device{owned_device};
    std::shared_ptr<TransferPool> transfer_pool;
    std::vector<D3D12Resource> input_resources;
    D3D12DescriptorArena resource_descriptors, sampler_descriptors;
    D3D12DescriptorArena color_descriptors, depth_descriptors;
    ShaderCache shaders;
    ResourceCache resources;
    ResourceCache inputs;
    GpuCommands gpu_commands;
    ImageCache images;
    ResourceCache::Read read;
    ResourceCache::Write write;
    AcquireMemory acquire_memory;
    std::array<u8, 65536> gds{};
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
    std::recursive_mutex mutex;
    u32 strmout_filled{};
};
} // namespace D3D12
