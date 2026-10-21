#pragma once

#include "d3d12_resource_allocator.h"
#include "video_core/renderer_d3d12/shader_cache.h"

namespace D3D12 {
class FsrPass {
public:
    static bool Supports(u32 width, u32 height);
    ID3D12Resource* Record(ID3D12Device* device, ID3D12GraphicsCommandList* commands,
                           ID3D12Resource* source, u32 width, u32 height, bool sharpen,
                           float attenuation);
    void ResetImages();
    void TrimMemory() {
        ResetImages();
        shaders.TrimMemory(0);
    }
    u64 MemoryBytes() const { return allocator.Stats().live_bytes + shaders.MemoryBytes(); }

private:
    void Initialize(ID3D12Device* device);
    ShaderCache shaders;
    D3D12ResourceAllocator allocator;
    D3D12Resource intermediate, output;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> easu, rcas;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors;
    u32 width{}, height{};
    D3D12_RESOURCE_STATES intermediate_state{D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    D3D12_RESOURCE_STATES output_state{D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
};
} // namespace D3D12
