#pragma once

#include <array>
#include <exception>

#include "d3d12_device_context.h"
#include "d3d12_resource_allocator.h"
#include "video_core/renderer_d3d12/shader_cache.h"
#include "video_core/renderer_d3d12/resource_cache.h"

namespace D3D12 {
class GpuDrawFallback final : public std::exception {};
class GpuCommands {
public:
    struct Arguments {
        VAddr address{}, count_address{};
        u32 stride{}, count{};
        bool indexed{};
    };
    struct Expansion {
        u32 mode{}, input_count{}, output_count{};
        bool provoking_last{};
    };
    void Initialize(D3D12DeviceContext& device, ShaderCache& shaders);
    void Prepare(u32 commands, u32 indices);
    u64 MemoryBytes() const { return allocator.Stats().live_bytes; }
    void TrimMemory() {
        arguments.Reset();
        indices.Reset();
        argument_capacity = index_capacity = 0;
    }
    void RecordArguments(ID3D12GraphicsCommandList* list, const Arguments& description,
                         const ResourceCache::View& arguments, const ResourceCache::View& count);
    void RecordDispatch(ID3D12GraphicsCommandList* list, const ResourceCache::View& input);
    void ExecuteDispatch(ID3D12GraphicsCommandList* list, ID3D12RootSignature* compute);
    void RecordIndices(ID3D12GraphicsCommandList* list, const Expansion& description,
                       const ResourceCache::View& input, u32 index_bytes, u32 swap,
                       u32 readable_count = UINT32_MAX, u32 read_count = UINT32_MAX);
    ID3D12Resource* Indices() const { return indices.Get(); }
    void Execute(ID3D12GraphicsCommandList* list, ID3D12RootSignature* graphics, bool indexed,
                 u32 count);

private:
    void Transition(ID3D12GraphicsCommandList* list, D3D12Resource& resource,
                    D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES next);
    void Bind(ID3D12GraphicsCommandList* list, const std::array<u32, 12>& constants,
              const ResourceCache::View& input, const ResourceCache::View& count);
    D3D12DeviceContext* device{};
    D3D12ResourceAllocator allocator;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> signature_root;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> draw, indexed_draw, dispatch;
    D3D12Resource arguments, indices;
    u64 argument_capacity{}, index_capacity{};
    D3D12_RESOURCE_STATES argument_state{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES index_state{D3D12_RESOURCE_STATE_COMMON};
};
} // namespace D3D12
