// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>
#include <d3d12.h>
#include <dxcapi.h>
#include "common/types.h"
#include "d3d12_device_context.h"
#include "d3d12_pipeline_cache.h"

namespace D3D12 {
struct MsaaFormat {
    DXGI_FORMAT resource, transfer;
    bool integer{};
};
MsaaFormat GetMsaaFormat(DXGI_FORMAT format, bool depth);
void SetSamplePositions(ID3D12GraphicsCommandList* commands, u32 samples,
                        std::span<const D3D12_SAMPLE_POSITION> positions);

class MsaaTransfer {
public:
    struct Resources {
        Microsoft::WRL::ComPtr<ID3D12Resource> planes;
        Microsoft::WRL::ComPtr<ID3D12Resource> stencil_planes;
        D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
        D3D12_RESOURCE_STATES stencil_state{D3D12_RESOURCE_STATE_COMMON};
        D3D12DescriptorArena descriptors, targets, depth_targets;
        D3D12DescriptorArena stencil_descriptors;
    };
    void Initialize(ID3D12Device* device);
    u64 MemoryBytes() const {
        return pipelines.size() * 65536 + vertex.capacity() + mask_shader.capacity();
    }
    void TrimMemory() { pipelines.clear(); }
    const std::vector<u8>& MaskShader() const {
        return mask_shader;
    }
    static void Barrier(Resources& resources, ID3D12GraphicsCommandList* commands,
                        D3D12_RESOURCE_STATES state);
    static void StencilBarrier(Resources& resources, ID3D12GraphicsCommandList* commands,
                               D3D12_RESOURCE_STATES state);
    void Record(ID3D12GraphicsCommandList* commands, ID3D12Resource* image, Resources& resources,
                DXGI_FORMAT format, bool depth, bool upload);
    void RecordStencil(ID3D12GraphicsCommandList* commands, ID3D12Resource* image,
                       Resources& resources, DXGI_FORMAT dsv_format, bool upload);

private:
    struct Pipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> state;
    };
    Pipeline& GetPipeline(DXGI_FORMAT format, u32 samples, bool integer, bool depth, bool upload,
                          bool stencil, DXGI_FORMAT dsv_format);
    std::vector<u8> Compile(std::string_view source, const wchar_t* target);
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<IDxcCompiler3> compiler;
    std::vector<u8> vertex, mask_shader;
    std::map<std::tuple<DXGI_FORMAT, u32, bool, bool, bool, bool, DXGI_FORMAT>, Pipeline> pipelines;
};
} // namespace D3D12
