// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_d3d12/msaa_transfer.h"

#include <stdexcept>
#include <fmt/format.h>
#include <winrt/base.h>

namespace D3D12 {
MsaaFormat GetMsaaFormat(DXGI_FORMAT format, bool depth) {
    if (depth) {
        if (format == DXGI_FORMAT_D16_UNORM)
            return {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM};
        if (format == DXGI_FORMAT_D32_FLOAT)
            return {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT};
        if (format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
            return {DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT};
        throw std::runtime_error("Unsupported D3D12 multisample depth format");
    }
    switch (format) {
#define MSAA_FAMILY(prefix)                                                                        \
    case DXGI_FORMAT_##prefix##_TYPELESS:                                                          \
    case DXGI_FORMAT_##prefix##_FLOAT:                                                             \
    case DXGI_FORMAT_##prefix##_UINT:                                                              \
    case DXGI_FORMAT_##prefix##_SINT:                                                              \
        return {DXGI_FORMAT_##prefix##_TYPELESS, DXGI_FORMAT_##prefix##_UINT, true}
        MSAA_FAMILY(R32);
        MSAA_FAMILY(R32G32);
        MSAA_FAMILY(R32G32B32A32);
#undef MSAA_FAMILY
#define MSAA_FAMILY(prefix)                                                                        \
    case DXGI_FORMAT_##prefix##_TYPELESS:                                                          \
    case DXGI_FORMAT_##prefix##_UNORM:                                                             \
    case DXGI_FORMAT_##prefix##_SNORM:                                                             \
    case DXGI_FORMAT_##prefix##_UINT:                                                              \
    case DXGI_FORMAT_##prefix##_SINT:                                                              \
        return {DXGI_FORMAT_##prefix##_TYPELESS, DXGI_FORMAT_##prefix##_UINT, true}
        MSAA_FAMILY(R8);
        MSAA_FAMILY(R8G8);
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        MSAA_FAMILY(R8G8B8A8);
    case DXGI_FORMAT_R16_FLOAT:
        MSAA_FAMILY(R16);
    case DXGI_FORMAT_R16G16_FLOAT:
        MSAA_FAMILY(R16G16);
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        MSAA_FAMILY(R16G16B16A16);
#undef MSAA_FAMILY
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
        return {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UINT, true};
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return {DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM};
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return {format, format};
    default:
        throw std::runtime_error("Unsupported D3D12 multisample color format");
    }
}
void SetSamplePositions(ID3D12GraphicsCommandList* commands, u32 samples,
                        std::span<const D3D12_SAMPLE_POSITION> positions) {
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList1> extended;
    if (positions.empty()) {
        if (SUCCEEDED(commands->QueryInterface(IID_PPV_ARGS(&extended))))
            extended->SetSamplePositions(0, 0, nullptr);
        return;
    }
    if ((positions.size() != samples && positions.size() != samples * 4) || positions.size() > 16)
        throw std::runtime_error("Invalid D3D12 sample position pattern");
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    winrt::check_hresult(commands->GetDevice(IID_PPV_ARGS(&device)));
    D3D12_FEATURE_DATA_D3D12_OPTIONS2 options{};
    winrt::check_hresult(
        device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options, sizeof(options)));
    const u32 pixels = positions.size() / samples;
    if (options.ProgrammableSamplePositionsTier ==
            D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED ||
        (pixels == 4 &&
         options.ProgrammableSamplePositionsTier < D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_2))
        throw std::runtime_error("Guest sample positions exceed D3D12 device capabilities");
    for (const auto& position : positions)
        if (position.X < -8 || position.X > 7 || position.Y < -8 || position.Y > 7)
            throw std::runtime_error("Guest sample position exceeds D3D12 coordinate bounds");
    winrt::check_hresult(commands->QueryInterface(IID_PPV_ARGS(&extended)));
    extended->SetSamplePositions(samples, pixels,
                                 const_cast<D3D12_SAMPLE_POSITION*>(positions.data()));
}
void MsaaTransfer::Initialize(ID3D12Device* native_device) {
    device = native_device;
    winrt::check_hresult(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));
    vertex = Compile(R"(
float4 main(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
})",
                     L"vs_6_6");
    mask_shader = Compile(R"(
cbuffer State : register(b3) { uint4 masks; };
uint main(float4 position : SV_Position) : SV_Coverage {
    uint2 pixel = uint2(position.xy);
    return masks[(pixel.x & 1) | ((pixel.y & 1) << 1)];
})",
                          L"ps_6_6");
}
std::vector<u8> MsaaTransfer::Compile(std::string_view source, const wchar_t* target) {
    const wchar_t* arguments[]{L"-E", L"main", L"-T", target, L"-HV", L"2021", L"-O3", L"-Gis"};
    const DxcBuffer input{source.data(), source.size(), DXC_CP_UTF8};
    Microsoft::WRL::ComPtr<IDxcResult> result;
    winrt::check_hresult(
        compiler->Compile(&input, arguments, std::size(arguments), nullptr, IID_PPV_ARGS(&result)));
    HRESULT status;
    winrt::check_hresult(result->GetStatus(&status));
    if (FAILED(status)) {
        Microsoft::WRL::ComPtr<IDxcBlobUtf8> errors;
        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        throw std::runtime_error(errors ? errors->GetStringPointer()
                                        : "MSAA transfer shader compilation failed");
    }
    Microsoft::WRL::ComPtr<IDxcBlob> binary;
    winrt::check_hresult(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&binary), nullptr));
    const auto* bytes = static_cast<const u8*>(binary->GetBufferPointer());
    return {bytes, bytes + binary->GetBufferSize()};
}
MsaaTransfer::Pipeline& MsaaTransfer::GetPipeline(DXGI_FORMAT format, u32 samples, bool integer,
                                                  bool depth, bool upload, bool stencil,
                                                  DXGI_FORMAT dsv_format) {
    const auto key = std::tuple{format, samples, integer, depth, upload, stencil, dsv_format};
    if (const auto found = pipelines.find(key); found != pipelines.end())
        return found->second;
    if (pipelines.size() >= 128)
        throw std::runtime_error("MSAA transfer pipeline budget exhausted");
    const std::string type = (integer || stencil) ? "uint4" : "float4";
    std::string source = fmt::format(
        "Texture2DArray<{}> planes : register(t0);\nTexture2DMSArray<{}> image : register(t1);\n",
        type, type);
    source += "cbuffer State : register(b0) { uint layer; uint samples; uint selectedSample; uint "
              "unused; };\n";
    if (stencil && upload)
        source += "uint main(float4 p : SV_Position, uint sample : SV_SampleIndex) : "
                  "SV_StencilRef { return planes.Load(int4(int2(p.xy), layer * samples + sample, "
                  "0)).x; }";
    else if (stencil)
        source +=
            "uint main(float4 p : SV_Position) : SV_Target0 { return image.Load(int3(int2(p.xy), "
            "layer), selectedSample).y; }";
    else if (upload)
        source += fmt::format(
            "{} main(float4 p : SV_Position, uint sample : SV_SampleIndex) : {} {{ return "
            "planes.Load(int4(int2(p.xy), layer * samples + sample, 0)){}; }}",
            depth ? "float" : type, depth ? "SV_Depth" : "SV_Target0", depth ? ".x" : "");
    else
        source += fmt::format("{} main(float4 p : SV_Position) : SV_Target0 {{ return "
                              "image.Load(int3(int2(p.xy), layer), selectedSample); }}",
                              type);
    const auto pixel = Compile(source, L"ps_6_6");
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 4};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {1, &range};
    D3D12_ROOT_SIGNATURE_DESC root_description{2, parameters};
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
    winrt::check_hresult(D3D12SerializeRootSignature(&root_description,
                                                     D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
    Pipeline pipeline;
    winrt::check_hresult(device->CreateRootSignature(
        0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&pipeline.root)));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = pipeline.root.Get();
    desc.VS = {vertex.data(), vertex.size()};
    desc.PS = {pixel.data(), pixel.size()};
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.RasterizerState.MultisampleEnable = upload;
    auto& blend = desc.BlendState.RenderTarget[0];
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.DepthStencilState.DepthEnable = depth && upload;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.StencilEnable = stencil && upload;
    desc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    desc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    if (stencil && upload) {
        desc.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_REPLACE,
                                            D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS};
        desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;
    } else {
        desc.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                            D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
        desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;
    }
    desc.SampleMask = UINT_MAX;
    desc.SampleDesc.Count = upload ? samples : 1;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    if ((depth || stencil) && upload)
        desc.DSVFormat = dsv_format;
    else {
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = format;
    }
    winrt::check_hresult(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline.state)));
    return pipelines.emplace(key, std::move(pipeline)).first->second;
}
void MsaaTransfer::Barrier(Resources& resources, ID3D12GraphicsCommandList* commands,
                           D3D12_RESOURCE_STATES state) {
    if (resources.state == state)
        return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resources.planes.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          resources.state, state};
    commands->ResourceBarrier(1, &barrier);
    resources.state = state;
}
void MsaaTransfer::StencilBarrier(Resources& resources, ID3D12GraphicsCommandList* commands,
                                  D3D12_RESOURCE_STATES state) {
    if (resources.stencil_state == state)
        return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resources.stencil_planes.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          resources.stencil_state, state};
    commands->ResourceBarrier(1, &barrier);
    resources.stencil_state = state;
}
void MsaaTransfer::Record(ID3D12GraphicsCommandList* commands, ID3D12Resource* image,
                          Resources& resources, DXGI_FORMAT format, bool depth, bool upload) {
    const auto desc = image->GetDesc();
    const auto formats = GetMsaaFormat(format, depth);
    const bool stencil_res = depth && (format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
                                       format == DXGI_FORMAT_D24_UNORM_S8_UINT);
    const DXGI_FORMAT upload_dsv =
        stencil_res ? format
                    : (formats.transfer == DXGI_FORMAT_R16_UNORM ? DXGI_FORMAT_D16_UNORM
                                                                 : DXGI_FORMAT_D32_FLOAT);
    auto& pipeline = GetPipeline(formats.transfer, desc.SampleDesc.Count, formats.integer, depth,
                                 upload, false, upload_dsv);
    Barrier(resources, commands,
            upload ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                   : D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = formats.transfer;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Texture2DArray = {0, 1, 0, u32(desc.DepthOrArraySize) * desc.SampleDesc.Count, 0, 0};
    device->CreateShaderResourceView(resources.planes.Get(), &srv, resources.descriptors.Cpu(0));
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
    srv.Texture2DMSArray = {0, desc.DepthOrArraySize};
    if (stencil_res) {
        srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    }
    device->CreateShaderResourceView(image, &srv, resources.descriptors.Cpu(1));
    commands->SetPipelineState(pipeline.state.Get());
    commands->SetGraphicsRootSignature(pipeline.root.Get());
    ID3D12DescriptorHeap* heaps[]{resources.descriptors.Heap()};
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetGraphicsRootDescriptorTable(1, resources.descriptors.Gpu(0));
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport{0, 0, float(desc.Width), float(desc.Height), 0, 1};
    const D3D12_RECT scissor{0, 0, LONG(desc.Width), LONG(desc.Height)};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    for (u32 layer = 0; layer < desc.DepthOrArraySize; ++layer) {
        const u32 count = upload ? 1 : desc.SampleDesc.Count;
        for (u32 sample = 0; sample < count; ++sample) {
            const u32 index = upload ? layer : layer * desc.SampleDesc.Count + sample;
            const std::array<u32, 4> constants{layer, desc.SampleDesc.Count, sample, 0};
            commands->SetGraphicsRoot32BitConstants(0, 4, constants.data(), 0);
            if (depth && upload) {
                D3D12_DEPTH_STENCIL_VIEW_DESC view{};
                view.Format = upload_dsv;
                view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
                view.Texture2DMSArray = {layer, 1};
                const auto target = resources.depth_targets.Cpu(index);
                device->CreateDepthStencilView(image, &view, target);
                commands->ClearDepthStencilView(target, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, nullptr);
                commands->OMSetRenderTargets(0, nullptr, FALSE, &target);
            } else {
                D3D12_RENDER_TARGET_VIEW_DESC view{};
                view.Format = formats.transfer;
                if (upload) {
                    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
                    view.Texture2DMSArray = {layer, 1};
                } else {
                    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                    view.Texture2DArray = {0, index, 1, 0};
                }
                const auto target = resources.targets.Cpu(index);
                device->CreateRenderTargetView(upload ? image : resources.planes.Get(), &view,
                                               target);
                commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
            }
            commands->DrawInstanced(3, 1, 0, 0);
        }
    }
}
void MsaaTransfer::RecordStencil(ID3D12GraphicsCommandList* commands, ID3D12Resource* image,
                                 Resources& resources, DXGI_FORMAT dsv_format, bool upload) {
    const auto desc = image->GetDesc();
    auto& pipeline = GetPipeline(DXGI_FORMAT_R8_UINT, desc.SampleDesc.Count, true, false, upload,
                                 true, upload ? dsv_format : DXGI_FORMAT_UNKNOWN);
    if (upload)
        StencilBarrier(resources, commands, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    else
        StencilBarrier(resources, commands, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R8_UINT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Texture2DArray = {0, 1, 0, u32(desc.DepthOrArraySize) * desc.SampleDesc.Count, 0, 0};
    device->CreateShaderResourceView(resources.stencil_planes.Get(), &srv,
                                     resources.stencil_descriptors.Cpu(0));
    srv.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
    srv.Texture2DMSArray = {0, desc.DepthOrArraySize};
    device->CreateShaderResourceView(image, &srv, resources.stencil_descriptors.Cpu(1));
    commands->SetPipelineState(pipeline.state.Get());
    commands->SetGraphicsRootSignature(pipeline.root.Get());
    ID3D12DescriptorHeap* heaps[]{resources.stencil_descriptors.Heap()};
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetGraphicsRootDescriptorTable(1, resources.stencil_descriptors.Gpu(0));
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport{0, 0, float(desc.Width), float(desc.Height), 0, 1};
    const D3D12_RECT scissor{0, 0, LONG(desc.Width), LONG(desc.Height)};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    for (u32 layer = 0; layer < desc.DepthOrArraySize; ++layer) {
        const u32 count = upload ? 1 : desc.SampleDesc.Count;
        for (u32 sample = 0; sample < count; ++sample) {
            const u32 index = upload ? layer : layer * desc.SampleDesc.Count + sample;
            const std::array<u32, 4> constants{layer, desc.SampleDesc.Count, sample, 0};
            commands->SetGraphicsRoot32BitConstants(0, 4, constants.data(), 0);
            if (upload) {
                D3D12_DEPTH_STENCIL_VIEW_DESC view{};
                view.Format = dsv_format;
                view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
                view.Texture2DMSArray = {layer, 1};
                const auto target = resources.depth_targets.Cpu(index);
                device->CreateDepthStencilView(image, &view, target);
                commands->ClearDepthStencilView(target, D3D12_CLEAR_FLAG_STENCIL, 1.f, 0, 0,
                                                nullptr);
                commands->OMSetRenderTargets(0, nullptr, FALSE, &target);
            } else {
                D3D12_RENDER_TARGET_VIEW_DESC view{};
                view.Format = DXGI_FORMAT_R8_UINT;
                view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                view.Texture2DArray = {0, index, 1, 0};
                const auto target = resources.targets.Cpu(index);
                device->CreateRenderTargetView(resources.stencil_planes.Get(), &view, target);
                commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
            }
            commands->DrawInstanced(3, 1, 0, 0);
        }
    }
}
} // namespace D3D12
