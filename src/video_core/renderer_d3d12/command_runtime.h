#pragma once

#include <array>
#include <mutex>
#include <optional>
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/amdgpu/regs_vertex.h"
#include "video_core/renderer_d3d12/image_cache.h"
#include "video_core/renderer_d3d12/resource_bindings.h"
#include "video_core/renderer_d3d12/resource_cache.h"
#include "video_core/renderer_d3d12/shader_cache.h"

namespace D3D12 {
class CommandRuntime {
public:
    using AcquireMemory =
        std::function<std::shared_ptr<void>(std::span<const ResourceCache::Range>)>;
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
        bool rectangles{};
        std::vector<VideoCore::ImageInfo> colors;
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
                  std::array<u32, 3> groups, VAddr border_colors = 0);
    void Draw(const DrawRequest& request);
    void DrawImpl(const DrawRequest& request);
    static u64 GsInvocations(D3D_PRIMITIVE_TOPOLOGY topology, u64 count);
    void Resolve(const VideoCore::ImageInfo& source, const VideoCore::ImageInfo& destination,
                 u32 source_layer = 0, u32 destination_layer = 0, u32 layers = 0);
    u64 Flush();
    ResourceCache& Resources() {
        return resources;
    }
    u32 StrmoutFilledSize() const {
        return strmout_filled;
    }
    std::span<u8, 65536> Gds() {
        return gds;
    }
    D3D12DeviceContext& Device() {
        return device;
    }

private:
    struct RecordingGuard {
        bool& failed;
        bool completed{};
        ~RecordingGuard() {
            failed |= !completed;
        }
    };
    bool failed{};
    struct Pipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> state;
    };
    Pipeline& GetPipeline(
        std::vector<u8> key, ID3DBlob* root,
        const std::function<HRESULT(ID3D12RootSignature*, ID3D12PipelineState**)>& create);
    std::map<std::vector<u8>, Pipeline> pipelines;
    size_t pipeline_bytes{};
    D3D12DeviceContext device;
    ShaderCache shaders;
    ResourceCache resources;
    ImageCache images;
    ResourceCache::Read read;
    AcquireMemory acquire_memory;
    std::array<u8, 65536> gds{};
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
    std::mutex mutex;
    u32 strmout_filled{};
};
} // namespace D3D12
