#pragma once

#include <array>
#include <mutex>
#include <optional>
#include "video_core/renderer_d3d12/image_cache.h"
#include "video_core/renderer_d3d12/resource_cache.h"
#include "video_core/renderer_d3d12/shader_cache.h"

namespace D3D12 {
class CommandRuntime {
public:
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
        std::optional<VideoCore::ImageInfo> depth_image;
        VAddr depth_read_address{};
        u32 depth_base_layer{};
        Shader::PushData push{};
        std::array<float, 24> clip_planes{};
        D3D12_BLEND_DESC blend{};
        D3D12_RASTERIZER_DESC rasterizer{};
        D3D12_DEPTH_STENCIL_DESC depth{};
        D3D12_VIEWPORT viewport{};
        D3D12_RECT scissor{};
        D3D_PRIMITIVE_TOPOLOGY topology{};
        D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type{};
        std::array<float, 4> blend_factor{};
        u32 count{}, instances{1}, first_vertex{}, first_instance{};
        VAddr index_address{};
        VAddr border_colors{};
        DXGI_FORMAT index_format{DXGI_FORMAT_R16_UINT};
        D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut{D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED};
        s32 base_vertex{};
        bool flip_y{};
        bool sample_shading{};
        u32 samples{1};
        std::array<u32, 4> sample_masks{UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX};
        std::vector<D3D12_SAMPLE_POSITION> sample_positions;
    };
    void Initialize(ResourceCache::Read read, ResourceCache::Write write,
                    ResourceCache::Resolve resolve = {});
    void Dispatch(Shader::ShaderParams params, Shader::RuntimeInfo runtime,
                  std::array<u32, 3> groups, VAddr border_colors = 0);
    void Draw(const DrawRequest& request);
    void Resolve(const VideoCore::ImageInfo& source, const VideoCore::ImageInfo& destination,
                 u32 source_layer = 0, u32 destination_layer = 0, u32 layers = 0);
    u64 Flush();
    ResourceCache& Resources() {
        return resources;
    }
    std::span<u8, 65536> Gds() {
        return gds;
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
    std::array<u8, 65536> gds{};
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
    std::mutex mutex;
};
} // namespace D3D12
