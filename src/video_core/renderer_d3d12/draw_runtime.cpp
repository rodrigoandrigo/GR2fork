#include "video_core/renderer_d3d12/command_runtime.h"
#include "video_core/renderer_d3d12/image_buffer_alias.h"

#include <bit>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <winrt/base.h>
#include "common/logging/log.h"
#include "d3d12_pipeline_cache.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/frontend/tessellation.h"
#include "video_core/renderer_d3d12/cmask_address.h"
#include "video_core/renderer_d3d12/logic_op_format.h"
#include "video_core/renderer_d3d12/primitive_conversion.h"
#include "video_core/renderer_d3d12/profile.h"
#include "video_core/renderer_d3d12/resource_bindings.h"
#include "video_core/renderer_d3d12/temporal_jitter.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"

namespace D3D12 {
namespace {
Profile profile;
constexpr char StencilShader[] = R"(
RWByteAddressBuffer Values : register(u0,space101);
cbuffer StencilConfig : register(b0,space101) { uint pitch, operation, refs, write_mask; };
uint Apply(uint old) {
    uint operand = refs >> 8;
    switch (operation) {
    case 0: return old;
    case 1: return 0;
    case 2: return 255;
    case 3: return refs & 255;
    case 4: return operand;
    case 5: return min(old + operand,255u);
    case 6: return old > operand ? old - operand : 0;
    case 7: return ~old;
    case 8: return old + operand;
    case 9: return old - operand;
    case 10: return old & operand;
    case 11: return old | operand;
    case 12: return old ^ operand;
    case 13: return ~(old & operand);
    case 14: return ~(old | operand);
    default: return ~(old ^ operand);
    }
}
[earlydepthstencil]
void main(float4 position : SV_Position) {
    uint offset = uint(position.y) * pitch + uint(position.x);
    uint address = offset & ~3u, shift = (offset & 3u) * 8u;
    uint expected, observed;
    Values.InterlockedOr(address,0,expected);
    for (;;) {
        uint old = (expected >> shift) & 255u;
        uint value = (old & ~write_mask) | (Apply(old) & write_mask);
        uint desired = (expected & ~(255u << shift)) | (value << shift);
        Values.InterlockedCompareExchange(address,expected,desired,observed);
        if (observed == expected) break;
        expected = observed;
    }
}
)";
template <typename T, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type>
struct alignas(void*) PipelinePart {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{Type};
    T value;
    PipelinePart(const T& data) : value{data} {}
};
HRESULT CreateGraphicsPipeline(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                               bool depth_bounds, ID3D12PipelineState** state) {
    if (!depth_bounds)
        return device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(state));
    Microsoft::WRL::ComPtr<ID3D12Device2> extended;
    const HRESULT result = device->QueryInterface(IID_PPV_ARGS(&extended));
    if (FAILED(result))
        return result;
    const auto& original = desc.DepthStencilState;
    const D3D12_DEPTH_STENCIL_DESC1 depth{
        original.DepthEnable,   original.DepthWriteMask,  original.DepthFunc,
        original.StencilEnable, original.StencilReadMask, original.StencilWriteMask,
        original.FrontFace,     original.BackFace,        TRUE};
    D3D12_RT_FORMAT_ARRAY formats{};
    formats.NumRenderTargets = desc.NumRenderTargets;
    std::copy_n(desc.RTVFormats, 8, formats.RTFormats);
    struct {
        PipelinePart<ID3D12RootSignature*, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> root;
        PipelinePart<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS> vs;
        PipelinePart<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS> ps;
        PipelinePart<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS> ds;
        PipelinePart<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS> hs;
        PipelinePart<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS> gs;
        PipelinePart<D3D12_STREAM_OUTPUT_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT>
            so;
        PipelinePart<D3D12_BLEND_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND> blend;
        PipelinePart<UINT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK> mask;
        PipelinePart<D3D12_RASTERIZER_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER> raster;
        PipelinePart<D3D12_DEPTH_STENCIL_DESC1, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1>
            depth;
        PipelinePart<D3D12_INPUT_LAYOUT_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT>
            inputs;
        PipelinePart<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE,
                     D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE>
            cut;
        PipelinePart<D3D12_PRIMITIVE_TOPOLOGY_TYPE,
                     D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY>
            topology;
        PipelinePart<D3D12_RT_FORMAT_ARRAY,
                     D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS>
            formats;
        PipelinePart<DXGI_FORMAT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT> dsv;
        PipelinePart<DXGI_SAMPLE_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC> samples;
        PipelinePart<UINT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK> node;
        PipelinePart<D3D12_CACHED_PIPELINE_STATE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO>
            cache;
        PipelinePart<D3D12_PIPELINE_STATE_FLAGS, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS> flags;
    } stream{desc.pRootSignature,
             desc.VS,
             desc.PS,
             desc.DS,
             desc.HS,
             desc.GS,
             desc.StreamOutput,
             desc.BlendState,
             desc.SampleMask,
             desc.RasterizerState,
             depth,
             desc.InputLayout,
             desc.IBStripCutValue,
             desc.PrimitiveTopologyType,
             formats,
             desc.DSVFormat,
             desc.SampleDesc,
             desc.NodeMask,
             desc.CachedPSO,
             desc.Flags};
    const D3D12_PIPELINE_STATE_STREAM_DESC description{sizeof(stream), &stream};
    return extended->CreatePipelineState(&description, IID_PPV_ARGS(state));
}
std::vector<u8> PipelineKey(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, ID3DBlob* root,
                            bool depth_bounds = false) {
    std::vector<u8> key{2, u8(depth_bounds)};
    const auto integer = [&](u64 value) {
        for (u32 i = 0; i < 8; ++i)
            key.push_back(static_cast<u8>(value >> (i * 8)));
    };
    const auto bytes = [&](const void* data, size_t size) {
        integer(size);
        if (!size)
            return;
        const auto* begin = static_cast<const u8*>(data);
        key.insert(key.end(), begin, begin + size);
    };
    const auto real = [&](float value) { integer(std::bit_cast<u32>(value)); };
    bytes(root->GetBufferPointer(), root->GetBufferSize());
    bytes(desc.VS.pShaderBytecode, desc.VS.BytecodeLength);
    bytes(desc.PS.pShaderBytecode, desc.PS.BytecodeLength);
    bytes(desc.GS.pShaderBytecode, desc.GS.BytecodeLength);
    bytes(desc.HS.pShaderBytecode, desc.HS.BytecodeLength);
    bytes(desc.DS.pShaderBytecode, desc.DS.BytecodeLength);
    integer(desc.BlendState.AlphaToCoverageEnable);
    integer(desc.BlendState.IndependentBlendEnable);
    for (const auto& blend : desc.BlendState.RenderTarget) {
        integer(blend.BlendEnable);
        integer(blend.LogicOpEnable);
        integer(blend.SrcBlend);
        integer(blend.DestBlend);
        integer(blend.BlendOp);
        integer(blend.SrcBlendAlpha);
        integer(blend.DestBlendAlpha);
        integer(blend.BlendOpAlpha);
        integer(blend.LogicOp);
        integer(blend.RenderTargetWriteMask);
    }
    const auto& raster = desc.RasterizerState;
    integer(raster.FillMode);
    integer(raster.CullMode);
    integer(raster.FrontCounterClockwise);
    integer(raster.DepthBias);
    real(raster.DepthBiasClamp);
    real(raster.SlopeScaledDepthBias);
    integer(raster.DepthClipEnable);
    integer(raster.MultisampleEnable);
    integer(raster.AntialiasedLineEnable);
    integer(raster.ForcedSampleCount);
    integer(raster.ConservativeRaster);
    const auto& depth = desc.DepthStencilState;
    integer(depth.DepthEnable);
    integer(depth.DepthWriteMask);
    integer(depth.DepthFunc);
    integer(depth.StencilEnable);
    integer(depth.StencilReadMask);
    integer(depth.StencilWriteMask);
    for (const auto& face : {depth.FrontFace, depth.BackFace}) {
        integer(face.StencilFailOp);
        integer(face.StencilDepthFailOp);
        integer(face.StencilPassOp);
        integer(face.StencilFunc);
    }
    integer(desc.StreamOutput.NumEntries);
    for (u32 i = 0; i < desc.StreamOutput.NumEntries; ++i) {
        const auto& entry = desc.StreamOutput.pSODeclaration[i];
        integer(entry.Stream);
        bytes(entry.SemanticName, entry.SemanticName ? std::strlen(entry.SemanticName) : 0);
        integer(entry.SemanticIndex);
        integer(entry.StartComponent);
        integer(entry.ComponentCount);
        integer(entry.OutputSlot);
    }
    integer(desc.StreamOutput.NumStrides);
    for (u32 i = 0; i < desc.StreamOutput.NumStrides; ++i)
        integer(desc.StreamOutput.pBufferStrides[i]);
    integer(desc.StreamOutput.RasterizedStream);
    integer(desc.InputLayout.NumElements);
    for (u32 i = 0; i < desc.InputLayout.NumElements; ++i) {
        const auto& element = desc.InputLayout.pInputElementDescs[i];
        bytes(element.SemanticName, std::strlen(element.SemanticName));
        integer(element.SemanticIndex);
        integer(element.Format);
        integer(element.InputSlot);
        integer(element.AlignedByteOffset);
        integer(element.InputSlotClass);
        integer(element.InstanceDataStepRate);
    }
    integer(desc.SampleMask);
    integer(desc.IBStripCutValue);
    integer(desc.PrimitiveTopologyType);
    integer(desc.NumRenderTargets);
    for (const auto format : desc.RTVFormats)
        integer(format);
    integer(desc.DSVFormat);
    integer(desc.SampleDesc.Count);
    integer(desc.SampleDesc.Quality);
    integer(desc.NodeMask);
    integer(desc.Flags);
    return key;
}
} // namespace
u64 CommandRuntime::GsInvocations(D3D_PRIMITIVE_TOPOLOGY topology, u64 count) {
    if (!count)
        return 0;
    switch (topology) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
        return count;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
        return count / 2;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
        return count - 1;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
        return count / 4;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
        return count > 3 ? count - 3 : 0;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
        return count / 3;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
        return count > 2 ? count - 2 : 0;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:
        return count / 6;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
        return count > 3 ? (count - 2) / 2 : 0;
    default:
        throw std::runtime_error("Unsupported D3D12 topology for geometry invocation count");
    }
}
void CommandRuntime::Draw(const DrawRequest& request) {
    ProfileSample(profile, "runtime_draw", 128, [&] { DrawImpl(request); });
}
void CommandRuntime::DrawDepthGated(const DrawRequest& request) {
    std::scoped_lock lock{mutex};
    if (!request.count || !request.instances)
        return;
    if (request.point_fill || request.line_expand ||
        std::ranges::any_of(request.faces,
                            [](const auto& face) { return face.point_fill || face.line_expand; }))
        throw std::runtime_error("Depth-result color gating requires unexpanded rasterization");
    const auto writes_memory = [](const Shader::Info& info) {
        return info.uses_dma || info.has_storage_images ||
               std::ranges::any_of(info.buffers,
                                   [](const auto& buffer) { return buffer.is_written; }) ||
               std::ranges::any_of(info.images, [](const auto& image) {
                   return image.is_written || image.is_atomic;
               });
    };
    const auto fragment = request.fragment.code.empty()
                              ? nullptr
                              : shaders.Translate(request.fragment, request.fragment_runtime);
    const bool storage = fragment && writes_memory(fragment->shader.info);
    if (storage) {
        const auto& info = fragment->shader.info;
        if (request.samples != 1 || request.alpha_to_coverage || request.depth_bounds ||
            info.has_discard || info.stores.Get(Shader::IR::Attribute::Depth) ||
            info.stores.Get(Shader::IR::Attribute::SampleMask) ||
            info.stores.Get(Shader::IR::Attribute::StencilRef))
            throw std::runtime_error(
                "Depth-result gating needs capture of fragment coverage/depth");
        const auto ranges = GetShaderResourceRanges(info);
        for (const auto& range : ranges) {
            const auto aliases = [&](VAddr address, u64 size) {
                return resources.Aliases(address, size, range.address, range.size);
            };
            if (request.depth_image &&
                (aliases(request.depth_image->guest_address, request.depth_image->guest_size) ||
                 aliases(request.depth_image->stencil_addr, request.depth_image->stencil_size) ||
                 aliases(request.depth_read_address, request.depth_image->guest_size) ||
                 aliases(request.stencil_read_address, request.depth_image->stencil_size)))
                throw std::runtime_error(
                    "Depth-result gating needs ordered depth/stencil feedback");
            if (range.written && request.index_address &&
                aliases(request.index_address,
                        u64(request.count) *
                            (request.index_format == DXGI_FORMAT_R16_UINT ? 2 : 4)))
                throw std::runtime_error("Depth-result gating needs coherent index capture");
        }
    }
    const auto inverse = [](D3D12_COMPARISON_FUNC value) {
        if (value < D3D12_COMPARISON_FUNC_NEVER || value > D3D12_COMPARISON_FUNC_ALWAYS)
            throw std::runtime_error("Invalid guest comparison function");
        return D3D12_COMPARISON_FUNC(9 - u32(value));
    };
    const auto keep_stencil = [](D3D12_DEPTH_STENCILOP_DESC& face) {
        face.StencilFailOp = face.StencilDepthFailOp = face.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    };
    auto replay = request;
    replay.depth_gated = false;
    replay.faces.clear();
    if (storage)
        replay.fragment_runtime.hw.fs.depth_before_shader = true;
    auto original = request.faces;
    if (original.empty())
        original.push_back(
            {request.rasterizer, request.depth, request.stencil_ref, false, false, true});
    for (u32 target = 0; target < 8; ++target) {
        if (!request.blend.IndependentBlendEnable)
            replay.blend.RenderTarget[target] = request.blend.RenderTarget[0];
        replay.blend.RenderTarget[target].RenderTargetWriteMask |= request.depth_fail_masks[target];
    }
    replay.blend.IndependentBlendEnable = TRUE;
    for (auto face : original) {
        if (!face.color_masks) {
            std::array<u8, 8> masks{};
            for (u32 target = 0; target < 8; ++target)
                masks[target] =
                    request.blend.RenderTarget[request.blend.IndependentBlendEnable ? target : 0]
                        .RenderTargetWriteMask;
            face.color_masks = masks;
        }
        auto fail = face;
        fail.stencil.reset();
        fail.count_occlusion = false;
        fail.depth.DepthFunc = inverse(face.depth.DepthFunc);
        fail.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        keep_stencil(fail.depth.FrontFace);
        keep_stencil(fail.depth.BackFace);
        fail.color_masks = request.depth_fail_masks;
        if (storage && face.depth.StencilEnable &&
            !request.fragment_runtime.hw.fs.depth_before_shader) {
            auto stencil_fail = fail;
            stencil_fail.depth.DepthEnable = FALSE;
            stencil_fail.depth.FrontFace.StencilFunc = inverse(face.depth.FrontFace.StencilFunc);
            stencil_fail.depth.BackFace.StencilFunc = inverse(face.depth.BackFace.StencilFunc);
            stencil_fail.color_masks = std::array<u8, 8>{};
            replay.faces.push_back(stencil_fail);
        }
        replay.faces.push_back(fail);
        replay.faces.push_back(face);
    }
    DrawImpl(replay);
}
void CommandRuntime::DrawImpl(const DrawRequest& request) {
    if (request.depth_gated) {
        DrawDepthGated(request);
        return;
    }
    if ((!request.faces.empty() || request.flat_last || request.viewport_transform) &&
        !request.geometry && !request.hull &&
        (request.topology == D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ ||
         request.topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ ||
         request.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ ||
         request.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ))
        throw std::runtime_error(
            "Ordered raster capture requires adjacency index compaction without guest GS");
    const bool stencil_replay = std::ranges::any_of(request.faces, [](const auto& face) {
        return face.stencil && face.depth.StencilWriteMask != 0;
    });
    if (stencil_replay && (request.samples != 1 || request.alpha_to_coverage ||
                           std::ranges::any_of(request.faces, [](const auto& face) {
                               return face.point_fill || face.line_expand;
                           })))
        throw std::runtime_error(
            "Ordered stencil updates require single-sample unexpanded rasterization");
    for (const auto& face : request.faces)
        if (face.depth.DepthEnable && (face.point_fill || face.line_expand) &&
            (face.rasterizer.DepthBias != 0 || face.rasterizer.SlopeScaledDepthBias != 0))
            throw std::runtime_error(
                "Per-face bias on expanded points/lines requires original-primitive bias lowering");
    const u32 sample_iterations = request.sample_iterations ? request.sample_iterations
                                  : request.sample_shading  ? request.samples
                                                            : 1u;
    if (!std::has_single_bit(request.samples) || request.samples > 16 ||
        !std::has_single_bit(sample_iterations) || sample_iterations > request.samples)
        throw std::runtime_error("Invalid guest coverage/shading sample counts");
    if (sample_iterations != 1 && sample_iterations != request.samples)
        throw std::runtime_error("Guest partial sample shading requires grouped execution; "
                                 "full-rate replay changes side effects");
    for (const auto& color : request.colors)
        if (color.num_samples != request.samples)
            throw std::runtime_error("Guest EQAA requires coverage-to-fragment mapping");
    if (request.depth_image && request.depth_image->num_samples != request.samples)
        throw std::runtime_error("Guest EQAA requires coverage-to-depth anchor mapping");
    const u32 index_bytes = request.index_format == DXGI_FORMAT_R16_UINT ? 2 : 4;
    const u32 index_unit = request.index_swap == AmdGpu::IndexSwapMode::None ? index_bytes
                           : request.index_swap == AmdGpu::IndexSwapMode::Swap16
                               ? std::max(2u, index_bytes)
                               : 4;
    const u32 index_source_count = std::min(
        request.expansion ? request.source_index_count : request.count, request.index_capacity);
    const u64 index_read_count =
        std::min<u64>((u64(index_source_count) + index_unit / index_bytes - 1) /
                          (index_unit / index_bytes) * (index_unit / index_bytes),
                      request.index_read_count);
    std::scoped_lock lock{mutex};
    MaintainMemory();
    if (!contexts.empty()) {
        std::vector<ResourceCache::Range> writes, reads;
        if (request.indirect) {
            const auto& indirect = *request.indirect;
            reads.push_back({indirect.address, u64(indirect.count - 1) * indirect.stride +
                                                   (indirect.indexed ? 20 : 16)});
            reads.push_back({indirect.count_address, indirect.count_address ? 4u : 0u});
        }
        const auto collect = [&](const Shader::ShaderParams& params,
                                 const Shader::RuntimeInfo& state, bool vertex) {
            if (params.code.empty())
                return;
            const auto translated = shaders.Translate(params, state);
            const auto& info = translated->shader.info;
            for (const auto& image : info.images) {
                const auto image_info = ShaderImageLayout(image.GetSharp(info), image);
                reads.push_back({image_info.guest_address, image_info.guest_size});
            }
            for (const auto& buffer : info.buffers) {
                if (buffer.buffer_type != Shader::BufferType::Guest)
                    continue;
                const auto sharp = buffer.GetSharp(info);
                const ResourceCache::Range range{
                    sharp.base_address, u64(sharp.num_records) * std::max<u32>(1, sharp.stride)};
                reads.push_back(range);
                if (buffer.is_written)
                    writes.push_back(range);
            }
            if (vertex) {
                Shader::Gcn::FetchShaderData fetch;
                Shader::Gcn::ParseFetchShader(info, fetch);
                for (const auto& attribute : fetch.attributes) {
                    const auto sharp = attribute.GetSharp(info);
                    reads.push_back({sharp.base_address,
                                     u64(sharp.num_records) * std::max<u32>(1, sharp.stride)});
                }
            }
        };
        collect(request.fragment, request.fragment_runtime, false);
        if (!request.hull)
            collect(request.vertex, request.vertex_runtime, true);
        if (request.geometry)
            collect(*request.geometry, request.geometry_runtime, false);
        if (request.index_address || request.index_buffer_bound)
            reads.push_back(
                {request.index_address,
                 u64(index_read_count) * (request.index_format == DXGI_FORMAT_R16_UINT ? 2 : 4)});
        for (const auto& color : request.colors)
            writes.push_back({color.guest_address, color.guest_size});
        for (const auto& slot : request.strmout)
            if (slot.address && slot.size)
                writes.push_back({slot.address, slot.size});
        if (request.depth_image) {
            const auto& depth = *request.depth_image;
            writes.push_back({depth.guest_address, depth.guest_size});
            writes.push_back({depth.stencil_addr, depth.stencil_size});
        }
        auto& context = NextContext(writes, reads);
        struct Scope {
            bool& active;
            ~Scope() {
                active = false;
            }
        } scope{context.recording_context};
        context.recording_context = true;
        context.DrawImpl(request);
        return;
    }
    strmout_filled = 0;
    if (failed)
        throw std::runtime_error(
            "D3D12 command runtime requires recreation after recording failure");
    if (!request.count || !request.instances)
        return;
    if (!std::has_single_bit(request.samples) || request.samples > 16 ||
        request.colors.size() > 8 || request.color_slots.size() != request.colors.size() ||
        request.color_base_slice.size() != request.colors.size() ||
        ((request.depth.DepthEnable || request.depth.StencilEnable || request.depth_bounds) &&
         !request.depth_image) ||
        (!request.depth.DepthEnable && !request.depth.StencilEnable && !request.depth_bounds &&
         request.depth_image) ||
        request.vertex_runtime.sw_stage != Shader::SwStage::Vertex ||
        request.fragment_runtime.sw_stage != Shader::SwStage::Fragment ||
        (request.geometry && request.geometry_runtime.sw_stage != Shader::SwStage::Geometry) ||
        (!request.converted_indices.empty() && request.count != request.converted_indices.size()) ||
        request.hull.has_value() != request.domain.has_value())
        throw std::runtime_error("Invalid or unsupported D3D12 guest graphics state");
    const float bounds_min = std::isnan(request.bounds_min) ? 0.f : request.bounds_min;
    const float bounds_max = std::isnan(request.bounds_max) ? 0.f : request.bounds_max;
    const bool empty_bounds = request.depth_bounds && bounds_min > bounds_max;
    const auto apply_empty_bounds = [&](D3D12_DEPTH_STENCIL_DESC& depth) {
        if (empty_bounds) {
            depth.DepthEnable = TRUE;
            depth.DepthFunc = D3D12_COMPARISON_FUNC_NEVER;
            depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            depth.StencilEnable = FALSE;
        }
    };
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList1> depth_bounds_commands;
    if (request.depth_bounds) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS2 options{};
        if (FAILED(device.Device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options,
                                                        sizeof(options))) ||
            !options.DepthBoundsTestSupported)
            throw std::runtime_error("Guest depth bounds requires native stored-depth testing");
        winrt::check_hresult(commands.As(&depth_bounds_commands));
    }
    Shader::Backend::Bindings next_bindings{};
    auto vertex_runtime = request.vertex_runtime;
    auto domain_runtime = request.domain_runtime;
    auto geometry_runtime = request.geometry_runtime;
    geometry_runtime.hw.gs.export_vertex_order = request.flat_last;
    if (request.viewport_transform) {
        if (vertex_runtime.hw_stage == Shader::HwStage::Vertex)
            vertex_runtime.hw.vs.clip_disable = false;
        if (domain_runtime.hw_stage == Shader::HwStage::Vertex)
            domain_runtime.hw.vs.clip_disable = false;
    }
    std::optional<CompiledShader> hull, domain;
    std::vector<CompiledShader*> stages;
    std::unique_ptr<TranslatedShader> translated_hull, translated_domain;
    std::optional<TessellationLink> tessellation_link;
    if (request.hull) {
        if (request.hull_runtime.sw_stage != Shader::SwStage::TessellationControl ||
            domain_runtime.sw_stage != Shader::SwStage::TessellationEval ||
            request.hull_runtime.sw.tcs.tess_type != domain_runtime.sw.tes.tess_type ||
            vertex_runtime.hw_stage != Shader::HwStage::Local)
            throw std::runtime_error("Invalid native tessellation stage configuration");
        tessellation_link.emplace(TessellationLink{
            domain_runtime.sw.tes.tess_type, domain_runtime.sw.tes.tess_partitioning,
            domain_runtime.sw.tes.tess_topology,
            request.hull_runtime.sw.tcs.num_input_control_points, 0});
        tessellation_link->ValidateNativeTopology();
        translated_hull = shaders.Translate(*request.hull, request.hull_runtime);
        Shader::TessellationDataConstantBuffer constants{};
        translated_hull->shader.info.ReadTessConstantBuffer(constants);
        vertex_runtime.hw.ls.ls_stride = constants.ls_stride;
        domain_runtime.InitFromTessConstants(constants);
        tessellation_link->output_control_points =
            translated_hull->shader.runtime.sw.tcs.NumOutputControlPoints();
        translated_domain = shaders.Translate(*request.domain, domain_runtime);
    }
    auto translated_vertex = shaders.Translate(request.vertex, vertex_runtime);
    auto translated_geometry =
        request.geometry ? shaders.Translate(*request.geometry, geometry_runtime) : nullptr;
    auto translated_fragment = !request.fragment.code.empty()
                                   ? shaders.Translate(request.fragment, request.fragment_runtime)
                                   : nullptr;
    std::vector<ShaderResourceRange> stage_resources;
    for (const auto* translated :
         {translated_hull.get(), translated_domain.get(), translated_vertex.get(),
          translated_geometry.get(), translated_fragment.get()}) {
        if (!translated)
            continue;
        const auto ranges = GetShaderResourceRanges(translated->shader.info);
        stage_resources.insert(stage_resources.end(), ranges.begin(), ranges.end());
    }
    IndexFetch index_fetch;
    if (request.index_address || request.index_buffer_bound) {
        const u32 bytes = request.index_format == DXGI_FORMAT_R16_UINT ? 2 : 4;
        const u64 size = u64(index_read_count) * bytes;
        const bool feedback = std::ranges::any_of(stage_resources, [&](const auto& range) {
            return range.written &&
                   resources.Aliases(request.index_address, size, range.address, range.size);
        });
        if (feedback) {
            if (request.indirect)
                throw GpuDrawFallback{};
            if (request.expansion || !request.converted_indices.empty() ||
                request.strip_cut != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED)
                throw std::runtime_error("D3D12 coherent index fetch with primitive "
                                         "conversion/restart is not implemented");
            if (!size || size > 16ULL * 1024 * 1024 ||
                (request.index_format != DXGI_FORMAT_R16_UINT &&
                 request.index_format != DXGI_FORMAT_R32_UINT))
                throw std::runtime_error("Invalid D3D12 coherent index range");
            const u32 byte_xor = request.index_swap == AmdGpu::IndexSwapMode::Swap16     ? 1
                                 : request.index_swap == AmdGpu::IndexSwapMode::Swap32   ? 3
                                 : request.index_swap == AmdGpu::IndexSwapMode::SwapWord ? 2
                                                                                         : 0;
            index_fetch = {request.index_address, u32(size), bytes, byte_xor,
                           request.index_capacity};
            stage_resources.push_back(
                {request.index_address, size, Shader::SwStage::Vertex, false});
        }
    }
    if (translated_hull) {
        auto prepared_hull =
            shaders.Prepare(std::move(translated_hull), next_bindings, stage_resources);
        next_bindings = prepared_hull.shader.end_bindings;
        auto prepared_domain =
            shaders.Prepare(std::move(translated_domain), next_bindings, stage_resources);
        const auto& link = *tessellation_link;
        hull.emplace(
            shaders.Finalize(prepared_hull, false, false, false, &link, prepared_domain.words));
        domain.emplace(shaders.Finalize(
            prepared_domain, !request.viewport_transform && !request.geometry && request.flip_y,
            false, false, &link, prepared_hull.words, false,
            !request.viewport_transform && !request.geometry && request.flip_x));
        next_bindings = domain->end_bindings;
        stages.push_back(&*hull);
        stages.push_back(&*domain);
    }
    const auto compile_stage = [&](std::unique_ptr<TranslatedShader> translated,
                                   Shader::Backend::Bindings bindings, bool flip_y,
                                   bool sample_mask, bool sample_shading, bool depth_bounds,
                                   bool flip_x) {
        const auto indices = translated->shader.info.sw_stage == Shader::SwStage::Vertex
                                 ? index_fetch
                                 : IndexFetch{};
        auto prepared = shaders.Prepare(std::move(translated), bindings, stage_resources, indices);
        return shaders.Finalize(prepared, flip_y, sample_mask, sample_shading, nullptr, {},
                                depth_bounds, flip_x);
    };
    auto vertex = ProfileSample(profile, "pso_compile", 128, [&] {
        return compile_stage(
            std::move(translated_vertex), next_bindings,
            !request.viewport_transform && !request.geometry && !request.domain && request.flip_y,
            false, false, false,
            !request.viewport_transform && !request.geometry && !request.domain && request.flip_x);
    });
    std::array<DXGI_FORMAT, 8> logic_formats{};
    std::array<u32, 8> logic_masks{};
    for (u32 bound = 0; bound < request.colors.size(); ++bound) {
        const auto slot = request.color_slots[bound];
        if (slot >= 8)
            throw std::runtime_error("Invalid logic operation target slot");
        const auto format = ConvertFormat(request.colors[bound].pixel_format);
        if (request.blend.RenderTarget[slot].LogicOpEnable) {
            logic_formats[slot] = format;
            logic_masks[slot] = request.blend.RenderTarget[slot].RenderTargetWriteMask;
        }
    }
    std::optional<CompiledShader> fragment;
    std::optional<CompiledShader> geometry;
    std::shared_ptr<const ShaderBinary> rectangle;
    std::shared_ptr<const ShaderBinary> point_fill;
    std::shared_ptr<const ShaderBinary> line_expand;
    stages.push_back(&vertex);
    auto end_bindings = vertex.end_bindings;
    const bool capture_geometry = !request.faces.empty() || request.flat_last ||
                                  request.viewport_transform ||
                                  (request.geometry || domain ||
                                   (request.line_expand && request.line_pattern_scale > 0 &&
                                    request.line_pattern != u16(0xffff))) &&
                                      (request.point_fill || request.line_expand);
    if (request.geometry) {
        geometry.emplace(compile_stage(std::move(translated_geometry), end_bindings,
                                       !request.viewport_transform && request.flip_y, false, false,
                                       false, !request.viewport_transform && request.flip_x));
        end_bindings = geometry->end_bindings;
        stages.push_back(&*geometry);
    }
    const auto output_primitive =
        geometry ? request.geometry_runtime.hw.gs.out_primitive[0]
        : domain ? tessellation_link->topology == AmdGpu::TessellationTopology::Line
                       ? AmdGpu::GsOutputPrimitiveType::LineStrip
                   : tessellation_link->topology == AmdGpu::TessellationTopology::Point
                       ? AmdGpu::GsOutputPrimitiveType::PointList
                       : AmdGpu::GsOutputPrimitiveType::TriangleStrip
        : request.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
            ? AmdGpu::GsOutputPrimitiveType::PointList
        : request.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
            ? AmdGpu::GsOutputPrimitiveType::LineStrip
            : AmdGpu::GsOutputPrimitiveType::TriangleStrip;
    const u32 replay_vertices = output_primitive == AmdGpu::GsOutputPrimitiveType::TriangleStrip ? 3
                                : output_primitive == AmdGpu::GsOutputPrimitiveType::LineStrip   ? 2
                                                                                               : 1;
    const std::string replay_input = replay_vertices == 3   ? "triangle"
                                     : replay_vertices == 2 ? "line"
                                                            : "point";
    const std::string replay_stream = replay_vertices == 3   ? "TriangleStream"
                                      : replay_vertices == 2 ? "LineStream"
                                                             : "PointStream";
    auto last_stage = geometry ? geometry->binary : domain ? domain->binary : vertex.binary;
    std::shared_ptr<const ShaderBinary> face_capture_shader;
    const bool capture_primitive_id =
        (!request.faces.empty() || request.flat_last || request.viewport_transform) && !geometry &&
        !request.rectangles;
    if (capture_primitive_id) {
        const auto input = shaders.OutputInterface(*last_stage);
        auto output = input;
        const auto name = output.find("SPIRV_Cross_Output");
        output.replace(name, 18, "Capture_Output");
        output.insert(output.find("};"), "uint capture_primitive : SV_PrimitiveID;\n");
        std::string source = input + output + "[maxvertexcount(" + std::to_string(replay_vertices) +
                             ")] void main(" + replay_input + " SPIRV_Cross_Output input[" +
                             std::to_string(replay_vertices) +
                             "], uint primitive : SV_PrimitiveID, inout " + replay_stream +
                             "<Capture_Output> stream) { for(uint i=0;i<" +
                             std::to_string(replay_vertices) + ";++i) { Capture_Output value;\n";
        const auto fields = shaders.StageOutputs(*last_stage);
        for (u32 field = 0; field < fields.size(); ++field)
            source +=
                "value.p" + std::to_string(field) + " = input[i].p" + std::to_string(field) + ";\n";
        source += "value.capture_primitive = primitive; stream.Append(value); } }\n";
        face_capture_shader = shaders.CompileHelperHlsl(source, Shader::SwStage::Geometry);
        last_stage = face_capture_shader;
    }
    if (request.rectangles) {
        if (request.geometry)
            throw std::runtime_error("Guest geometry cannot precede rectangle expansion");
        rectangle = shaders.CompileRectangle(*vertex.binary, request.fragment_runtime.hw.fs,
                                             request.flat_last);
        if (!request.faces.empty()) {
            auto source = rectangle->hlsl;
            const auto maximum = source.find("maxvertexcount(4)");
            const std::string emit =
                "for(uint i=0;i<4;++i) stream.Append(v[i]); stream.RestartStrip();";
            const auto start = source.find(emit);
            if (maximum == std::string::npos || start == std::string::npos)
                throw std::runtime_error("Invalid rectangle capture helper");
            source.replace(maximum, 17, "maxvertexcount(6)");
            source.replace(start, emit.size(),
                           "stream.Append(v[0]); stream.Append(v[1]); stream.Append(v[2]); "
                           "stream.RestartStrip(); "
                           "stream.Append(v[1]); stream.Append(v[3]); stream.Append(v[2]); "
                           "stream.RestartStrip();");
            const auto input = shaders.OutputInterface(*vertex.binary);
            auto output = input;
            output.replace(output.find("SPIRV_Cross_Output"), 18, "Capture_Output");
            output.insert(output.find("};"), "uint capture_primitive : SV_PrimitiveID;\n");
            std::string convert = output + "Capture_Output ConvertRectangle(SPIRV_Cross_Output "
                                           "value, uint primitive) { Capture_Output result;\n";
            const auto fields = shaders.StageOutputs(*vertex.binary);
            const auto original =
                source.substr(source.find("struct SPIRV_Cross_Output"), source.find("};") + 2);
            for (u32 field = 0; field < fields.size(); ++field) {
                const auto& item = fields[field];
                const auto semantic =
                    item.semantic +
                    (item.semantic.starts_with("SV_") ? "" : std::to_string(item.index));
                const std::regex member{"(\\w+)\\s*:\\s*" + semantic + "\\s*;"};
                std::smatch match;
                if (!std::regex_search(original, match, member))
                    throw std::runtime_error("Missing rectangle output semantic");
                convert +=
                    "result.p" + std::to_string(field) + " = value." + match[1].str() + ";\n";
            }
            convert += "result.capture_primitive = primitive; return result; }\n";
            source.insert(source.find("};") + 2, "\n" + convert);
            const std::string stream = "inout TriangleStream<SPIRV_Cross_Output> stream";
            source.replace(
                source.find(stream), stream.size(),
                "uint primitive : SV_PrimitiveID, inout TriangleStream<Capture_Output> stream");
            for (u32 vertex_index = 0; vertex_index < 4; ++vertex_index) {
                const auto append = "stream.Append(v[" + std::to_string(vertex_index) + "])";
                const auto replacement = "stream.Append(ConvertRectangle(v[" +
                                         std::to_string(vertex_index) + "], primitive))";
                for (size_t offset = 0; (offset = source.find(append, offset)) != std::string::npos;
                     offset += replacement.size())
                    source.replace(offset, append.size(), replacement);
            }
            rectangle = shaders.CompileHelperHlsl(source, Shader::SwStage::Geometry);
        }
    }
    if (capture_geometry && rectangle)
        last_stage = rectangle;
    ShaderBinary expansion_interface;
    if (capture_geometry) {
        expansion_interface.dxil = last_stage->dxil;
        expansion_interface.hlsl = shaders.OutputInterface(*last_stage);
    }
    const ShaderBinary& expansion_source = capture_geometry ? expansion_interface : *last_stage;
    u32 line_linear{}, line_flat{};
    if (translated_fragment)
        for (u32 input = 0; input < request.fragment_runtime.hw.fs.num_inputs; ++input) {
            const auto parameter = request.fragment_runtime.hw.fs.inputs[input].param_index;
            if (parameter < 32) {
                line_linear |= translated_fragment->shader.info.fs_interpolation[input].primary ==
                                       Shader::Qualifier::NoPerspective
                                   ? 1u << parameter
                                   : 0;
                line_flat |= translated_fragment->shader.info.fs_interpolation[input].primary ==
                                     Shader::Qualifier::Flat
                                 ? 1u << parameter
                                 : 0;
            }
        }
    if (request.point_fill) {
        point_fill = shaders.CompilePointFill(expansion_source, request.point_keep_front,
                                              request.rasterizer.FrontCounterClockwise != 0,
                                              request.point_cull, &request.fragment_runtime.hw.fs);
    }
    if (request.line_expand) {
        const auto& source_binary = expansion_source;
        const bool adjacency =
            !capture_geometry && (request.topology == D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ ||
                                  request.topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ ||
                                  request.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ ||
                                  request.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ);
        const auto input_topology = [&]() {
            if (geometry)
                return request.geometry_runtime.hw.gs.out_primitive[0] ==
                               AmdGpu::GsOutputPrimitiveType::LineStrip
                           ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                           : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            if (request.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH)
                return tessellation_link->topology == AmdGpu::TessellationTopology::Line
                           ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                           : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            return request.topology_type;
        }();
        line_expand = shaders.CompileLineExpand(
            source_binary, request.fragment_runtime.hw.fs, request.line_keep_front,
            request.rasterizer.FrontCounterClockwise != 0, input_topology, adjacency,
            request.line_width, request.line_pattern, request.line_pattern_repeat,
            request.line_pattern_msb, request.line_pattern_scale, request.line_viewport_dims,
            request.line_cull, capture_geometry, false, line_linear, line_flat,
            request.rasterizer.DepthClipEnable != 0);
    }
    auto blend = request.blend;
    const bool constant_alpha_emulation = request.constant_alpha_blend_emulation &&
                                          blend.RenderTarget[0].BlendOp != D3D12_BLEND_OP_MIN &&
                                          blend.RenderTarget[0].BlendOp != D3D12_BLEND_OP_MAX;
    if (request.constant_alpha_blend_emulation && !constant_alpha_emulation)
        for (auto* factor : {&blend.RenderTarget[0].SrcBlend, &blend.RenderTarget[0].DestBlend}) {
            if (*factor == D3D12_BLEND_ALPHA_FACTOR)
                *factor = D3D12_BLEND_BLEND_FACTOR;
            else if (*factor == D3D12_BLEND_INV_ALPHA_FACTOR)
                *factor = D3D12_BLEND_INV_BLEND_FACTOR;
        }
    if (constant_alpha_emulation) {
        if (request.color_slots.size() != 1 || request.color_slots[0] != 0 ||
            request.fragment.code.empty())
            throw std::runtime_error(
                "Constant color/alpha blend emulation requires one color target at slot zero");
        const auto secondary = [](D3D12_BLEND factor) {
            return factor == D3D12_BLEND_SRC1_COLOR || factor == D3D12_BLEND_INV_SRC1_COLOR ||
                   factor == D3D12_BLEND_SRC1_ALPHA || factor == D3D12_BLEND_INV_SRC1_ALPHA;
        };
        auto& target = blend.RenderTarget[0];
        if (secondary(target.SrcBlend) || secondary(target.DestBlend) ||
            secondary(target.SrcBlendAlpha) || secondary(target.DestBlendAlpha))
            throw std::runtime_error(
                "Constant alpha blend emulation conflicts with dual-source guest blending");
        for (auto* factor : {&target.SrcBlend, &target.DestBlend}) {
            if (*factor == D3D12_BLEND_ALPHA_FACTOR)
                *factor = D3D12_BLEND_SRC1_COLOR;
            else if (*factor == D3D12_BLEND_INV_ALPHA_FACTOR)
                *factor = D3D12_BLEND_INV_SRC1_COLOR;
        }
        blend.IndependentBlendEnable = FALSE;
    }
    if (!request.fragment.code.empty()) {
        fragment.emplace(compile_stage(std::move(translated_fragment), end_bindings, false,
                                       request.samples > 1, sample_iterations > 1, false, false));
        if (std::any_of(logic_formats.begin(), logic_formats.end(),
                        [](auto format) { return format != DXGI_FORMAT_UNKNOWN; }))
            fragment->binary =
                shaders.CompileLogicOp(*fragment->binary, logic_formats, logic_masks);
        if (constant_alpha_emulation)
            fragment->binary =
                shaders.CompileConstantAlphaBlend(*fragment->binary, request.blend_factor[3]);
        stages.push_back(&*fragment);
    } else if (std::ranges::any_of(request.color_slots, [&](u32 slot) {
                   return request.blend
                              .RenderTarget[request.blend.IndependentBlendEnable ? slot : 0]
                              .RenderTargetWriteMask != 0;
               }))
        throw std::runtime_error("D3D12 color rendering requires a fragment shader");
    if (stencil_replay && fragment &&
        (fragment->info.has_discard || fragment->info.stores.Get(Shader::IR::Attribute::Depth) ||
         fragment->info.stores.Get(Shader::IR::Attribute::SampleMask) ||
         fragment->info.stores.Get(Shader::IR::Attribute::StencilRef)))
        throw std::runtime_error(
            "Ordered stencil updates require capture of fragment coverage/depth exports");
    PrepareContext(stages);
    if (request.indirect &&
        (request.geometry || request.hull || request.rectangles || request.clear_depth ||
         request.clear_stencil || std::ranges::any_of(stages, [](const auto* stage) {
             return std::ranges::any_of(stage->info.buffers,
                                        [](const auto& buffer) { return buffer.is_written; }) ||
                    std::ranges::any_of(stage->info.images,
                                        [](const auto& image) { return image.is_written; });
         })))
        throw GpuDrawFallback{};
    ResourceBindings bindings{device,
                              resources,
                              images,
                              stages,
                              request.strmout,
                              &resource_descriptors,
                              &sampler_descriptors,
                              coordinator != nullptr};
    Shader::Gcn::FetchShaderData fetch;
    if (!vertex.info.has_inline_fetch_shader)
        Shader::Gcn::ParseFetchShader(vertex.info, fetch);
    std::vector<AmdGpu::Buffer> vertex_sharps;
    auto access_ranges = bindings.AccessRanges(request.border_colors);
    for (const auto& attribute : fetch.attributes) {
        const auto sharp = attribute.GetSharp(vertex.info);
        vertex_sharps.push_back(sharp);
        access_ranges.push_back(
            {sharp.base_address, u64(sharp.num_records) * std::max<u32>(1, sharp.stride)});
    }
    if (request.index_address || request.index_buffer_bound)
        access_ranges.push_back(
            {request.index_address,
             u64(index_read_count) * (request.index_format == DXGI_FORMAT_R16_UINT ? 2 : 4)});
    for (const auto& color : request.colors)
        access_ranges.push_back({color.guest_address, color.guest_size});
    if (request.depth_image) {
        const auto& depth = *request.depth_image;
        access_ranges.push_back({depth.guest_address, depth.guest_size});
        access_ranges.push_back({depth.stencil_addr, depth.stencil_size});
        access_ranges.push_back({request.depth_read_address, depth.guest_size});
        access_ranges.push_back({request.stencil_read_address, depth.stencil_size});
    }
    if (!request.color_clear_buffers.empty() &&
        request.color_clear_buffers.size() != request.colors.size())
        throw std::runtime_error("D3D12 CMASK descriptors do not match color targets");
    for (const auto& buffer : request.color_clear_buffers)
        if (buffer.info.fast_clear && buffer.cmask_base_address)
            access_ranges.push_back({buffer.CmaskAddress(), (u64(buffer.cmask_slice.tile_max) + 1) *
                                                                128 * buffer.NumSlices()});
    std::vector<ResourceCache::Range> gpu_ranges;
    if (request.indirect) {
        const auto& indirect = *request.indirect;
        if (!indirect.count || indirect.count > 4096)
            throw std::runtime_error("Invalid D3D12 GPU indirect draw count");
        gpu_ranges.push_back({indirect.address, u64(indirect.count - 1) * indirect.stride +
                                                    (indirect.indexed ? 20 : 16)});
        gpu_ranges.push_back({indirect.count_address, indirect.count_address ? 4u : 0u});
    }
    if (request.expansion && (request.index_address || request.index_buffer_bound))
        gpu_ranges.push_back(
            {request.index_address,
             u64(index_read_count) * (request.index_format == DXGI_FORMAT_R16_UINT ? 2 : 4)});
    access_ranges.insert(access_ranges.end(), gpu_ranges.begin(), gpu_ranges.end());
    resources.BeginBatch();
    BeginImages();
    auto cpu_ranges = bindings.CpuRanges(request.border_colors);
    for (const auto& sharp : vertex_sharps)
        cpu_ranges.push_back(
            {sharp.base_address, u64(sharp.num_records) * std::max<u32>(1, sharp.stride)});
    if (request.index_address || request.index_buffer_bound)
        cpu_ranges.push_back(
            {request.index_address,
             u64(index_read_count) * (request.index_format == DXGI_FORMAT_R16_UINT ? 2 : 4)});
    for (u32 i = 0; i < request.color_clear_buffers.size(); ++i) {
        const auto& buffer = request.color_clear_buffers[i];
        if (buffer.info.fast_clear && buffer.cmask_base_address)
            cpu_ranges.push_back({request.colors[i].guest_address, request.colors[i].guest_size});
    }
    if (request.depth_image &&
        ((request.depth_read_address &&
          request.depth_read_address != request.depth_image->guest_address) ||
         (request.stencil_read_address &&
          request.stencil_read_address != request.depth_image->stencil_addr))) {
        cpu_ranges.push_back({request.depth_image->guest_address, request.depth_image->guest_size});
        cpu_ranges.push_back(
            {request.depth_image->stencil_addr, request.depth_image->stencil_size});
        cpu_ranges.push_back({request.depth_read_address, request.depth_image->guest_size});
        cpu_ranges.push_back({request.stencil_read_address, request.depth_image->stencil_size});
    }
    SynchronizeImages(cpu_ranges, true);
    SynchronizeImages(gpu_ranges, true);
    auto image_ranges = bindings.ImageRanges();
    for (const auto& color : request.colors)
        image_ranges.push_back({color.guest_address, color.guest_size});
    if (request.depth_image) {
        image_ranges.push_back(
            {request.depth_image->guest_address, request.depth_image->guest_size});
        image_ranges.push_back(
            {request.depth_image->stencil_addr, request.depth_image->stencil_size});
    }
    SynchronizeBuffers(image_ranges, true);
    bindings.PrepareImages();

    std::array<ImageCache::Image*, 8> colors{};
    u32 num_render_targets = 0;
    VAddr jitter_address{};
    for (u32 bound = 0; bound < request.colors.size(); ++bound) {
        const auto& info = request.colors[bound];
        const u32 slot = request.color_slots[bound];
        if (slot >= 8 || bound >= request.color_base_slice.size())
            throw std::runtime_error("Invalid D3D12 guest color slot mapping");
        bindings.ValidateTargetRange(
            info.guest_address, info.guest_size,
            request.blend.RenderTarget[request.blend.IndependentBlendEnable ? slot : 0]
                    .RenderTargetWriteMask != 0);
        colors[slot] = &images.GetImage(
            info, ConvertFormat(info.pixel_format), D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
            request.color_base_slice[bound], 0, logic_formats[slot] != DXGI_FORMAT_UNKNOWN,
            logic_masks[slot]);
        num_render_targets = std::max(num_render_targets, slot + 1);
        if (!jitter_address) {
            jitter_address = info.guest_address;
        }
    }
    ImageCache::Image* depth{};
    const bool depth_written =
        (request.depth.DepthEnable && request.depth.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL) ||
        request.clear_depth;
    const bool stencil_written =
        (request.depth.StencilEnable && request.depth.StencilWriteMask != 0) ||
        request.clear_stencil;
    if (request.depth_image) {
        if (request.depth_image->num_samples != request.samples)
            throw std::runtime_error("Guest raster and depth sample counts do not match");
        if (request.depth_image->guest_address) {
            bindings.ValidateTargetRange(request.depth_image->guest_address,
                                         request.depth_image->guest_size, depth_written);
        }
        if (request.depth_read_address) {
            bindings.ValidateTargetRange(request.depth_read_address,
                                         request.depth_image->guest_size, depth_written);
        }
        if (request.depth_image->stencil_addr) {
            bindings.ValidateTargetRange(request.depth_image->stencil_addr,
                                         request.depth_image->stencil_size, stencil_written);
        }
        if (request.stencil_read_address) {
            bindings.ValidateTargetRange(request.stencil_read_address,
                                         request.depth_image->stencil_size, stencil_written);
        }
        depth =
            &images.GetImage(*request.depth_image, ConvertFormat(request.depth_image->pixel_format),
                             D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, request.depth_base_layer);
        if (request.depth.StencilEnable && !depth->has_stencil) {
            throw std::runtime_error("D3D12 stencil test requires stencil plane");
        }
    }
    const auto synchronize_sample_positions = [&](ImageCache::Image* image) {
        if (!image || !image->msaa)
            return;
        const auto& previous = image->sample_positions;
        const auto& next = request.sample_positions;
        if (previous.size() == next.size() &&
            (previous.empty() || std::memcmp(previous.data(), next.data(),
                                             previous.size() * sizeof(D3D12_SAMPLE_POSITION)) == 0))
            return;
        const std::array<ResourceCache::Range, 2> ranges{
            {{image->info.guest_address, image->info.guest_size},
             {image->info.stencil_addr, image->info.stencil_size}}};
        SynchronizeImages(ranges, true);
    };
    for (auto* color : colors)
        synchronize_sample_positions(color);
    synchronize_sample_positions(depth);
    const auto memory_access = acquire_memory ? acquire_memory(access_ranges) : nullptr;
    for (u32 i = 0; i < request.color_clear_buffers.size(); ++i)
        MaterializeColorClearImpl(request.color_clear_buffers[i], request.colors[i]);
    resources.PrepareViews(bindings.GuestRanges());
    std::vector<ResourceCache::View> gpu_views;
    if (request.indirect || request.expansion) {
        resources.PrepareViews(gpu_ranges);
        gpu_views = resources.GetViews(gpu_ranges);
        for (const auto& view : gpu_views)
            if (view.buffer && std::ranges::any_of(view.segments, [&](const auto& segment) {
                    return segment.offset != view.offset + segment.address - view.guest_address;
                }))
                throw GpuDrawFallback{};
        gpu_commands.Initialize(device, shaders);
        gpu_commands.Prepare(request.indirect ? request.indirect->count : 0,
                             request.expansion ? request.expansion->output_count : 0);
    }
    u64 input_bytes{};
    const auto upload_contents = [&](std::span<const u8> contents) -> ID3D12Resource* {
        const u64 size = contents.size();
        if (!size || size > 16ULL * 1024 * 1024)
            throw std::runtime_error("D3D12 input buffer exceeds upload budget");
        if (input_bytes + ((size + 65535) & ~u64{65535}) > 64ULL * 1024 * 1024)
            throw std::runtime_error("D3D12 draw inputs exceed upload budget");
        auto resource = transfer_pool->Acquire(size, D3D12_HEAP_TYPE_UPLOAD);
        input_bytes += resource.AllocationBytes();
        void* mapped;
        const D3D12_RANGE empty{0, 0};
        winrt::check_hresult(resource->Map(0, &empty, &mapped));
        std::memcpy(mapped, contents.data(), contents.size());
        const D3D12_RANGE written{0, static_cast<SIZE_T>(size)};
        resource->Unmap(0, &written);
        auto* output = resource.Get();
        input_resources.push_back(std::move(resource));
        return output;
    };
    inputs.BeginBatch();
    struct InputUpload {
        ResourceCache::View view;
        bool copy;
    };
    std::vector<InputUpload> pending_inputs;
    const auto upload_input = [&](VAddr address, u64 size) -> D3D12_GPU_VIRTUAL_ADDRESS {
        if (!size || size > 16ULL * 1024 * 1024 || address > UINT64_MAX - size)
            throw std::runtime_error("Invalid D3D12 guest input buffer");
        bindings.ValidateInputRange(address, size);
        const ResourceCache::Range range{address, size};
        auto view = inputs.GetViews({&range, 1}).front();
        if (!view.buffer || std::ranges::any_of(view.segments, [&](const auto& part) {
                return part.offset != view.offset + part.address - address;
            })) {
            SynchronizeBuffers({&range, 1}, false);
            std::vector<u8> contents(size);
            read(address, contents);
            return upload_contents(contents)->GetGPUVirtualAddress();
        }
        const bool copy = resources.CopyResident(address, size, *view.buffer, view.offset, nullptr);
        if (!copy)
            SynchronizeBuffers({&range, 1}, false);
        const auto allocation = (size + 65535) & ~u64{65535};
        if (input_bytes + allocation > 64ULL * 1024 * 1024)
            throw std::runtime_error("D3D12 draw inputs exceed upload budget");
        input_bytes += allocation;
        pending_inputs.push_back({view, copy});
        return view.buffer->native->GetGPUVirtualAddress() + view.offset;
    };
    if (fetch.attributes.size() > D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT)
        throw std::runtime_error("Guest fetch shader exceeds D3D12 input slots");
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    std::vector<D3D12_VERTEX_BUFFER_VIEW> vertices;
    for (u32 index = 0; index < fetch.attributes.size(); ++index) {
        const auto& attribute = fetch.attributes[index];
        const auto sharp = vertex_sharps[index];
        const u64 size = u64(sharp.num_records) * std::max<u32>(1, sharp.stride);
        const auto buffer = upload_input(sharp.base_address, size);
        u32 step{};
        switch (attribute.GetStepRate()) {
        case Shader::Gcn::VertexAttribute::None:
            break;
        case Shader::Gcn::VertexAttribute::Plain:
            step = 1;
            break;
        case Shader::Gcn::VertexAttribute::OverStepRate0:
            step = request.vertex_runtime.sw.vs.step_rate_0;
            break;
        case Shader::Gcn::VertexAttribute::OverStepRate1:
            step = request.vertex_runtime.sw.vs.step_rate_1;
            break;
        }
        if (attribute.GetStepRate() != Shader::Gcn::VertexAttribute::None && !step)
            throw std::runtime_error(
                "Zero guest instance step rate requires shader input emulation");
        elements.push_back({"TEXCOORD", index,
                            ConvertFormat(Vulkan::LiverpoolToVK::SurfaceFormat(
                                sharp.GetDataFmt(), sharp.GetNumberFmt())),
                            index, 0,
                            step ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                 : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                            step});
        vertices.push_back({buffer, static_cast<UINT>(size), sharp.stride});
    }
    D3D12_INDEX_BUFFER_VIEW index_view{};
    const bool indexed = request.expansion || request.index_address || request.index_buffer_bound ||
                         !request.converted_indices.empty();
    if (request.expansion) {
        index_view = {gpu_commands.Indices()->GetGPUVirtualAddress(),
                      request.expansion->output_count * 4, DXGI_FORMAT_R32_UINT};
    } else if (!request.converted_indices.empty()) {
        const auto size = request.converted_indices.size() * sizeof(u32);
        auto* buffer =
            upload_contents({reinterpret_cast<const u8*>(request.converted_indices.data()), size});
        index_view = {buffer->GetGPUVirtualAddress(), static_cast<UINT>(size),
                      DXGI_FORMAT_R32_UINT};
    } else if ((request.index_address || request.index_buffer_bound) && !index_fetch.address) {
        if (request.index_format != DXGI_FORMAT_R16_UINT &&
            request.index_format != DXGI_FORMAT_R32_UINT)
            throw std::runtime_error("Invalid guest index format");
        const u64 size =
            u64(index_read_count) * (request.index_format == DXGI_FORMAT_R16_UINT ? 2 : 4);
        if (size > 16ULL * 1024 * 1024 || request.index_address > UINT64_MAX - size)
            throw std::runtime_error("Invalid D3D12 guest input buffer");
        bindings.ValidateInputRange(request.index_address, size);
        if (!size) {
            std::array<u32, 1> zero{};
            auto* buffer =
                upload_contents({reinterpret_cast<const u8*>(zero.data()), sizeof(zero)});
            index_view = {buffer->GetGPUVirtualAddress(), 0, request.index_format};
        } else if (request.index_swap == AmdGpu::IndexSwapMode::None) {
            index_view = {
                upload_input(request.index_address, size),
                static_cast<UINT>(std::min<u64>(size, u64(request.index_capacity) * index_bytes)),
                request.index_format};
        } else {
            const ResourceCache::Range range{request.index_address, size};
            SynchronizeBuffers({&range, 1}, false);
            const u64 unit = request.index_swap == AmdGpu::IndexSwapMode::Swap16 ? 2 : 4;
            std::vector<u8> contents((size + unit - 1) & ~(unit - 1));
            read(request.index_address, {contents.data(), size});
            SwapIndexBytes(contents, request.index_swap);
            auto* buffer = upload_contents(contents);
            index_view = {
                buffer->GetGPUVirtualAddress(),
                static_cast<UINT>(std::min<u64>(size, u64(request.index_capacity) * index_bytes)),
                request.index_format};
        }
    }
    D3D12_GPU_VIRTUAL_ADDRESS viewport_data{}, flat_data{};
    if (request.viewport_transform) {
        struct ViewportData {
            std::array<std::array<float, 4>, 16> xy;
            std::array<u32, 16> flags;
        } config{request.viewport_xy, request.viewport_flags};
        viewport_data = upload_contents({reinterpret_cast<const u8*>(&config), sizeof(config)})
                            ->GetGPUVirtualAddress();
    }
    if (!request.flat_vertices.empty())
        flat_data = upload_contents({reinterpret_cast<const u8*>(request.flat_vertices.data()),
                                     request.flat_vertices.size() * sizeof(u32)})
                        ->GetGPUVirtualAddress();
    std::vector<D3D12_ROOT_PARAMETER> parameters(2);
    parameters[0].ParameterType = parameters[1].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 20};
    parameters[1].Constants = {1, 0, 4};
    if (request.samples > 1) {
        D3D12_ROOT_PARAMETER masks{};
        masks.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        masks.Constants = {3, 0, 4};
        parameters.push_back(masks);
    }
    const auto add_table = [&](const auto& ranges) {
        const u32 slot = parameters.size();
        if (!ranges.empty()) {
            D3D12_ROOT_PARAMETER parameter{};
            parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameter.DescriptorTable = {static_cast<UINT>(ranges.size()), ranges.data()};
            parameters.push_back(parameter);
        }
        return slot;
    };
    const u32 resource_slot = add_table(bindings.ResourceRanges());
    const u32 sampler_slot = add_table(bindings.SamplerRanges());
    const u32 capture_slot = parameters.size();
    if (capture_geometry) {
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameter.Descriptor = {0, 100};
        parameters.push_back(parameter);
        parameter = {};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants = {0, 100,
                               request.faces.empty()           ? 1u
                               : request.flat_vertices.empty() ? 4u
                                                               : 5u};
        parameters.push_back(parameter);
    }
    const u32 stencil_slot = parameters.size();
    if (stencil_replay) {
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameter.Descriptor = {0, 101};
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters.push_back(parameter);
        parameter = {};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants = {0, 101, 4};
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters.push_back(parameter);
    }
    const u32 viewport_slot = parameters.size();
    if (request.viewport_transform) {
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameter.Descriptor = {0, 102};
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        parameters.push_back(parameter);
    }
    const u32 flat_slot = parameters.size();
    if (flat_data) {
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameter.Descriptor = {1, 100};
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        parameters.push_back(parameter);
    }
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = parameters.size();
    root_desc.pParameters = parameters.data();
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (capture_geometry)
        root_desc.Flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
    winrt::check_hresult(
        D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.VS = {vertex.binary->dxil.data(), vertex.binary->dxil.size()};
    if (hull) {
        desc.HS = {hull->binary->dxil.data(), hull->binary->dxil.size()};
        desc.DS = {domain->binary->dxil.data(), domain->binary->dxil.size()};
    }
    if (geometry)
        desc.GS = {geometry->binary->dxil.data(), geometry->binary->dxil.size()};
    else if (rectangle)
        desc.GS = {rectangle->dxil.data(), rectangle->dxil.size()};
    else if (point_fill)
        desc.GS = {point_fill->dxil.data(), point_fill->dxil.size()};
    else if (line_expand)
        desc.GS = {line_expand->dxil.data(), line_expand->dxil.size()};
    if (fragment)
        desc.PS = {fragment->binary->dxil.data(), fragment->binary->dxil.size()};
    else if (request.samples > 1)
        desc.PS = {images.MaskShader().data(), images.MaskShader().size()};
    desc.BlendState = blend;
    const auto logic_target = std::ranges::find_if(request.colors, [&](const auto& info) {
        const auto bound = &info - request.colors.data();
        return request.blend.RenderTarget[request.color_slots[bound]].LogicOpEnable != FALSE;
    });
    if (logic_target != request.colors.end()) {
        const auto bound = std::distance(request.colors.begin(), logic_target);
        const auto state = request.blend.RenderTarget[request.color_slots[bound]];
        for (const auto slot : request.color_slots)
            if (!request.blend.RenderTarget[slot].LogicOpEnable ||
                request.blend.RenderTarget[slot].LogicOp != state.LogicOp)
                throw std::runtime_error(
                    "Guest logic operation must be shared by all color targets");
        desc.BlendState.IndependentBlendEnable = FALSE;
        for (auto& target : desc.BlendState.RenderTarget) {
            target = state;
            target.BlendEnable = FALSE;
            target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }
    }
    desc.BlendState.AlphaToCoverageEnable = request.alpha_to_coverage;
    desc.RasterizerState = request.rasterizer;
    desc.DepthStencilState = request.depth;
    apply_empty_bounds(desc.DepthStencilState);
    desc.SampleMask = UINT_MAX;
    desc.SampleDesc.Count = request.samples;
    desc.RasterizerState.MultisampleEnable = request.samples > 1;
    desc.InputLayout = {elements.data(), static_cast<UINT>(elements.size())};
    desc.PrimitiveTopologyType = request.topology_type;
    desc.IBStripCutValue = request.strip_cut;
    desc.NumRenderTargets = num_render_targets;
    for (u32 i = 0; i < num_render_targets; ++i) {
        desc.RTVFormats[i] =
            colors[i] ? (colors[i]->logic_op ? DXGI_FORMAT_R32G32B32A32_UINT
                                             : ConvertFormat(colors[i]->info.pixel_format))
                      : DXGI_FORMAT_UNKNOWN;
    }
    desc.DSVFormat =
        depth ? (depth->has_stencil ? depth->dsv_format : ConvertFormat(depth->info.pixel_format))
              : DXGI_FORMAT_UNKNOWN;
    std::vector<ShaderCache::StageOutput> capture_outputs;
    std::vector<D3D12_SO_DECLARATION_ENTRY> capture_declarations;
    u32 capture_stride{}, capture_batches{1}, capture_records{1}, capture_primitive_vertices{};
    std::vector<D3D12_GRAPHICS_PIPELINE_STATE_DESC> face_descriptions;
    std::vector<std::shared_ptr<const ShaderBinary>> face_expansions, face_fragments;
    std::vector<Microsoft::WRL::ComPtr<ID3D12PipelineState>> face_pipelines;
    std::vector<Microsoft::WRL::ComPtr<ID3D12RootSignature>> face_roots;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC capture_desc{};
    std::shared_ptr<const ShaderBinary> capture_vertex, capture_expansion;
    D3D_PRIMITIVE_TOPOLOGY capture_topology{};
    if (capture_geometry) {
        capture_desc = desc;
        if (!geometry) {
            const auto& shader = face_capture_shader ? face_capture_shader : rectangle;
            capture_desc.GS = shader
                                  ? D3D12_SHADER_BYTECODE{shader->dxil.data(), shader->dxil.size()}
                                  : D3D12_SHADER_BYTECODE{};
        }
        capture_outputs = shaders.StageOutputs(*last_stage);
        auto capture_interface = shaders.OutputInterface(*last_stage);
        for (const auto& output : capture_outputs) {
            capture_declarations.push_back({0, output.semantic.c_str(), output.index, 0,
                                            static_cast<BYTE>(output.components), 0});
            capture_stride += output.components * 4;
        }
        auto replay_interface = capture_interface;
        const std::string primitive_semantic = "SV_PrimitiveID";
        if (const auto offset = replay_interface.find(primitive_semantic);
            offset != std::string::npos)
            replay_interface.replace(offset, primitive_semantic.size(), "TEXCOORD32");
        std::string replay =
            replay_interface + "ByteAddressBuffer Captured : register(t0,space100);\n";
        if (!request.faces.empty())
            replay += "cbuffer CaptureDraw : register(b0,space100) { uint capture_batch; uint "
                      "polygon_mask; uint first_vertex; uint logical_id; " +
                      std::string(flat_data ? "uint flat_count; " : "") + "};\n";
        if (flat_data)
            replay += "ByteAddressBuffer FlatIndices : register(t1,space100);\n";
        if (request.viewport_transform)
            replay += "cbuffer ViewportConfig : register(b0,space102) { float4 ViewportXY[16]; "
                      "uint4 ViewportFlags[4]; };\n";
        replay += "SPIRV_Cross_Output main(uint id : SV_VertexID) { SPIRV_Cross_Output value;\n";
        if (!request.faces.empty())
            replay += "id += first_vertex;\n";
        if (request.flat_last) {
            replay += "uint group = id / " + std::to_string(replay_vertices) +
                      "; uint flat_id = group * " + std::to_string(replay_vertices) + " + " +
                      std::to_string(replay_vertices - 1) + ";\n";
            if (flat_data) {
                replay += "flat_id = FlatIndices.Load((group % flat_count) * 4) + (group / "
                          "flat_count) * flat_count * 3;\n";
            } else if (geometry) {
                u32 order_offset{};
                for (const auto& field : capture_outputs) {
                    if (field.semantic == "TEXCOORD" && field.index == 31)
                        break;
                    order_offset += field.components * 4;
                }
                if (order_offset == capture_stride)
                    throw std::runtime_error(
                        "Missing geometry emission order for flat-last replay");
                replay += "uint best_order = 0; for(uint lane=0;lane<" +
                          std::to_string(replay_vertices) + ";++lane) { uint candidate = group * " +
                          std::to_string(replay_vertices) +
                          " + lane; uint order = Captured.Load(candidate * " +
                          std::to_string(capture_stride) + " + " + std::to_string(order_offset) +
                          "); if(lane==0 || order > best_order) { best_order=order; "
                          "flat_id=candidate; } }\n";
            }
        }
        u32 offset{};
        for (u32 i = 0; i < capture_outputs.size(); ++i) {
            const auto& output = capture_outputs[i];
            const bool flat =
                request.flat_last && output.semantic == "TEXCOORD" &&
                std::ranges::any_of(std::span{request.fragment_runtime.hw.fs.inputs.data(),
                                              request.fragment_runtime.hw.fs.num_inputs},
                                    [&](const auto& input) {
                                        return input.is_flat && input.param_index == output.index;
                                    });
            const auto load = std::string("Captured.Load") +
                              (output.components > 1 ? std::to_string(output.components) : "") +
                              "(" + std::string(flat ? "flat_id" : "id") + " * " +
                              std::to_string(capture_stride) + " + " + std::to_string(offset) + ")";
            replay += "value.p" + std::to_string(i) + " = " +
                      (output.integer ? output.signed_integer ? "asint(" : "(" : "asfloat(") +
                      load + ");\n";
            if (capture_primitive_id && !domain && !request.face_masks.empty() &&
                output.semantic == "SV_PrimitiveID")
                replay += "value.p" + std::to_string(i) + " = logical_id;\n";
            offset += output.components * 4;
        }
        if (request.viewport_transform) {
            const auto field = [&](std::string_view semantic) {
                for (u32 i = 0; i < capture_outputs.size(); ++i)
                    if (capture_outputs[i].semantic == semantic)
                        return std::string("value.p") + std::to_string(i);
                return std::string{};
            };
            const auto position = field("SV_Position");
            const auto viewport = field("SV_ViewportArrayIndex");
            if (position.empty())
                throw std::runtime_error("Viewport replay requires a position export");
            replay += "uint viewport = " + (viewport.empty() ? std::string("0") : viewport) + ";\n";
            replay += "if(viewport < 16) { uint flags = ViewportFlags[viewport/4][viewport%4];\n";
            replay += "if ((flags & 4) != 0) { float2 window = " + position +
                      ".xy * ViewportXY[viewport].xy + ViewportXY[viewport].zw; " + position +
                      ".xy = float2(window.x/8192.0-1.0,1.0-window.y/8192.0); } else { "
                      "if((flags&1)!=0) " +
                      position + ".x = -" + position + ".x; if((flags&2)!=0) " + position +
                      ".y = -" + position + ".y; } }\n";
        }
        replay += "return value; }\n";
        capture_vertex = shaders.CompileHelperHlsl(replay, Shader::SwStage::Vertex);
        capture_expansion = point_fill ? point_fill : line_expand;
        const auto primitive = output_primitive;
        const u32 vertices_per_primitive =
            primitive == AmdGpu::GsOutputPrimitiveType::TriangleStrip ? 3
            : primitive == AmdGpu::GsOutputPrimitiveType::LineStrip   ? 2
                                                                      : 1;
        capture_topology = vertices_per_primitive == 3   ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
                           : vertices_per_primitive == 2 ? D3D_PRIMITIVE_TOPOLOGY_LINELIST
                                                         : D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        u64 invocations = request.hull ? u64(request.count) * 8192
                                       : GsInvocations(request.topology, request.count);
        u64 bytes = std::max<u64>(invocations, 1);
        for (const u64 factor : {u64(request.instances),
                                 u64(geometry ? request.geometry_runtime.hw.gs.num_invocations : 1),
                                 u64(geometry    ? request.geometry_runtime.hw.gs.output_vertices
                                     : rectangle ? 2
                                                 : 1),
                                 u64(vertices_per_primitive), u64(capture_stride)}) {
            if (factor && bytes > (32ULL * 1024 * 1024) / factor)
                throw std::runtime_error("D3D12 geometry capture exceeds memory budget");
            bytes *= factor;
        }
        gpu_commands.Initialize(device, shaders);
        if (!request.indirect && !request.expansion)
            gpu_commands.Prepare(1, 0);
        if ((request.line_expand ||
             std::ranges::any_of(request.faces,
                                 [](const auto& face) { return face.line_expand; })) &&
            request.line_pattern_scale > 0 && request.line_pattern != u16(0xffff)) {
            const u32 windows = std::max(1u, std::min(128u, 1024u / capture_stride));
            const u32 groups = 32 / (vertices_per_primitive == 3 ? 3 : 1);
            double max_length{};
            for (u32 index = 0; index < request.line_viewport_dims.size(); index += 2)
                max_length =
                    std::max(max_length, std::hypot(double(request.line_viewport_dims[index]),
                                                    double(request.line_viewport_dims[index + 1])));
            const double bit_length =
                double(request.line_pattern_scale) * (u32(request.line_pattern_repeat) + 1);
            const double window_count = std::ceil(max_length / bit_length) + 1;
            if (!std::isfinite(window_count) || window_count > UINT32_MAX - groups * windows)
                throw std::runtime_error(
                    "Line stipple subdivision exceeds the addressable draw range");
            capture_batches =
                std::max(1u, (u32(window_count) + groups * windows - 1) / (groups * windows));
        }
        capture_records = capture_batches;
        if (!request.faces.empty()) {
            if (request.faces.size() > 6)
                throw std::runtime_error("Ordered raster replay exceeds the pass limit");
            capture_primitive_vertices = vertices_per_primitive;
            capture_records =
                u32(std::max<u64>(1, bytes / (u64(capture_stride) * vertices_per_primitive)));
        }
        gpu_commands.PrepareGeometry(std::max<u64>(bytes, capture_stride), capture_records);
        capture_desc.StreamOutput = {capture_declarations.data(), u32(capture_declarations.size()),
                                     &capture_stride, 1, D3D12_SO_NO_RASTERIZED_STREAM};
        capture_desc.PS = {};
        capture_desc.NumRenderTargets = 0;
        std::fill(std::begin(capture_desc.RTVFormats), std::end(capture_desc.RTVFormats),
                  DXGI_FORMAT_UNKNOWN);
        capture_desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        capture_desc.DepthStencilState.DepthEnable = FALSE;
        capture_desc.DepthStencilState.StencilEnable = FALSE;
        capture_desc.SampleDesc.Count = 1;
        capture_desc.RasterizerState.MultisampleEnable = FALSE;
        desc.VS = {capture_vertex->dxil.data(), capture_vertex->dxil.size()};
        desc.HS = desc.DS = {};
        desc.GS = capture_expansion ? D3D12_SHADER_BYTECODE{capture_expansion->dxil.data(),
                                                            capture_expansion->dxil.size()}
                                    : D3D12_SHADER_BYTECODE{};
        desc.InputLayout = {};
        desc.PrimitiveTopologyType =
            vertices_per_primitive == 3   ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE
            : vertices_per_primitive == 2 ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                          : D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
        for (const auto& face : request.faces) {
            auto face_desc = desc;
            face_desc.RasterizerState = face.rasterizer;
            face_desc.DepthStencilState = face.depth;
            if (face.stencil) {
                for (auto* state : {&face_desc.DepthStencilState.FrontFace,
                                    &face_desc.DepthStencilState.BackFace})
                    state->StencilFailOp = state->StencilDepthFailOp = state->StencilPassOp =
                        D3D12_STENCIL_OP_KEEP;
            }
            apply_empty_bounds(face_desc.DepthStencilState);
            if (face.color_masks)
                for (u32 target = 0; target < 8; ++target)
                    face_desc.BlendState.RenderTarget[target].RenderTargetWriteMask =
                        (*face.color_masks)[target];
            if (face.color_masks && logic_target != request.colors.end()) {
                std::optional<u8> shared_mask;
                for (const auto slot : request.color_slots) {
                    const auto format = GetLogicOpFormat(logic_formats[slot]);
                    u32 packed{};
                    u8 mask{};
                    for (u32 component = 0; component < 4; ++component)
                        if (format.bits[component] && (logic_masks[slot] & (1u << component))) {
                            if ((*face.color_masks)[slot] & (1u << component))
                                mask |= u8(1u << packed);
                            ++packed;
                        }
                    if (shared_mask && *shared_mask != mask)
                        throw std::runtime_error(
                            "Ordered logic operations require matching packed target masks");
                    shared_mask = mask;
                }
                for (auto& target : face_desc.BlendState.RenderTarget)
                    target.RenderTargetWriteMask = shared_mask.value_or(0);
            }
            if ((face.point_fill || face.line_expand) && fragment &&
                fragment->info.loads.Get(Shader::IR::Attribute::IsFrontFace)) {
                auto shader = shaders.CompileFaceFragment(*fragment->binary, face.keep_front);
                face_desc.PS = {shader->dxil.data(), shader->dxil.size()};
                face_fragments.push_back(std::move(shader));
            }
            std::shared_ptr<const ShaderBinary> helper;
            if (face.point_fill)
                helper = shaders.CompilePointFill(expansion_source, face.keep_front,
                                                  face.rasterizer.FrontCounterClockwise != 0, true,
                                                  &request.fragment_runtime.hw.fs, true);
            else if (face.line_expand)
                helper = shaders.CompileLineExpand(
                    expansion_source, request.fragment_runtime.hw.fs, face.keep_front,
                    face.rasterizer.FrontCounterClockwise != 0,
                    D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, false, request.line_width,
                    request.line_pattern, request.line_pattern_repeat, request.line_pattern_msb,
                    request.line_pattern_scale, request.line_viewport_dims, true, true, true,
                    line_linear, line_flat, face.rasterizer.DepthClipEnable != 0);
            if (!helper && replay_interface != capture_interface) {
                std::string identity = capture_interface + "\n[maxvertexcount(" +
                                       std::to_string(replay_vertices) + ")] void main(" +
                                       replay_input + " SPIRV_Cross_Output input[" +
                                       std::to_string(replay_vertices) + "], inout " +
                                       replay_stream + "<SPIRV_Cross_Output> stream) { ";
                for (u32 vertex = 0; vertex < replay_vertices; ++vertex)
                    identity += "stream.Append(input[" + std::to_string(vertex) + "]); ";
                identity += "}\n";
                helper = shaders.CompileHelperHlsl(identity, Shader::SwStage::Geometry);
            }
            face_desc.GS = helper ? D3D12_SHADER_BYTECODE{helper->dxil.data(), helper->dxil.size()}
                                  : D3D12_SHADER_BYTECODE{};
            face_expansions.push_back(helper);
            face_descriptions.push_back(face_desc);
        }
    }
    if (const char* dump = std::getenv("GAME_SHADER_DUMP"); dump) {
        std::ofstream vs{std::string(dump) + "\\draw-vs.hlsl", std::ios::binary};
        vs << vertex.binary->hlsl;
        if (fragment) {
            std::ofstream ps{std::string(dump) + "\\draw-ps.hlsl", std::ios::binary};
            ps << fragment->binary->hlsl;
        }
        if (geometry) {
            std::ofstream gs{std::string(dump) + "\\draw-gs.hlsl", std::ios::binary};
            gs << geometry->binary->hlsl;
        }
        if (hull) {
            std::ofstream hs{std::string(dump) + "\\draw-hull.hlsl", std::ios::binary};
            hs << hull->binary->hlsl;
            std::ofstream ds{std::string(dump) + "\\draw-domain.hlsl", std::ios::binary};
            ds << domain->binary->hlsl;
        }
    }
    const bool first_pipeline = pipelines.lower_bound(std::vector<u8>{1}) == pipelines.end();
    if (first_pipeline)
        LOG_INFO(Core, "[UWP stage] first graphics pipeline creation begin");
    auto& cached = GetPipeline(
        PipelineKey(desc, blob.Get(), request.depth_bounds), blob.Get(),
        [&](auto* root, auto** state, D3D12_CACHED_PIPELINE_STATE cached) {
            desc.pRootSignature = root;
            desc.CachedPSO = cached;
            const HRESULT created = ProfileSample(profile, "pso_create", 128, [&] {
                return CreateGraphicsPipeline(device.Device(), desc, request.depth_bounds, state);
            });
            return created;
        });
    if (first_pipeline)
        LOG_INFO(Core, "[UWP stage] first graphics pipeline creation completed");
    auto pipeline = cached.state;
    auto root = cached.root;
    for (auto& face_desc : face_descriptions) {
        auto& face =
            GetPipeline(PipelineKey(face_desc, blob.Get(), request.depth_bounds), blob.Get(),
                        [&](auto* signature, auto** state, D3D12_CACHED_PIPELINE_STATE saved) {
                            face_desc.pRootSignature = signature;
                            face_desc.CachedPSO = saved;
                            return CreateGraphicsPipeline(device.Device(), face_desc,
                                                          request.depth_bounds, state);
                        });
        face_pipelines.push_back(face.state);
        face_roots.push_back(face.root);
    }

    std::vector<std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 3>> stencil_pipelines(
        request.faces.size());
    D3D12_CPU_DESCRIPTOR_HANDLE stencil_view{};
    if (stencil_replay) {
        if (!depth || !depth->has_stencil || depth->native_layers != 1 ||
            depth->info.resources.levels != 1)
            throw std::runtime_error("Ordered stencil updates require a single stencil layer");
        gpu_commands.PrepareStencil(depth->Native()->GetDesc(), depth->stencil_staging_bytes);
        const auto shader = shaders.CompileHelperHlsl(StencilShader, Shader::SwStage::Fragment);
        for (u32 face = 0; face < request.faces.size(); ++face) {
            if (!request.faces[face].stencil || !request.faces[face].depth.StencilWriteMask)
                continue;
            if (!request.faces[face].depth.StencilEnable)
                throw std::runtime_error("Ordered stencil state requires enabled stencil testing");
            const auto& state = *request.faces[face].stencil;
            if (state.fail > 15 || state.depth_fail > 15 || state.pass > 15)
                throw std::runtime_error("Invalid guest stencil operation");
            for (u32 outcome = 0; outcome < 3; ++outcome) {
                auto helper = face_descriptions[face];
                helper.PS = {shader->dxil.data(), shader->dxil.size()};
                helper.BlendState = {};
                for (auto& target : helper.BlendState.RenderTarget)
                    target = {FALSE,
                              FALSE,
                              D3D12_BLEND_ONE,
                              D3D12_BLEND_ZERO,
                              D3D12_BLEND_OP_ADD,
                              D3D12_BLEND_ONE,
                              D3D12_BLEND_ZERO,
                              D3D12_BLEND_OP_ADD,
                              D3D12_LOGIC_OP_NOOP,
                              0};
                helper.DepthStencilState = request.faces[face].depth;
                auto& test = helper.DepthStencilState;
                test.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
                test.StencilWriteMask = 0;
                for (auto* side : {&test.FrontFace, &test.BackFace}) {
                    side->StencilFailOp = side->StencilDepthFailOp = side->StencilPassOp =
                        D3D12_STENCIL_OP_KEEP;
                    if (outcome == 0)
                        side->StencilFunc = D3D12_COMPARISON_FUNC(9 - u32(side->StencilFunc));
                }
                if (outcome == 0)
                    test.DepthEnable = FALSE;
                else if (outcome == 1)
                    test.DepthFunc = D3D12_COMPARISON_FUNC(9 - u32(test.DepthFunc));
                apply_empty_bounds(test);
                auto& saved = GetPipeline(
                    PipelineKey(helper, blob.Get(), request.depth_bounds), blob.Get(),
                    [&](auto* signature, auto** native, D3D12_CACHED_PIPELINE_STATE cache) {
                        helper.pRootSignature = signature;
                        helper.CachedPSO = cache;
                        return CreateGraphicsPipeline(device.Device(), helper, request.depth_bounds,
                                                      native);
                    });
                stencil_pipelines[face][outcome] = saved.state;
                face_pipelines.push_back(saved.state);
                face_roots.push_back(saved.root);
            }
        }
    }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> capture_root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> capture_pipeline;
    if (capture_geometry) {
        auto& captured = GetPipeline(
            PipelineKey(capture_desc, blob.Get()), blob.Get(),
            [&](auto* capture_root, auto** state, D3D12_CACHED_PIPELINE_STATE saved) {
                capture_desc.pRootSignature = capture_root;
                capture_desc.CachedPSO = saved;
                return device.Device()->CreateGraphicsPipelineState(
                    &capture_desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(state));
            });
        capture_pipeline = captured.state;
        capture_root = captured.root;
    }
    auto& rtv = color_descriptors;
    auto& dsv = depth_descriptors;
    device.CheckResult(allocator->Reset(), L"Native graphics allocator Reset");
    if (first_pipeline)
        LOG_INFO(Core, "[UWP stage] first graphics allocator reset completed");
    device.CheckResult(commands->Reset(allocator.Get(), nullptr),
                       L"Native graphics command list Reset");
    buffer_commands_open = true;
    struct BufferScope {
        bool& open;
        ~BufferScope() {
            open = false;
        }
    } buffer_scope{buffer_commands_open};
    for (const auto& input : pending_inputs) {
        auto& buffer = *input.view.buffer;
        if (!buffer.resident || !buffer.dirty.empty()) {
            if (input.copy && resources.CopyResident(input.view.guest_address, input.view.size,
                                                     buffer, input.view.offset, commands.Get())) {
                buffer.resident = true;
                buffer.dirty.clear();
                buffer.snapshot.clear();
            } else {
                inputs.Upload(buffer, commands.Get());
            }
        }
        inputs.Barrier(buffer, commands.Get(), D3D12_RESOURCE_STATE_GENERIC_READ);
    }
    if (request.indirect || request.expansion) {
        for (const auto& view : gpu_views)
            if (view.buffer)
                resources.Upload(*view.buffer, commands.Get());
        if (request.indirect)
            gpu_commands.RecordArguments(commands.Get(), *request.indirect, gpu_views[0],
                                         gpu_views[1]);
        else
            gpu_commands.RecordIndices(commands.Get(), *request.expansion,
                                       gpu_views.empty() ? ResourceCache::View{} : gpu_views[0],
                                       (request.index_address || request.index_buffer_bound)
                                           ? (request.index_format == DXGI_FORMAT_R16_UINT ? 2 : 4)
                                           : 0,
                                       u32(request.index_swap), request.index_capacity,
                                       u32(index_read_count));
    }
    BeginGpuTiming();
    if (first_pipeline)
        LOG_INFO(Core, "[UWP stage] first graphics command list reset completed");
    RecordingGuard recording{*this};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> color_handles{};
    for (u32 i = 0; i < num_render_targets; ++i) {
        if (!colors[i]) {
            D3D12_RENDER_TARGET_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R32G32B32A32_UINT;
            view.ViewDimension = request.samples > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS
                                                     : D3D12_RTV_DIMENSION_TEXTURE2D;
            color_handles[i] = rtv.Cpu(i);
            device.Device()->CreateRenderTargetView(nullptr, &view, color_handles[i]);
            continue;
        }
        colors[i]->sample_positions = request.sample_positions;
        ProfileSample(profile, "color_upload", 128,
                      [&] { images.Upload(*colors[i], commands.Get()); });
        images.Barrier(*colors[i], commands.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        color_handles[i] = rtv.Cpu(i);
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = desc.RTVFormats[i];
        if (request.samples > 1) {
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
            view.Texture2DMSArray = {0, colors[i]->native_layers};
        } else {
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray = {0, 0, colors[i]->native_layers, 0};
        }
        device.Device()->CreateRenderTargetView(colors[i]->Native(), &view, color_handles[i]);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE depth_view{};
    if (depth) {
        depth->sample_positions = request.sample_positions;
        images.Upload(*depth, commands.Get(), request.depth_read_address,
                      request.stencil_read_address);
        images.Barrier(*depth, commands.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        const bool clear_depth = request.clear_depth && depth->has_depth;
        const bool clear_stencil = request.clear_stencil && depth->has_stencil;
        if (clear_depth && (request.dsv_flags & D3D12_DSV_FLAG_READ_ONLY_DEPTH)) {
            throw std::runtime_error("D3D12 guest depth clear with read-only view");
        }
        if (clear_stencil && (request.dsv_flags & D3D12_DSV_FLAG_READ_ONLY_STENCIL)) {
            throw std::runtime_error("D3D12 guest stencil clear with read-only view");
        }
        depth_view = dsv.Cpu(0);
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = desc.DSVFormat;
        view.Flags = request.dsv_flags;
        if (request.samples > 1) {
            view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
            view.Texture2DMSArray = {0, depth->native_layers};
        } else {
            view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray = {0, 0, depth->native_layers};
        }
        device.Device()->CreateDepthStencilView(depth->Native(), &view, depth_view);
        if (clear_depth || clear_stencil) {
            const D3D12_CLEAR_FLAGS flags =
                (clear_depth ? D3D12_CLEAR_FLAG_DEPTH : D3D12_CLEAR_FLAGS(0)) |
                (clear_stencil ? D3D12_CLEAR_FLAG_STENCIL : D3D12_CLEAR_FLAGS(0));
            commands->ClearDepthStencilView(depth_view, flags, request.depth_clear,
                                            static_cast<UINT8>(request.stencil_clear), 0, nullptr);
        }
    }
    if (stencil_replay) {
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = depth->dsv_format;
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray = {0, 0, 1};
        view.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL;
        stencil_view = dsv.Cpu(1);
        device.Device()->CreateDepthStencilView(gpu_commands.StencilSnapshot(), &view,
                                                stencil_view);
    }
    Shader::PushData push = request.push;
    bindings.Upload(commands.Get(), push, request.clip_planes, request.border_colors, read, gds);
    if (capture_geometry) {
        const auto output = gpu_commands.BeginGeometry(commands.Get());
        commands->SOSetTargets(0, 1, &output);
    }
    commands->SetGraphicsRootSignature(root.Get());
    commands->SetPipelineState(capture_geometry ? capture_pipeline.Get() : pipeline.Get());
    if (!capture_geometry)
        SetSamplePositions(commands.Get(), request.samples, request.sample_positions);
    std::array<u32, 20> constants{};
    std::memcpy(constants.data(), &push, sizeof(push));
    commands->SetGraphicsRoot32BitConstants(0, constants.size(), constants.data(), 0);
    const std::array<u32, 4> base{
        static_cast<u32>(indexed ? request.base_vertex : request.first_vertex),
        request.first_instance, 0, 0};
    commands->SetGraphicsRoot32BitConstants(1, base.size(), base.data(), 0);
    if (request.samples > 1)
        commands->SetGraphicsRoot32BitConstants(2, request.sample_masks.size(),
                                                request.sample_masks.data(), 0);
    if (depth_bounds_commands)
        depth_bounds_commands->OMSetDepthBounds(empty_bounds ? 0.f : bounds_min,
                                                empty_bounds ? 1.f : bounds_max);
    bindings.Bind(commands.Get(), true, resource_slot, sampler_slot);
    commands->IASetPrimitiveTopology(request.topology);
    commands->IASetVertexBuffers(0, vertices.size(), vertices.data());
    std::array<D3D12_VIEWPORT, AmdGpu::NUM_VIEWPORTS> viewports = request.viewports;
    for (u32 index = 0; index < request.viewport_count; ++index)
        viewports[index] = NativeTemporalJitter().Apply(jitter_address, viewports[index]);
    commands->RSSetViewports(request.viewport_count, viewports.data());
    commands->RSSetScissorRects(request.viewport_count, request.scissors.data());
    commands->OMSetRenderTargets(num_render_targets,
                                 num_render_targets ? color_handles.data() : nullptr, FALSE,
                                 depth ? &depth_view : nullptr);
    if (capture_geometry)
        commands->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    commands->OMSetBlendFactor(request.blend_factor.data());
    commands->OMSetStencilRef(request.stencil_ref);
    if (!capture_geometry)
        BeginOcclusion();
    if (index_fetch.address) {
        commands->DrawInstanced(request.count, request.instances, 0, request.first_instance);
    } else if (indexed) {
        commands->IASetIndexBuffer(&index_view);
        if (request.indirect)
            gpu_commands.Execute(commands.Get(), root.Get(), true, request.indirect->count);
        else
            commands->DrawIndexedInstanced(request.count, request.instances, 0, request.base_vertex,
                                           request.first_instance);
    } else if (request.indirect)
        gpu_commands.Execute(commands.Get(), root.Get(), false, request.indirect->count);
    else
        commands->DrawInstanced(request.count, request.instances, request.first_vertex,
                                request.first_instance);
    if (!capture_geometry)
        EndOcclusion();
    if (capture_geometry) {
        gpu_commands.EndGeometry(commands.Get(), capture_stride, capture_records,
                                 capture_primitive_vertices);
        if (!request.faces.empty()) {
            auto readback = transfer_pool->Acquire(sizeof(u32), D3D12_HEAP_TYPE_READBACK);
            gpu_commands.CopyGeometryCounter(commands.Get(), readback.Get());
            winrt::check_hresult(commands->Close());
            if (coordinator)
                coordinator->SubmitReady();
            const u64 capture_fence = device.Submit(commands.Get());
            ProfileSample(profile, "face_capture_wait", 128, [&] { device.Wait(capture_fence); });
            void* mapped{};
            D3D12_RANGE range{0, sizeof(u32)};
            winrt::check_hresult(readback->Map(0, &range, &mapped));
            u32 bytes{};
            std::memcpy(&bytes, mapped, sizeof(bytes));
            D3D12_RANGE empty{};
            readback->Unmap(0, &empty);
            transfer_pool->Retire(std::move(readback), capture_fence);
            capture_records =
                std::min(capture_records, bytes / (capture_stride * capture_primitive_vertices));
            winrt::check_hresult(allocator->Reset());
            winrt::check_hresult(commands->Reset(allocator.Get(), nullptr));
            commands->SetGraphicsRootSignature(root.Get());
            commands->SetGraphicsRoot32BitConstants(0, constants.size(), constants.data(), 0);
            commands->SetGraphicsRoot32BitConstants(1, base.size(), base.data(), 0);
            if (request.samples > 1)
                commands->SetGraphicsRoot32BitConstants(2, request.sample_masks.size(),
                                                        request.sample_masks.data(), 0);
            if (depth_bounds_commands)
                depth_bounds_commands->OMSetDepthBounds(empty_bounds ? 0.f : bounds_min,
                                                        empty_bounds ? 1.f : bounds_max);
            bindings.Bind(commands.Get(), true, resource_slot, sampler_slot);
            commands->RSSetViewports(request.viewport_count, viewports.data());
            commands->RSSetScissorRects(request.viewport_count, request.scissors.data());
            commands->OMSetBlendFactor(request.blend_factor.data());
        }
        SetSamplePositions(commands.Get(), request.samples, request.sample_positions);
        commands->OMSetRenderTargets(num_render_targets,
                                     num_render_targets ? color_handles.data() : nullptr, FALSE,
                                     depth ? &depth_view : nullptr);
        commands->SetGraphicsRootSignature(root.Get());
        commands->SetPipelineState(pipeline.Get());
        commands->SetGraphicsRootShaderResourceView(
            capture_slot, gpu_commands.GeometryVertices()->GetGPUVirtualAddress());
        if (viewport_data)
            commands->SetGraphicsRootConstantBufferView(viewport_slot, viewport_data);
        if (flat_data) {
            commands->SetGraphicsRootShaderResourceView(flat_slot, flat_data);
            commands->SetGraphicsRoot32BitConstant(capture_slot + 1,
                                                   u32(request.flat_vertices.size()), 4);
        }
        commands->IASetPrimitiveTopology(capture_topology);
        commands->IASetVertexBuffers(0, 0, nullptr);
        if (request.faces.empty()) {
            BeginOcclusion();
            gpu_commands.ExecuteGeometry(commands.Get(), root.Get(), capture_slot + 1,
                                         capture_batches);
            EndOcclusion();
        } else {
            std::vector<u32> logical_ids;
            u32 logical{};
            for (u32 index = 0; index < request.face_masks.size(); ++index) {
                if (index && (request.face_masks[index] >> 3) == 7)
                    ++logical;
                logical_ids.push_back(logical);
            }
            for (u32 primitive = 0; primitive < capture_records; ++primitive)
                for (u32 face = 0; face < request.faces.size(); ++face) {
                    commands->SetPipelineState(face_pipelines[face].Get());
                    commands->OMSetStencilRef(request.faces[face].stencil_ref);
                    const u32 mask =
                        request.face_masks.empty()
                            ? (request.rectangles ? (primitive % 2 ? 19u : 61u) : 0x3f)
                            : request.face_masks[primitive % request.face_masks.size()];
                    commands->SetGraphicsRoot32BitConstant(capture_slot + 1, mask, 1);
                    commands->SetGraphicsRoot32BitConstant(
                        capture_slot + 1, primitive * capture_primitive_vertices, 2);
                    commands->SetGraphicsRoot32BitConstant(
                        capture_slot + 1,
                        logical_ids.empty() ? primitive
                                            : logical_ids[primitive % logical_ids.size()],
                        3);
                    const bool update_stencil = stencil_replay && request.faces[face].stencil &&
                                                request.faces[face].depth.StencilWriteMask;
                    if (update_stencil) {
                        images.Barrier(*depth, commands.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
                        gpu_commands.BeginStencil(commands.Get(), depth->Native(), 1,
                                                  depth->stencil_footprints[0]);
                        images.Barrier(*depth, commands.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
                    }
                    const u32 batches = request.faces[face].line_expand ? capture_batches : 1;
                    BeginOcclusion(request.faces[face].count_occlusion);
                    for (u32 batch = 0; batch < batches; ++batch) {
                        gpu_commands.ExecuteGeometryPrimitive(commands.Get(), capture_slot + 1,
                                                              primitive, batch);
                        D3D12_RESOURCE_BARRIER ordered{};
                        ordered.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        commands->ResourceBarrier(1, &ordered);
                    }
                    EndOcclusion();
                    if (update_stencil) {
                        commands->OMSetRenderTargets(
                            num_render_targets, num_render_targets ? color_handles.data() : nullptr,
                            FALSE, &stencil_view);
                        commands->SetGraphicsRootUnorderedAccessView(
                            stencil_slot, gpu_commands.StencilValues()->GetGPUVirtualAddress());
                        const auto& guest = *request.faces[face].stencil;
                        const std::array<u32, 3> operations{guest.fail, guest.depth_fail,
                                                            guest.pass};
                        for (u32 outcome = 0; outcome < 3; ++outcome) {
                            if (!operations[outcome] ||
                                (outcome == 1 && !request.faces[face].depth.DepthEnable))
                                continue;
                            commands->SetPipelineState(stencil_pipelines[face][outcome].Get());
                            const std::array<u32, 4> config{
                                depth->stencil_footprints[0].Footprint.RowPitch,
                                operations[outcome], u32(guest.test_ref) | (u32(guest.op_ref) << 8),
                                request.faces[face].depth.StencilWriteMask};
                            commands->SetGraphicsRoot32BitConstants(stencil_slot + 1, config.size(),
                                                                    config.data(), 0);
                            gpu_commands.ExecuteGeometryPrimitive(commands.Get(), capture_slot + 1,
                                                                  primitive, 0);
                            D3D12_RESOURCE_BARRIER barrier{};
                            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                            barrier.UAV.pResource = gpu_commands.StencilValues();
                            commands->ResourceBarrier(1, &barrier);
                        }
                        images.Barrier(*depth, commands.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
                        gpu_commands.EndStencil(commands.Get(), depth->Native(), 1,
                                                depth->stencil_footprints[0]);
                        images.Barrier(*depth, commands.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
                        commands->OMSetRenderTargets(
                            num_render_targets, num_render_targets ? color_handles.data() : nullptr,
                            FALSE, &depth_view);
                    }
                }
        }
    }
    if (request.indirect)
        ++(coordinator ? coordinator : this)->async_statistics.gpu_draw_batches;
    if (request.expansion)
        ++(coordinator ? coordinator : this)->async_statistics.gpu_index_expansions;
    for (u32 slot = 0; slot < colors.size(); ++slot)
        if (colors[slot])
            colors[slot]->written |=
                request.blend.RenderTarget[request.blend.IndependentBlendEnable ? slot : 0]
                    .RenderTargetWriteMask != 0;
    if (depth) {
        depth->written |= depth_written && depth->has_depth;
        depth->stencil_written |= stencil_written && depth->has_stencil;
    }
    resources.PrepareDownload(commands.Get());
    images.PrepareDownloads(commands.Get());
    EndGpuTiming();
    winrt::check_hresult(commands->Close());
    if (first_pipeline)
        LOG_INFO(Core, "[UWP stage] first graphics submission begin");
    std::shared_ptr<void> submission_lease = memory_access;
    if (capture_geometry) {
        struct CaptureLease {
            std::shared_ptr<void> memory;
            Microsoft::WRL::ComPtr<ID3D12PipelineState> raster, capture;
            Microsoft::WRL::ComPtr<ID3D12RootSignature> raster_root, capture_root;
            std::vector<Microsoft::WRL::ComPtr<ID3D12PipelineState>> faces;
            std::vector<Microsoft::WRL::ComPtr<ID3D12RootSignature>> face_roots;
        };
        submission_lease = std::make_shared<CaptureLease>(
            CaptureLease{memory_access, pipeline, capture_pipeline, root, capture_root,
                         std::move(face_pipelines), std::move(face_roots)});
    }
    FinishRecording(std::move(submission_lease), bindings.GdsBuffer(), bindings.CpuRanges(0, true),
                    bindings.StreamOutCounter());
    recording.completed = true;
}
} // namespace D3D12
