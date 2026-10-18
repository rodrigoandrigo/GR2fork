#pragma once

#include "d3d12_pipeline_cache.h"
#include "video_core/renderer_d3d12/image_cache.h"
#include "video_core/renderer_d3d12/shader_cache.h"

namespace D3D12 {
class ResourceBindings {
public:
    struct StreamOutSlot {
        VAddr address{};
        u64 size{};
    };
    ResourceBindings(D3D12DeviceContext& device, ResourceCache& buffers, ImageCache& images,
                     std::span<CompiledShader*> shaders,
                     std::span<const StreamOutSlot> strmout_slots = {});
    void Upload(ID3D12GraphicsCommandList* commands, Shader::PushData& push,
                std::span<const float, 24> clip_planes, VAddr border_colors,
                const ResourceCache::Read& read, std::span<const u8, 65536> gds,
                std::span<const u8> shared_memory = {});
    void CommitGds(std::span<u8, 65536> gds);
    const std::vector<D3D12_DESCRIPTOR_RANGE>& ResourceRanges() const {
        return resource_ranges;
    }
    const std::vector<D3D12_DESCRIPTOR_RANGE>& SamplerRanges() const {
        return sampler_ranges;
    }
    void Bind(ID3D12GraphicsCommandList* commands, bool graphics, u32 resource_slot,
              u32 sampler_slot);
    void ValidateInputRange(VAddr address, u64 size) const;
    void ValidateTargetRange(VAddr address, u64 size) const;
    std::vector<ResourceCache::Range> AccessRanges(VAddr border_colors) const;

private:
    bool Aliases(VAddr first, u64 first_size, VAddr second, u64 second_size) const;
    struct Entry {
        CompiledShader* shader;
        ShaderBinding binding;
        u32 index, element, heap_index;
    };
    D3D12DeviceContext& device;
    ResourceCache& buffers;
    ImageCache& images;
    std::vector<Entry> entries;
    std::vector<ResourceCache::Range> guest_ranges;
    std::vector<D3D12_DESCRIPTOR_RANGE> resource_ranges, sampler_ranges;
    D3D12DescriptorArena resources, samplers;
    ResourceCache::Buffer* gds_buffer{};
};
} // namespace D3D12
