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
        D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
        D3D12DescriptorArena descriptors, targets, depth_targets;
    };
    void Initialize(ID3D12Device* device);
    const std::vector<u8>& MaskShader() const {
        return mask_shader;
    }
    static void Barrier(Resources& resources, ID3D12GraphicsCommandList* commands,
                        D3D12_RESOURCE_STATES state);
    void Record(ID3D12GraphicsCommandList* commands, ID3D12Resource* image, Resources& resources,
                DXGI_FORMAT format, bool depth, bool upload);

private:
    struct Pipeline {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> state;
    };
    Pipeline& GetPipeline(DXGI_FORMAT format, u32 samples, bool integer, bool depth, bool upload);
    std::vector<u8> Compile(std::string_view source, const wchar_t* target);
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<IDxcCompiler3> compiler;
    std::vector<u8> vertex, mask_shader;
    std::map<std::tuple<DXGI_FORMAT, u32, bool, bool, bool>, Pipeline> pipelines;
};
} // namespace D3D12
