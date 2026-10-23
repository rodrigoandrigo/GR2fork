#include "video_core/renderer_d3d12/gpu_commands.h"

#include <algorithm>
#include <stdexcept>
#include <winrt/base.h>

namespace D3D12 {
namespace {
constexpr char Source[] = R"(
RWByteAddressBuffer Input : register(u0);
RWByteAddressBuffer Count : register(u1);
RWByteAddressBuffer Arguments : register(u2);
RWByteAddressBuffer Indices : register(u3);
cbuffer Config : register(b0) {
    uint mode, number, input_offset, count_offset;
    uint stride, indexed, has_count, index_bytes;
    uint swap_mode, primitive, provoking, input_count;
};
uint LoadIndex(uint i) {
    if (!index_bytes) return i;
    if (i >= stride) return 0;
    uint value = 0;
    uint mask = swap_mode == 1 ? 1 : swap_mode == 2 ? 3 : swap_mode == 3 ? 2 : 0;
    for (uint byte = 0; byte < index_bytes; ++byte) {
        uint relative = (i * index_bytes + byte) ^ mask;
        bool valid = relative < indexed * index_bytes;
        uint address = input_offset + (valid ? relative : 0);
        uint word = Input.Load(address & ~3u);
        uint component = valid ? (word >> ((address & 3u) * 8)) & 255u : 0;
        value |= component << (byte * 8);
    }
    return value;
}
[numthreads(64,1,1)]
void main(uint3 thread : SV_DispatchThreadID) {
    uint i = thread.x + thread.y * (65535u * 64u);
    if (i >= number) return;
    if (mode == 3) {
        Input.Store2(0, uint2(0,0));
        Arguments.Store4(0, uint4(0,1,0,0));
        return;
    }
    if (mode == 4) {
        Arguments.Store(i * 20, i);
        Arguments.Store4(i * 20 + 4, uint4(Input.Load(0) / stride,1,0,0));
        return;
    }
    if (mode == 2) {
        uint3 groups = Input.Load3(input_offset);
        if (any(groups > 65535u)) groups = uint3(0,0,0);
        Arguments.Store3(0, groups);
        Arguments.Store3(12, groups);
        return;
    }
    if (mode == 0) {
        uint output = i * (indexed != 0 ? 28 : 24);
        bool active = !has_count || i < Count.Load(count_offset);
        uint4 value = active ? Input.Load4(input_offset + i * stride) : uint4(0,0,0,0);
        uint instance = indexed != 0 && active ? Input.Load(input_offset + i * stride + 16) : value.w;
        Arguments.Store2(output, uint2(indexed != 0 ? value.w : value.z, instance));
        Arguments.Store4(output + 8, value);
        if (indexed != 0) Arguments.Store(output + 24, instance);
        return;
    }
    uint group = i / 3, lane = i % 3, source = i;
    if (primitive == 1) source = (i & 1) == 0 ? i / 2 : (i / 2 + 1) % input_count;
    if (primitive == 2) source = lane == 0 ? 0 : group + lane;
    if (primitive == 3) {
        uint vertex = i % 6;
        source = (i / 6) * 4 + (vertex < 3 ? vertex : (vertex == 3 ? 0 : vertex - 2));
    }
    if (primitive == 4) {
        uint vertex = i % 6;
        source = (i / 6) * 2 + (vertex == 0 || vertex == 3 ? 0 : vertex == 1 ? 1 : vertex == 5 ? 2 : 3);
    }
    if (primitive == 5) {
        source = group + lane;
        if ((group & 1) != 0) {
            if (provoking != 0) source = group + (lane == 0 ? 1 : lane == 1 ? 0 : 2);
            else source = group + (lane == 0 ? 0 : lane == 1 ? 2 : 1);
        }
    }
    if (primitive == 7) source = i / 2 + (i & 1);
    if (provoking != 0) {
        if (primitive == 1 || primitive == 7 || primitive == 8) {
            uint other = i ^ 1u;
            source = primitive == 8 ? other : primitive == 7 ? other / 2 + (other & 1) :
                (other & 1) == 0 ? other / 2 : (other / 2 + 1) % input_count;
        } else {
            uint rotated = (lane + 2) % 3;
            if (primitive == 2) source = rotated == 0 ? 0 : group + rotated;
            if (primitive == 3) {
                uint vertex = (i / 3 % 2) * 3 + rotated;
                source = (i / 6) * 4 + (vertex < 3 ? vertex : vertex == 3 ? 0 : vertex - 2);
            }
            if (primitive == 4) {
                uint vertex = (i / 3 % 2) * 3 + rotated;
                source = (i / 6) * 2 + (vertex == 0 || vertex == 3 ? 0 : vertex == 1 ? 1 : vertex == 5 ? 2 : 3);
            }
            if (primitive == 5) {
                source = group + ((group & 1) != 0 ? rotated == 0 ? 1 : rotated == 1 ? 0 : 2 : rotated);
            }
            if (primitive == 6) source = group * 3 + rotated;
        }
    }
    Indices.Store(i * 4, LoadIndex(source));
}
)";
}
void GpuCommands::Initialize(D3D12DeviceContext& context, ShaderCache& shaders) {
    if (device)
        return;
    device = &context;
    allocator.Initialize(context.Device(), 64ULL * 1024 * 1024);
    D3D12_ROOT_PARAMETER parameters[5]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 12};
    for (u32 index = 1; index < 5; ++index) {
        parameters[index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[index].Descriptor = {index - 1, 0};
    }
    D3D12_ROOT_SIGNATURE_DESC description{5, parameters};
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
    winrt::check_hresult(
        D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
    winrt::check_hresult(context.Device()->CreateRootSignature(
        0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)));
    const auto shader = shaders.CompileHlslRaw(Source, L"cs_6_4", {});
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
    compute.pRootSignature = root.Get();
    compute.CS = {shader->dxil.data(), shader->dxil.size()};
    winrt::check_hresult(
        context.Device()->CreateComputePipelineState(&compute, IID_PPV_ARGS(&pipeline)));
}
void GpuCommands::Transition(ID3D12GraphicsCommandList* list, D3D12Resource& resource,
                             D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES next) {
    if (state == next)
        return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state, next};
    list->ResourceBarrier(1, &barrier);
    state = next;
}
void GpuCommands::Prepare(u32 commands, u32 elements) {
    if (commands > 4096 || elements > 4 * 1024 * 1024)
        throw std::runtime_error("D3D12 GPU command expansion exceeds budget");
    const auto resize = [&](D3D12Resource& resource, u64& capacity, u64 size,
                            D3D12_RESOURCE_STATES& state) {
        size = std::max<u64>(size, 16);
        if (size <= capacity)
            return;
        resource.Reset();
        capacity = 0;
        winrt::check_hresult(allocator.CreateBuffer(
            size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, resource));
        capacity = size;
        state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    };
    resize(arguments, argument_capacity, u64(commands) * 28, argument_state);
    resize(indices, index_capacity, u64(elements) * 4, index_state);
}
void GpuCommands::Bind(ID3D12GraphicsCommandList* list, const std::array<u32, 12>& constants,
                       const ResourceCache::View& input, const ResourceCache::View& count) {
    Transition(list, arguments, argument_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list, indices, index_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetComputeRootSignature(root.Get());
    list->SetPipelineState(pipeline.Get());
    list->SetComputeRoot32BitConstants(0, constants.size(), constants.data(), 0);
    list->SetComputeRootUnorderedAccessView(1, input.buffer
                                                   ? input.buffer->native->GetGPUVirtualAddress()
                                                   : arguments->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2, count.buffer
                                                   ? count.buffer->native->GetGPUVirtualAddress()
                                                   : arguments->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(3, arguments->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(4, indices->GetGPUVirtualAddress());
    const auto groups = (constants[1] + 63) / 64;
    list->Dispatch(std::min(groups, 65535u), (groups + 65534) / 65535, 1);
}
void GpuCommands::RecordArguments(ID3D12GraphicsCommandList* list, const Arguments& description,
                                  const ResourceCache::View& input,
                                  const ResourceCache::View& count) {
    Bind(list,
         {0, description.count, u32(input.offset), u32(count.offset), description.stride,
          u32(description.indexed), u32(bool(description.count_address)), 0, 0, 0, 0, 0},
         input, count);
    Transition(list, arguments, argument_state, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
}
void GpuCommands::RecordDispatch(ID3D12GraphicsCommandList* list,
                                 const ResourceCache::View& input) {
    Bind(list, {2, 1, u32(input.offset), 0, 0, 0, 0, 0, 0, 0, 0, 0}, input, {});
    Transition(list, arguments, argument_state, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
}
void GpuCommands::ExecuteDispatch(ID3D12GraphicsCommandList* list, ID3D12RootSignature* compute) {
    if (signature_root.Get() != compute) {
        draw.Reset();
        indexed_draw.Reset();
        dispatch.Reset();
        signature_root = compute;
    }
    if (!dispatch) {
        D3D12_INDIRECT_ARGUMENT_DESC parameters[2]{};
        parameters[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
        parameters[0].Constant = {1, 0, 3};
        parameters[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC description{24, 2, parameters};
        winrt::check_hresult(device->Device()->CreateCommandSignature(&description, compute,
                                                                      IID_PPV_ARGS(&dispatch)));
    }
    list->ExecuteIndirect(dispatch.Get(), 1, arguments.Get(), 0, nullptr, 0);
}
void GpuCommands::RecordIndices(ID3D12GraphicsCommandList* list, const Expansion& description,
                                const ResourceCache::View& input, u32 bytes, u32 swap,
                                u32 readable_count, u32 read_count) {
    Bind(list,
         {1, description.output_count, u32(input.offset), 0,
          std::min(description.input_count, readable_count),
          read_count == UINT32_MAX ? description.input_count : read_count, 0, bytes, swap,
          description.mode, u32(description.provoking_last), description.input_count},
         input, {});
    Transition(list, indices, index_state, D3D12_RESOURCE_STATE_INDEX_BUFFER);
}
void GpuCommands::Execute(ID3D12GraphicsCommandList* list, ID3D12RootSignature* graphics,
                          bool indexed, u32 count) {
    if (signature_root.Get() != graphics) {
        draw.Reset();
        indexed_draw.Reset();
        dispatch.Reset();
        signature_root = graphics;
    }
    auto& signature = indexed ? indexed_draw : draw;
    if (!signature) {
        D3D12_INDIRECT_ARGUMENT_DESC parameters[2]{};
        parameters[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
        parameters[0].Constant = {1, 0, 2};
        parameters[1].Type =
            indexed ? D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        D3D12_COMMAND_SIGNATURE_DESC description{indexed ? 28u : 24u, 2, parameters};
        winrt::check_hresult(device->Device()->CreateCommandSignature(&description, graphics,
                                                                      IID_PPV_ARGS(&signature)));
    }
    list->ExecuteIndirect(signature.Get(), count, arguments.Get(), 0, nullptr, 0);
}
void GpuCommands::PrepareGeometry(u64 bytes, u32 batches) {
    if (!bytes || bytes > 32ULL * 1024 * 1024)
        throw std::runtime_error("D3D12 geometry capture exceeds memory budget");
    if (bytes > geometry_capacity) {
        geometry_vertices.Reset();
        geometry_capacity = 0;
        winrt::check_hresult(allocator.CreateBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT,
                                                    D3D12_RESOURCE_STATE_STREAM_OUT,
                                                    D3D12_RESOURCE_FLAG_NONE, geometry_vertices));
        geometry_capacity = bytes;
        geometry_state = D3D12_RESOURCE_STATE_STREAM_OUT;
    }
    if (!geometry_counter.Get()) {
        winrt::check_hresult(allocator.CreateBuffer(
            16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, geometry_counter));
        geometry_counter_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (geometry_argument_capacity < u64(batches) * 20) {
        geometry_arguments.Reset();
        geometry_argument_capacity = 0;
        winrt::check_hresult(allocator.CreateBuffer(
            u64(batches) * 20, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, geometry_arguments));
        geometry_argument_capacity = u64(batches) * 20;
        geometry_argument_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
}
D3D12_STREAM_OUTPUT_BUFFER_VIEW GpuCommands::BeginGeometry(ID3D12GraphicsCommandList* list) {
    Transition(list, geometry_counter, geometry_counter_state,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list, geometry_arguments, geometry_argument_state,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetComputeRootSignature(root.Get());
    list->SetPipelineState(pipeline.Get());
    const std::array<u32, 12> constants{3, 1};
    list->SetComputeRoot32BitConstants(0, constants.size(), constants.data(), 0);
    list->SetComputeRootUnorderedAccessView(1, geometry_counter->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2, geometry_counter->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(3, geometry_arguments->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(4, geometry_arguments->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    Transition(list, geometry_arguments, geometry_argument_state,
               D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    Transition(list, geometry_counter, geometry_counter_state, D3D12_RESOURCE_STATE_STREAM_OUT);
    Transition(list, geometry_vertices, geometry_state, D3D12_RESOURCE_STATE_STREAM_OUT);
    return {geometry_vertices->GetGPUVirtualAddress(), geometry_capacity,
            geometry_counter->GetGPUVirtualAddress()};
}
void GpuCommands::EndGeometry(ID3D12GraphicsCommandList* list, u32 stride, u32 batches) {
    list->SOSetTargets(0, 0, nullptr);
    D3D12_RESOURCE_BARRIER memory{};
    memory.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &memory);
    Transition(list, geometry_counter, geometry_counter_state,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list, geometry_arguments, geometry_argument_state,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list, geometry_vertices, geometry_state,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->SetComputeRootSignature(root.Get());
    list->SetPipelineState(pipeline.Get());
    const std::array<u32, 12> constants{4, batches, 0, 0, stride};
    list->SetComputeRoot32BitConstants(0, constants.size(), constants.data(), 0);
    list->SetComputeRootUnorderedAccessView(1, geometry_counter->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2, geometry_counter->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(3, geometry_arguments->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(4, geometry_arguments->GetGPUVirtualAddress());
    list->Dispatch((batches + 63) / 64, 1, 1);
    Transition(list, geometry_arguments, geometry_argument_state,
               D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
}
void GpuCommands::ExecuteGeometry(ID3D12GraphicsCommandList* list, ID3D12RootSignature* graphics,
                                  u32 slot, u32 batches) {
    if (geometry_signature_root.Get() != graphics) {
        geometry_draw.Reset();
        geometry_signature_root = graphics;
    }
    if (!geometry_draw) {
        D3D12_INDIRECT_ARGUMENT_DESC arguments[2]{};
        arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
        arguments[0].Constant = {slot, 0, 1};
        arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        D3D12_COMMAND_SIGNATURE_DESC description{20, 2, arguments};
        winrt::check_hresult(device->Device()->CreateCommandSignature(
            &description, graphics, IID_PPV_ARGS(&geometry_draw)));
    }
    list->ExecuteIndirect(geometry_draw.Get(), batches, geometry_arguments.Get(), 0, nullptr, 0);
}
} // namespace D3D12
