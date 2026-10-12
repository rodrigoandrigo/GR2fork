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
        std::vector<VideoCore::ImageInfo> colors;
        std::optional<VideoCore::ImageInfo> depth_image;
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
        DXGI_FORMAT index_format{DXGI_FORMAT_R16_UINT};
        D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut{D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED};
        s32 base_vertex{};
    };
    void Initialize(ResourceCache::Read read, ResourceCache::Write write);
    void Dispatch(Shader::ShaderParams params, Shader::RuntimeInfo runtime,
                  std::array<u32, 3> groups);
    void Draw(const DrawRequest& request);
    ResourceCache& Resources() {
        return resources;
    }

private:
    struct Pipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> state;
    };
    std::map<std::vector<u8>, Pipeline> pipelines;
    size_t pipeline_bytes{};
    D3D12DeviceContext device;
    ShaderCache shaders;
    ResourceCache resources;
    ImageCache images;
    ResourceCache::Read read;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
    std::mutex mutex;
};
} // namespace D3D12
