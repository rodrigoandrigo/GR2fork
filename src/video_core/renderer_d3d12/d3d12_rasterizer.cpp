#include "video_core/renderer_d3d12/d3d12_rasterizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"

namespace D3D12 {
namespace {
struct Program {
    std::array<u32, 16> user_data;
    std::vector<u32> code;
    u64 hash;
    VAddr address;
    Shader::ShaderParams Params() const {
        return {user_data, code, hash, address};
    }
};
Program ReadProgram(Core::MemoryManager& memory, const auto& regs) {
    const VAddr address = regs.template Address<VAddr>();
    if (!address)
        throw std::runtime_error("Guest shader program address is null");
    std::vector<u32> header(0x4000 + sizeof(AmdGpu::BinaryInfo) / 4);
    memory.CopySparseMemory(address, reinterpret_cast<u8*>(header.data()), header.size() * 4);
    AmdGpu::BinaryInfo info{};
    bool found{};
    if (header[0] == 0xbeeb03ff) {
        const u64 offset = (u64(header[1]) + 1) * 8;
        if (offset > 16 * 1024 * 1024 || address > UINT64_MAX - offset)
            throw std::runtime_error("Guest shader header offset exceeds compiler budget");
        memory.CopySparseMemory(address + offset, reinterpret_cast<u8*>(&info), sizeof(info));
        found = info.Valid();
    }
    if (!found)
        for (u32 index = 0; index < 0x4000; ++index) {
            std::memcpy(&info, header.data() + index, sizeof(info));
            if (info.Valid()) {
                found = true;
                break;
            }
        }
    if (!found || !info.length || info.length % 4 || info.length > 4 * 1024 * 1024)
        throw std::runtime_error("Guest shader binary metadata is invalid");
    Program result{regs.user_data, std::vector<u32>(info.length / 4), info.shader_hash, address};
    memory.CopySparseMemory(address, reinterpret_cast<u8*>(result.code.data()), info.length);
    return result;
}
void CommonInfo(Shader::RuntimeInfo& runtime, const AmdGpu::ShaderProgram& program) {
    runtime.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
    runtime.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
    runtime.props.fp_round_mode32 = program.settings.fp_round_mode32;
    runtime.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
    runtime.props.num_allocated_vgprs = program.NumVgprs();
    runtime.props.num_user_data = program.settings.num_user_regs;
    runtime.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
    runtime.props.dx10_clamp = program.settings.dx10_clamp;
}
D3D12_BLEND BlendFactor(AmdGpu::BlendControl::BlendFactor factor) {
    using F = AmdGpu::BlendControl::BlendFactor;
    switch (factor) {
    case F::Zero:
        return D3D12_BLEND_ZERO;
    case F::One:
        return D3D12_BLEND_ONE;
    case F::SrcColor:
        return D3D12_BLEND_SRC_COLOR;
    case F::OneMinusSrcColor:
        return D3D12_BLEND_INV_SRC_COLOR;
    case F::SrcAlpha:
        return D3D12_BLEND_SRC_ALPHA;
    case F::OneMinusSrcAlpha:
        return D3D12_BLEND_INV_SRC_ALPHA;
    case F::DstAlpha:
        return D3D12_BLEND_DEST_ALPHA;
    case F::OneMinusDstAlpha:
        return D3D12_BLEND_INV_DEST_ALPHA;
    case F::DstColor:
        return D3D12_BLEND_DEST_COLOR;
    case F::OneMinusDstColor:
        return D3D12_BLEND_INV_DEST_COLOR;
    case F::SrcAlphaSaturate:
        return D3D12_BLEND_SRC_ALPHA_SAT;
    case F::ConstantColor:
        return D3D12_BLEND_BLEND_FACTOR;
    case F::OneMinusConstantColor:
        return D3D12_BLEND_INV_BLEND_FACTOR;
    case F::ConstantAlpha:
        return D3D12_BLEND_ALPHA_FACTOR;
    case F::OneMinusConstantAlpha:
        return D3D12_BLEND_INV_ALPHA_FACTOR;
    case F::Src1Color:
        return D3D12_BLEND_SRC1_COLOR;
    case F::InvSrc1Color:
        return D3D12_BLEND_INV_SRC1_COLOR;
    case F::Src1Alpha:
        return D3D12_BLEND_SRC1_ALPHA;
    case F::InvSrc1Alpha:
        return D3D12_BLEND_INV_SRC1_ALPHA;
    default:
        throw std::runtime_error("Invalid guest blend factor");
    }
}
D3D12_BLEND_OP BlendOperation(AmdGpu::BlendControl::BlendFunc function) {
    using F = AmdGpu::BlendControl::BlendFunc;
    switch (function) {
    case F::Add:
        return D3D12_BLEND_OP_ADD;
    case F::Subtract:
        return D3D12_BLEND_OP_SUBTRACT;
    case F::ReverseSubtract:
        return D3D12_BLEND_OP_REV_SUBTRACT;
    case F::Min:
        return D3D12_BLEND_OP_MIN;
    case F::Max:
        return D3D12_BLEND_OP_MAX;
    default:
        throw std::runtime_error("Invalid guest blend operation");
    }
}
} // namespace
Rasterizer::Rasterizer(AmdGpu::Liverpool& gpu, Core::MemoryManager& manager)
    : liverpool{gpu}, memory{manager} {
    runtime.Initialize([this](VAddr address, std::span<u8> data) { Read(address, data); },
                       [this](VAddr address, std::span<const u8> data) { Write(address, data); });
    memory.SetRasterizer(this);
}
Rasterizer::~Rasterizer() {
    memory.SetRasterizer(nullptr);
}
void Rasterizer::Read(VAddr address, std::span<u8> data) {
    if (address > UINT64_MAX - data.size())
        throw std::runtime_error("Guest memory read range overflow");
    memory.CopySparseMemory(address, data.data(), data.size());
}
void Rasterizer::Write(VAddr address, std::span<const u8> data) {
    if (address > UINT64_MAX - data.size() ||
        !memory.TryWriteBacking(reinterpret_cast<void*>(address), data.data(), data.size()))
        throw std::runtime_error("D3D12 guest writeback targets unmapped memory");
}
void Rasterizer::Dispatch(std::array<u32, 3> groups) {
    Core::MemoryManager::GuestCopyScope scope{&memory};
    const auto& cs = liverpool.GetCsRegs();
    if (cs.start_x || cs.start_y || cs.start_z)
        throw std::runtime_error("D3D12 compute workgroup start offsets are not implemented");
    auto program = ReadProgram(memory, cs);
    Shader::RuntimeInfo info{};
    info.Initialize(Shader::HwStage::Compute, Shader::SwStage::Compute);
    info.props.num_user_data = cs.settings.num_user_regs;
    info.props.num_allocated_vgprs = cs.settings.num_vgprs * 4;
    info.props.fp_denorm_mode32 = cs.settings.fp_denorm_mode32;
    info.props.fp_denorm_mode16_64 = cs.settings.fp_denorm_mode64;
    info.props.fp_round_mode32 = cs.settings.fp_round_mode32;
    info.props.fp_round_mode16_64 = cs.settings.fp_round_mode64;
    info.props.dx10_clamp = cs.settings.dx10_clamp;
    info.hw.cs.workgroup_size = {cs.num_thread_x.full, cs.num_thread_y.full, cs.num_thread_z.full};
    info.hw.cs.tgid_enable = {cs.IsTgidEnabled(0), cs.IsTgidEnabled(1), cs.IsTgidEnabled(2)};
    info.hw.cs.shared_memory_size = cs.SharedMemSize();
    runtime.Dispatch(program.Params(), info, groups);
}
void Rasterizer::DispatchDirect() {
    Execute([&] {
        const auto& cs = liverpool.GetCsRegs();
        Dispatch({cs.dim_x, cs.dim_y, cs.dim_z});
    });
}
void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    Execute([&] { DispatchIndirectImpl(address, offset, size); });
}
void Rasterizer::DispatchIndirectImpl(VAddr address, u32 offset, u32 size) {
    if (size < 12 || address > UINT64_MAX - offset)
        throw std::runtime_error("Invalid guest indirect dispatch range");
    Core::MemoryManager::GuestCopyScope scope{&memory};
    std::array<u32, 3> groups;
    Read(address + offset, {reinterpret_cast<u8*>(groups.data()), sizeof(groups)});
    Dispatch(groups);
}
void Rasterizer::Draw(bool indexed, u32 index_offset) {
    Execute([&] { DrawImpl(indexed, index_offset, {}); });
}
void Rasterizer::DrawImpl(bool indexed, u32 index_offset, std::optional<IndirectDraw> indirect) {
    Core::MemoryManager::GuestCopyScope scope{&memory};
    const auto& regs = liverpool.regs;
    if (regs.stage_enable.IsStageEnabled(u32(Shader::HwStage::Geometry)) ||
        regs.stage_enable.IsStageEnabled(u32(Shader::HwStage::Local)) ||
        regs.depth_control.stencil_enable || regs.depth_control.depth_bounds_enable ||
        regs.depth_control.enable_color_writes_on_depth_fail ||
        regs.depth_control.disable_color_writes_on_depth_pass ||
        regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Normal ||
        regs.color_control.rop3 != AmdGpu::ColorControl::LogicOp::Copy ||
        regs.aa_config.NumSamples() != 1 || regs.vs_output_control.vs_out_misc_enable ||
        regs.vs_output_control.clip_distance_enable || regs.vs_output_control.cull_distance_enable)
        throw std::runtime_error("D3D12 advanced guest graphics state is not implemented");
    auto vs = ReadProgram(memory, regs.vs_program);
    auto ps = ReadProgram(memory, regs.ps_program);
    CommandRuntime::DrawRequest draw{vs.Params(), ps.Params()};
    draw.vertex_runtime.Initialize(Shader::HwStage::Vertex, Shader::SwStage::Vertex);
    draw.fragment_runtime.Initialize(Shader::HwStage::Fragment, Shader::SwStage::Fragment);
    CommonInfo(draw.vertex_runtime, regs.vs_program);
    CommonInfo(draw.fragment_runtime, regs.ps_program);
    draw.vertex_runtime.hw.vs.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
    draw.vertex_runtime.hw.vs.emulate_depth_negative_one_to_one =
        regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
    draw.vertex_runtime.hw.vs.clip_disable = regs.IsClipDisabled();
    draw.vertex_runtime.sw.vs.step_rate_0 = regs.vgt_instance_step_rate_0;
    draw.vertex_runtime.sw.vs.step_rate_1 = regs.vgt_instance_step_rate_1;
    if (indirect) {
        draw.vertex_runtime.sw.vs.vertex_sgpr_offset = indirect->vertex_sgpr;
        draw.vertex_runtime.sw.vs.instance_sgpr_offset = indirect->instance_sgpr;
        draw.vertex_runtime.sw.vs.is_indirect = true;
    }
    draw.fragment_runtime.hw.fs.en_flags = regs.ps_input_ena;
    draw.fragment_runtime.hw.fs.addr_flags = regs.ps_input_addr;
    draw.fragment_runtime.hw.fs.num_inputs = regs.num_interp;
    draw.fragment_runtime.hw.fs.front_face_all_bits = regs.barycentric_control.front_face_all_bits;
    draw.fragment_runtime.hw.fs.depth_before_shader = regs.depth_shader_control.depth_before_shader;
    draw.fragment_runtime.hw.fs.z_export_format = regs.z_export_format;
    draw.fragment_runtime.hw.fs.mrtz_mask = regs.depth_shader_control.z_export_enable;
    for (u32 i = 0; i < regs.num_interp; ++i)
        draw.fragment_runtime.hw.fs.inputs[i] = {
            u16(regs.ps_inputs[i].input_offset), u16(regs.ps_inputs[i].use_default),
            u16(regs.ps_inputs[i].flat_shade), u16(regs.ps_inputs[i].default_value)};
    draw.blend.IndependentBlendEnable = TRUE;
    for (u32 i = 0; i < 8; ++i) {
        const auto& color = regs.color_buffers[i];
        if (!color) {
            if (regs.color_target_mask.GetMask(i))
                throw std::runtime_error("D3D12 sparse guest render target is not implemented");
            continue;
        }
        if (draw.colors.size() != i || color.BaseSlice())
            throw std::runtime_error("D3D12 guest render target subview is not implemented");
        draw.colors.emplace_back(color, liverpool.last_cb_extent[i]);
        draw.fragment_runtime.hw.fs.color_buffers[i] = {color.GetDataFmt(),
                                                        color.GetNumberFmt(),
                                                        color.GetNumberConversion(),
                                                        regs.color_export_format.GetFormat(i),
                                                        0,
                                                        color.Swizzle()};
        const auto& blend = regs.blend_control[i];
        auto& target = draw.blend.RenderTarget[i];
        target.BlendEnable = blend.enable;
        target.SrcBlend = BlendFactor(blend.color_src_factor);
        target.DestBlend = BlendFactor(blend.color_dst_factor);
        target.BlendOp = BlendOperation(blend.color_func);
        target.SrcBlendAlpha = BlendFactor(blend.separate_alpha_blend ? blend.alpha_src_factor
                                                                      : blend.color_src_factor);
        target.DestBlendAlpha = BlendFactor(blend.separate_alpha_blend ? blend.alpha_dst_factor
                                                                       : blend.color_dst_factor);
        target.BlendOpAlpha =
            BlendOperation(blend.separate_alpha_blend ? blend.alpha_func : blend.color_func);
        target.RenderTargetWriteMask = regs.color_target_mask.GetMask(i);
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
    }
    draw.blend_factor = regs.blend_constants;
    const auto& polygon = regs.polygon_control;
    if (polygon.cull_front && polygon.cull_back)
        return;
    if (polygon.enable_polygon_mode && (polygon.polygon_mode_front != polygon.polygon_mode_back ||
                                        polygon.polygon_mode_front == AmdGpu::PolygonMode::Point))
        throw std::runtime_error("D3D12 guest polygon mode requires primitive emulation");
    draw.rasterizer.FillMode =
        polygon.enable_polygon_mode && polygon.polygon_mode_front == AmdGpu::PolygonMode::Line
            ? D3D12_FILL_MODE_WIREFRAME
            : D3D12_FILL_MODE_SOLID;
    draw.rasterizer.CullMode = polygon.cull_front  ? D3D12_CULL_MODE_FRONT
                               : polygon.cull_back ? D3D12_CULL_MODE_BACK
                                                   : D3D12_CULL_MODE_NONE;
    draw.rasterizer.FrontCounterClockwise =
        polygon.front_face == AmdGpu::FrontFace::CounterClockwise;
    draw.rasterizer.DepthClipEnable = regs.clipper_control.ZclipEnable();
    if (polygon.NeedsBias())
        throw std::runtime_error("D3D12 guest polygon offset conversion is not implemented");
    draw.depth.DepthEnable = regs.depth_control.depth_enable;
    draw.depth.DepthWriteMask = regs.depth_control.depth_write_enable ? D3D12_DEPTH_WRITE_MASK_ALL
                                                                      : D3D12_DEPTH_WRITE_MASK_ZERO;
    draw.depth.DepthFunc = D3D12_COMPARISON_FUNC(u32(regs.depth_control.depth_func) + 1);
    if (draw.depth.DepthEnable)
        draw.depth_image.emplace(regs.depth_buffer, regs.depth_view.NumSlices(),
                                 VAddr(regs.depth_htile_data_base.address) << 8,
                                 liverpool.last_db_extent, true);
    const auto& vp = regs.viewports[0];
    const float xscale = regs.viewport_control.xscale_enable ? vp.xscale : 1;
    const float yscale = regs.viewport_control.yscale_enable ? vp.yscale : 1;
    const float xoffset = regs.viewport_control.xoffset_enable ? vp.xoffset : 0;
    const float yoffset = regs.viewport_control.yoffset_enable ? vp.yoffset : 0;
    if (xscale <= 0 || yscale >= 0 || !std::isfinite(xscale) || !std::isfinite(yscale))
        throw std::runtime_error("D3D12 guest viewport orientation is not implemented");
    draw.viewport = {xoffset - xscale,
                     yoffset + yscale,
                     xscale * 2,
                     -yscale * 2,
                     regs.viewport_depths[0].zmin,
                     regs.viewport_depths[0].zmax};
    const auto& scissor = regs.screen_scissor;
    draw.scissor = {std::max<s32>(0, scissor.top_left_x), std::max<s32>(0, scissor.top_left_y),
                    std::max<s32>(0, scissor.bottom_right_x),
                    std::max<s32>(0, scissor.bottom_right_y)};
    if (regs.mode_control.vport_scissor_enable) {
        const auto& vp_scissor = regs.viewport_scissors[0];
        draw.scissor.left = std::max<LONG>(draw.scissor.left, vp_scissor.top_left_x);
        draw.scissor.top = std::max<LONG>(draw.scissor.top, vp_scissor.top_left_y);
        draw.scissor.right = std::min<LONG>(draw.scissor.right, vp_scissor.bottom_right_x);
        draw.scissor.bottom = std::min<LONG>(draw.scissor.bottom, vp_scissor.bottom_right_y);
    }
    draw.push = {xoffset, yoffset, xscale, yscale, {}};
    static_assert(sizeof(regs.clip_user_data) == sizeof(draw.clip_planes));
    std::memcpy(draw.clip_planes.data(), regs.clip_user_data.data(), sizeof(draw.clip_planes));
    switch (regs.primitive_type) {
    case AmdGpu::PrimitiveType::PointList:
        draw.topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        draw.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        break;
    case AmdGpu::PrimitiveType::LineList:
        draw.topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        draw.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        break;
    case AmdGpu::PrimitiveType::LineStrip:
        draw.topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        draw.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        break;
    case AmdGpu::PrimitiveType::TriangleList:
        draw.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        draw.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        break;
    case AmdGpu::PrimitiveType::TriangleStrip:
        draw.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        draw.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        break;
    default:
        throw std::runtime_error("D3D12 guest primitive requires topology emulation");
    }
    draw.count = regs.num_indices;
    draw.instances = regs.num_instances.NumInstances();
    if (indirect) {
        draw.count = indirect->count;
        draw.instances = indirect->instances;
        draw.first_instance = indirect->first_instance;
        index_offset = indirect->first;
    }
    if (indexed) {
        if (regs.index_buffer_type.swap_mode != AmdGpu::IndexSwapMode::None)
            throw std::runtime_error("D3D12 guest index byte swap is not implemented");
        const u32 size = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16 ? 2 : 4;
        const VAddr base = regs.index_base_address.Address();
        if (base > UINT64_MAX - u64(index_offset) * size)
            throw std::runtime_error("Guest index buffer address overflow");
        draw.index_address = base + u64(index_offset) * size;
        draw.index_format = size == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        draw.base_vertex = indirect ? indirect->base_vertex : regs.index_offset;
        if (regs.enable_primitive_restart) {
            const u32 cut = size == 2 ? 0xffff : UINT32_MAX;
            if (regs.primitive_restart_index != cut)
                throw std::runtime_error("D3D12 custom guest restart index requires primitive conversion");
            draw.strip_cut = size == 2 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF
                                      : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
        }
    } else
        draw.first_vertex = indirect ? indirect->first : regs.index_offset;
    runtime.Draw(draw);
}
void Rasterizer::DrawIndirect(bool indexed, VAddr address, u32 offset, u32 stride, u32 count, VAddr count_address) {
    DrawIndirect(indexed, address, offset, stride, count, count_address, 0, 0);
}
void Rasterizer::DrawIndirect(bool indexed, VAddr address, u32 offset, u32 stride, u32 count,
                             VAddr count_address, u16 vertex_sgpr, u16 instance_sgpr) {
    Execute([&] { DrawIndirectImpl(indexed, address, offset, stride, count, count_address,
                                   vertex_sgpr, instance_sgpr); });
}
void Rasterizer::DrawIndirectImpl(bool indexed, VAddr address, u32 offset, u32 stride, u32 count,
                                 VAddr count_address, u16 vertex_sgpr, u16 instance_sgpr) {
    Core::MemoryManager::GuestCopyScope scope{&memory};
    const u32 bytes = indexed ? 20 : 16;
    if (count > 4096 || (count > 1 && (stride < bytes || stride % 4)) || address > UINT64_MAX - offset)
        throw std::runtime_error("Invalid guest indirect draw range");
    if (count_address) {
        u32 actual;
        Read(count_address, {reinterpret_cast<u8*>(&actual), sizeof(actual)});
        count = std::min(count, actual);
    }
    const VAddr first = address + offset;
    for (u32 draw = 0; draw < count; ++draw) {
        const u64 delta = u64(draw) * stride;
        if (first > UINT64_MAX - delta)
            throw std::runtime_error("Guest indirect draw address overflow");
        std::array<u32, 5> args{};
        Read(first + delta, {reinterpret_cast<u8*>(args.data()), bytes});
        DrawImpl(indexed, 0, IndirectDraw{args[0], args[1], args[2], indexed ? args[4] : args[3],
            indexed ? std::bit_cast<s32>(args[3]) : 0, vertex_sgpr, instance_sgpr});
    }
}
void Rasterizer::FillBuffer(VAddr address, u32 size, u32 value, bool use_gds) {
    Execute([&] { FillBufferImpl(address, size, value, use_gds); });
}
void Rasterizer::FillBufferImpl(VAddr address, u32 size, u32 value, bool use_gds) {
    Core::MemoryManager::GuestCopyScope scope{&memory};
    if (!size)
        return;
    if (size % 4 || size > 16 * 1024 * 1024)
        throw std::runtime_error("Invalid guest fill range");
    std::vector<u32> data(size / 4, value);
    if (use_gds) {
        if (address > gds.size() || size > gds.size() - address)
            throw std::runtime_error("Guest GDS fill range overflow");
        std::memcpy(gds.data() + address, data.data(), size);
    } else
        Write(address, {reinterpret_cast<const u8*>(data.data()), size});
}
void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 size, bool dst_gds, bool src_gds) {
    Execute([&] { CopyBufferImpl(dst, src, size, dst_gds, src_gds); });
}
void Rasterizer::CopyBufferImpl(VAddr dst, VAddr src, u32 size, bool dst_gds, bool src_gds) {
    Core::MemoryManager::GuestCopyScope scope{&memory};
    if (size > 16 * 1024 * 1024 || (src_gds && (src > gds.size() || size > gds.size() - src)) ||
        (dst_gds && (dst > gds.size() || size > gds.size() - dst)))
        throw std::runtime_error("Guest copy range exceeds backing budget");
    std::vector<u8> data(size);
    if (src_gds)
        std::memcpy(data.data(), gds.data() + src, size);
    else
        Read(src, data);
    if (dst_gds)
        std::memcpy(gds.data() + dst, data.data(), size);
    else
        Write(dst, data);
}
u32 Rasterizer::ReadDataFromGds(u32 offset) {
    u32 value{};
    Execute([&] {
        if (offset > gds.size() - 4)
            throw std::runtime_error("Guest GDS read range overflow");
        std::memcpy(&value, gds.data() + offset, 4);
    });
    return value;
}
void Rasterizer::MapMemory(VAddr address, u64 size) {
    runtime.Resources().MapMemory(address, size);
}
void Rasterizer::RegisterMemory(VAddr address, u64 size) {
    runtime.Resources().RegisterMemory(address, size);
}
void Rasterizer::UnmapMemory(VAddr address, u64 size) {
    runtime.Resources().UnmapMemory(address, size);
}
bool Rasterizer::InvalidateMemory(VAddr address, u64 size, bool locks) {
    return runtime.Resources().InvalidateMemory(address, size, locks);
}
} // namespace D3D12
