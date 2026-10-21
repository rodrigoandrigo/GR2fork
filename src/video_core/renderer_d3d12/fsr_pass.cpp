#include "video_core/renderer_d3d12/fsr_pass.h"

#include "video_core/host_shaders/fsr_easu_fp32_comp.h"
#include "video_core/host_shaders/fsr_rcas_fp32_comp.h"
#include <algorithm>
#include <cmath>
#include <spirv_hlsl.hpp>
#include <winrt/base.h>

#define A_CPU
#include "video_core/host_shaders/fsr/ffx_a.h"
#include "video_core/host_shaders/fsr/ffx_fsr1.h"

namespace D3D12 {
namespace {
constexpr u64 ImageBudget = 128 * 1024 * 1024;
std::string Translate(std::span<const u32> words) {
    spirv_cross::CompilerHLSL cross{words.data(), words.size()};
    auto options = cross.get_hlsl_options();
    options.shader_model = 60;
    cross.set_hlsl_options(options);
    for (u32 binding = 0; binding < 3; ++binding) {
        spirv_cross::HLSLResourceBinding remap{};
        remap.stage = spv::ExecutionModelGLCompute;
        remap.binding = binding;
        remap.srv = remap.uav = remap.sampler = {0, 0};
        cross.add_hlsl_resource_binding(remap);
    }
    spirv_cross::HLSLResourceBinding push{};
    push.stage = spv::ExecutionModelGLCompute;
    push.desc_set = spirv_cross::ResourceBindingPushConstantDescriptorSet;
    push.binding = spirv_cross::ResourceBindingPushConstantBinding;
    push.cbv = {0, 0};
    cross.add_hlsl_resource_binding(push);
    return cross.compile();
}
void Transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after)
        return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    commands->ResourceBarrier(1, &barrier);
}
} // namespace
bool FsrPass::Supports(u32 width, u32 height) {
    return width && height && width <= 8192 && height <= 8192 &&
           u64((width + 255) & ~255u) * ((height + 63) & ~63u) * 12 +
                   2 * D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT <=
               ImageBudget;
}
void FsrPass::ResetImages() {
    intermediate.Reset();
    output.Reset();
    width = height = 0;
}
void FsrPass::Initialize(ID3D12Device* device) {
    if (root)
        return;
    shaders.Initialize(device);
    allocator.Initialize(device, ImageBudget);
    const D3D12_DESCRIPTOR_RANGE ranges[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                          {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {2, ranges};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants = {0, 0, 20};
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxAnisotropy = 1;
    const D3D12_ROOT_SIGNATURE_DESC description{2, parameters, 1, &sampler};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    winrt::check_hresult(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
                                                     &serialized, &errors));
    Microsoft::WRL::ComPtr<ID3D12RootSignature> new_root;
    winrt::check_hresult(device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&new_root)));
    const auto compile = [&](std::span<const u32> words, auto& pipeline) {
        const auto binary = shaders.CompileHlslRaw(Translate(words), L"cs_6_0", {});
        D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
        state.pRootSignature = new_root.Get();
        state.CS = {binary->dxil.data(), binary->dxil.size()};
        winrt::check_hresult(device->CreateComputePipelineState(&state, IID_PPV_ARGS(&pipeline)));
    };
    compile(FSR_EASU_FP32_COMP, easu);
    compile(FSR_RCAS_FP32_COMP, rcas);
    const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                          D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    winrt::check_hresult(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&descriptors)));
    root = std::move(new_root);
}
ID3D12Resource* FsrPass::Record(ID3D12Device* device, ID3D12GraphicsCommandList* commands,
                                ID3D12Resource* source, u32 new_width, u32 new_height, bool sharpen,
                                float attenuation) {
    if (!Supports(new_width, new_height) || !std::isfinite(attenuation))
        throw std::runtime_error("Invalid native FSR output");
    Initialize(device);
    if (width != new_width || height != new_height) {
        ResetImages();
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = new_width;
        description.Height = new_height;
        description.DepthOrArraySize = description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        winrt::check_hresult(allocator.CreateTexture2D(
            description, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, intermediate));
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        winrt::check_hresult(
            allocator.CreateTexture2D(description, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, output));
        width = new_width;
        height = new_height;
        intermediate_state = output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    const auto stride =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto bind = [&](u32 pair, ID3D12Resource* input, ID3D12Resource* destination) {
        auto cpu = descriptors->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += pair * 2 * stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = input->GetDesc().Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(input, &srv, cpu);
        cpu.ptr += stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = destination->GetDesc().Format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(destination, nullptr, &uav, cpu);
    };
    bind(0, source, sharpen ? intermediate.Get() : output.Get());
    if (sharpen)
        bind(1, intermediate.Get(), output.Get());
    std::array<u32, 20> constants{};
    const auto input = source->GetDesc();
    FsrEasuCon(constants.data(), constants.data() + 4, constants.data() + 8, constants.data() + 12,
               float(input.Width), float(input.Height), float(input.Width), float(input.Height),
               float(width), float(height));
    Transition(commands, source, D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(commands, intermediate.Get(), intermediate_state,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(commands, output.Get(), output_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commands->SetComputeRootSignature(root.Get());
    auto* heap = descriptors.Get();
    commands->SetDescriptorHeaps(1, &heap);
    const auto dispatch = [&](ID3D12PipelineState* pipeline, u32 pair) {
        auto gpu = descriptors->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += pair * 2 * stride;
        commands->SetPipelineState(pipeline);
        commands->SetComputeRootDescriptorTable(0, gpu);
        commands->SetComputeRoot32BitConstants(1, constants.size(), constants.data(), 0);
        commands->Dispatch((width + 15) / 16, (height + 15) / 16, 1);
    };
    dispatch(easu.Get(), 0);
    intermediate_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (sharpen) {
        Transition(commands, intermediate.Get(), intermediate_state,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        intermediate_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        constants = {};
        FsrRcasCon(constants.data(), std::clamp(attenuation, 0.f, 2.f));
        dispatch(rcas.Get(), 1);
    }
    Transition(commands, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(commands, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    output_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    return output.Get();
}
} // namespace D3D12
