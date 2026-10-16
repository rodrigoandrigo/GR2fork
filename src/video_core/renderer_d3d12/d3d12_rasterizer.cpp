#include "video_core/renderer_d3d12/d3d12_rasterizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_d3d12/primitive_conversion.h"

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
D3D12_BLEND AlphaBlendFactor(AmdGpu::BlendControl::BlendFactor factor) {
    switch (BlendFactor(factor)) {
    case D3D12_BLEND_SRC_COLOR:
        return D3D12_BLEND_SRC_ALPHA;
    case D3D12_BLEND_INV_SRC_COLOR:
        return D3D12_BLEND_INV_SRC_ALPHA;
    case D3D12_BLEND_DEST_COLOR:
        return D3D12_BLEND_DEST_ALPHA;
    case D3D12_BLEND_INV_DEST_COLOR:
        return D3D12_BLEND_INV_DEST_ALPHA;
    case D3D12_BLEND_SRC_ALPHA_SAT:
        return D3D12_BLEND_ONE;
    case D3D12_BLEND_SRC1_COLOR:
        return D3D12_BLEND_SRC1_ALPHA;
    case D3D12_BLEND_INV_SRC1_COLOR:
        return D3D12_BLEND_INV_SRC1_ALPHA;
    default:
        return BlendFactor(factor);
    }
}
} // namespace
Rasterizer::Rasterizer(AmdGpu::Liverpool& gpu, Core::MemoryManager& manager)
    : liverpool{gpu}, memory{manager} {
    runtime.Initialize(
        [this](VAddr address, std::span<u8> data) { Read(address, data); },
        [this](VAddr address, std::span<const u8> data) { Write(address, data); },
        [this](VAddr address, u64 size) -> std::optional<VAddr> {
            if (!Core::MemoryManager::tls_in_guest_copy_scope)
                throw std::runtime_error("D3D12 backing resolution requires a guest memory lease");
            std::array<Core::MemoryManager::BackingSpan, 64> spans;
            const u32 count =
                memory.ResolveBackingSpans(address, size, spans.data(), spans.size(), false);
            if (!count)
                throw std::runtime_error(
                    "Guest GPU range lacks contiguous mapped physical backing");
            const auto base = reinterpret_cast<VAddr>(spans[0].ptr);
            u64 offset{};
            for (u32 i = 0; i < count; ++i) {
                if (reinterpret_cast<VAddr>(spans[i].ptr) != base + offset)
                    throw std::runtime_error(
                        "D3D12 noncontiguous physical buffers require segmented views");
                offset += spans[i].size;
            }
            if (offset != size)
                throw std::runtime_error("D3D12 physical backing resolution is incomplete");
            return base;
        });
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
    info.props.num_allocated_vgprs = (cs.settings.num_vgprs + 1) * 4;
    info.props.fp_denorm_mode32 = cs.settings.fp_denorm_mode32;
    info.props.fp_denorm_mode16_64 = cs.settings.fp_denorm_mode64;
    info.props.fp_round_mode32 = cs.settings.fp_round_mode32;
    info.props.fp_round_mode16_64 = cs.settings.fp_round_mode64;
    info.props.dx10_clamp = cs.settings.dx10_clamp;
    info.hw.cs.workgroup_size = {cs.num_thread_x.full, cs.num_thread_y.full, cs.num_thread_z.full};
    info.hw.cs.tgid_enable = {cs.IsTgidEnabled(0), cs.IsTgidEnabled(1), cs.IsTgidEnabled(2)};
    info.hw.cs.shared_memory_size = cs.SharedMemSize();
    runtime.Dispatch(program.Params(), info, groups, liverpool.regs.ta_bc_base.Address());
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
    if (regs.num_interp > 32)
        throw std::runtime_error("Guest fragment interpolation count exceeds register bounds");
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        const VideoCore::ImageInfo source{regs.color_buffers[0], liverpool.last_cb_extent[0]};
        const VideoCore::ImageInfo destination{regs.color_buffers[1], liverpool.last_cb_extent[1]};
        runtime.Resolve(source, destination, regs.color_buffers[0].BaseSlice(),
                        regs.color_buffers[1].BaseSlice());
        return;
    }
    if (regs.depth_control.stencil_enable || regs.depth_control.depth_bounds_enable ||
        regs.depth_control.enable_color_writes_on_depth_fail ||
        regs.depth_control.disable_color_writes_on_depth_pass ||
        (regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Normal &&
         regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Disable) ||
        regs.color_control.rop3 != AmdGpu::ColorControl::LogicOp::Copy ||
        regs.vs_output_control.vs_out_misc_enable || regs.vs_output_control.clip_distance_enable ||
        regs.vs_output_control.cull_distance_enable)
        throw std::runtime_error(fmt::format(
            "Unsupported native D3D12 graphics: geometry={} local={} stencil={} bounds={} "
            "depth_fail_color={} depth_pass_disable={} mode={} rop={:#x} misc={} clip={} cull={}",
            regs.stage_enable.IsStageEnabled(u32(Shader::HwStage::Geometry)),
            regs.stage_enable.IsStageEnabled(u32(Shader::HwStage::Local)),
            bool(regs.depth_control.stencil_enable), bool(regs.depth_control.depth_bounds_enable),
            bool(regs.depth_control.enable_color_writes_on_depth_fail),
            bool(regs.depth_control.disable_color_writes_on_depth_pass),
            u32(regs.color_control.mode), u32(regs.color_control.rop3),
            bool(regs.vs_output_control.vs_out_misc_enable),
            u32(regs.vs_output_control.clip_distance_enable),
            u32(regs.vs_output_control.cull_distance_enable)));
    const bool has_geometry = regs.stage_enable.IsStageEnabled(u32(Shader::HwStage::Geometry));
    const bool tessellation = regs.stage_enable.IsStageEnabled(u32(Shader::HwStage::Local));
    if (has_geometry && (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw))
        throw std::runtime_error("D3D12 on-chip geometry and stream output are not implemented");
    if (has_geometry && (regs.clipper_control.user_clip_plane_enable ||
                         regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW))
        throw std::runtime_error("D3D12 geometry clip-plane/depth conversion is not implemented");
    auto vs =
        ReadProgram(memory, tessellation ? regs.ls_program
                                         : (has_geometry ? regs.es_program : regs.vs_program));
    Program hs{}, ds{};
    if (tessellation) {
        hs = ReadProgram(memory, regs.hs_program);
        ds = ReadProgram(memory, has_geometry ? regs.es_program : regs.vs_program);
    }
    Program gs{}, copy{};
    if (has_geometry) {
        gs = ReadProgram(memory, regs.gs_program);
        copy = ReadProgram(memory, regs.vs_program);
    }
    Program ps{};
    if (regs.ps_program.Address<VAddr>())
        ps = ReadProgram(memory, regs.ps_program);
    CommandRuntime::DrawRequest draw{vs.Params(), ps.Params()};
    draw.samples = regs.aa_config.NumSamples();
    if (draw.samples > 16)
        throw std::runtime_error("Guest raster sample count exceeds D3D12 limits");
    if (draw.samples > 1) {
        const auto& values = regs.reg_array;
        const u32 iterations = 1u << ((values[0xa201] >> 4) & 7);
        if (iterations != 1 && iterations != draw.samples)
            throw std::runtime_error(
                "Partial guest sample shading requires additional D3D12 lowering");
        draw.sample_shading = iterations > 1;
        if ((values[0xa2dc] & 1) && !regs.depth_shader_control.alpha_to_mask_disable)
            throw std::runtime_error("Guest alpha-to-mask requires additional D3D12 lowering");
        draw.sample_masks = {values[0xa30e] & 0xffff, values[0xa30e] >> 16, values[0xa30f] & 0xffff,
                             values[0xa30f] >> 16};
        draw.sample_positions.resize(draw.samples * 4);
        for (u32 pixel = 0; pixel < 4; ++pixel)
            for (u32 sample = 0; sample < draw.samples; ++sample) {
                const u32 packed = values[0xa2fe + pixel * 4 + sample / 4] >> ((sample % 4) * 8);
                const auto coordinate = [](u32 value) {
                    return static_cast<INT8>(s32(value & 15) - (value & 8 ? 16 : 0));
                };
                draw.sample_positions[pixel * draw.samples + sample] = {coordinate(packed),
                                                                        coordinate(packed >> 4)};
            }
        bool same = true;
        for (u32 pixel = 1; pixel < 4; ++pixel)
            for (u32 sample = 0; sample < draw.samples; ++sample) {
                const auto a = draw.sample_positions[sample];
                const auto b = draw.sample_positions[pixel * draw.samples + sample];
                same &= a.X == b.X && a.Y == b.Y;
            }
        if (same)
            draw.sample_positions.resize(draw.samples);
        if (!regs.mode_control.msaa_enable)
            draw.sample_positions.assign(draw.samples, D3D12_SAMPLE_POSITION{0, 0});
    }
    draw.vertex_runtime.Initialize(
        tessellation ? Shader::HwStage::Local
                     : (has_geometry ? Shader::HwStage::Export : Shader::HwStage::Vertex),
        Shader::SwStage::Vertex);
    draw.fragment_runtime.Initialize(Shader::HwStage::Fragment, Shader::SwStage::Fragment);
    CommonInfo(draw.vertex_runtime,
               tessellation ? regs.ls_program : (has_geometry ? regs.es_program : regs.vs_program));
    CommonInfo(draw.fragment_runtime, regs.ps_program);
    if (has_geometry) {
        if (!tessellation)
            draw.vertex_runtime.hw.es.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        draw.geometry = gs.Params();
        draw.geometry_runtime.Initialize(Shader::HwStage::Geometry, Shader::SwStage::Geometry);
        CommonInfo(draw.geometry_runtime, regs.gs_program);
        auto& geometry = draw.geometry_runtime.hw.gs;
        geometry.output_vertices = regs.vgt_gs_max_vert_out;
        geometry.num_invocations =
            regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
        if (!geometry.num_invocations || geometry.num_invocations > D3D12_GS_MAX_INSTANCE_COUNT)
            throw std::runtime_error("Guest geometry invocation count exceeds D3D12 limits");
        geometry.in_primitive = regs.primitive_type;
        if (geometry.in_primitive == AmdGpu::PrimitiveType::LineLoop)
            geometry.in_primitive = AmdGpu::PrimitiveType::LineList;
        if (geometry.in_primitive == AmdGpu::PrimitiveType::TriangleFan)
            geometry.in_primitive = AmdGpu::PrimitiveType::TriangleList;
        if (tessellation) {
            switch (regs.tess_config.topology) {
            case AmdGpu::TessellationTopology::Point:
                geometry.in_primitive = AmdGpu::PrimitiveType::PointList;
                break;
            case AmdGpu::TessellationTopology::Line:
                geometry.in_primitive = AmdGpu::PrimitiveType::LineList;
                break;
            case AmdGpu::TessellationTopology::TriangleCw:
            case AmdGpu::TessellationTopology::TriangleCcw:
                geometry.in_primitive = AmdGpu::PrimitiveType::TriangleList;
                break;
            default:
                throw std::runtime_error("Invalid tessellator geometry output topology");
            }
        }
        for (u32 stream = 0; stream < Shader::GsMaxOutputStreams; ++stream)
            geometry.out_primitive[stream] = regs.vgt_gs_out_prim_type.GetPrimitiveType(stream);
        geometry.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
        geometry.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
        geometry.mode = regs.vgt_gs_mode.mode;
        geometry.vs_copy = copy.code;
        geometry.vs_copy_hash = copy.hash;
    } else if (!tessellation) {
        draw.vertex_runtime.hw.vs.user_clip_plane_mask =
            regs.clipper_control.user_clip_plane_enable;
        draw.vertex_runtime.hw.vs.emulate_depth_negative_one_to_one =
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        draw.vertex_runtime.hw.vs.clip_disable = regs.IsClipDisabled();
    }
    if (tessellation) {
        if (regs.primitive_type != AmdGpu::PrimitiveType::PatchPrimitive ||
            regs.clipper_control.user_clip_plane_enable)
            throw std::runtime_error("Invalid native tessellation primitive or clip state");
        draw.hull = hs.Params();
        draw.domain = ds.Params();
        draw.hull_runtime.Initialize(Shader::HwStage::Hull, Shader::SwStage::TessellationControl);
        draw.domain_runtime.Initialize(has_geometry ? Shader::HwStage::Export
                                                    : Shader::HwStage::Vertex,
                                       Shader::SwStage::TessellationEval);
        CommonInfo(draw.hull_runtime, regs.hs_program);
        CommonInfo(draw.domain_runtime, has_geometry ? regs.es_program : regs.vs_program);
        auto& control = draw.hull_runtime.sw.tcs;
        control.num_input_control_points = regs.ls_hs_config.hs_input_control_points;
        control.num_threads = regs.ls_hs_config.hs_output_control_points;
        control.tess_type = regs.tess_config.type;
        control.offchip_lds_enable = regs.hs_program.settings.oc_lds_en;
        auto& evaluation = draw.domain_runtime.sw.tes;
        evaluation.tess_type = regs.tess_config.type;
        evaluation.tess_topology = regs.tess_config.topology;
        evaluation.tess_partitioning = regs.tess_config.partitioning;
        if (has_geometry) {
            draw.domain_runtime.hw.es.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        } else {
            draw.domain_runtime.hw.vs.emulate_depth_negative_one_to_one =
                regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
            draw.domain_runtime.hw.vs.clip_disable = regs.IsClipDisabled();
        }
    }
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
    draw.fragment_runtime.hw.fs.num_samples =
        regs.ps_input_addr.sample_coverage_ena && regs.ps_input_ena.sample_coverage_ena
            ? draw.samples
            : 1;
    draw.fragment_runtime.hw.fs.z_export_format = regs.z_export_format;
    draw.fragment_runtime.hw.fs.mrtz_mask =
        regs.depth_shader_control.z_export_enable |
        (regs.depth_shader_control.mask_export_enable << 2) |
        (regs.depth_shader_control.coverage_to_mask_enable << 3);
    for (u32 i = 0; i < regs.num_interp; ++i)
        draw.fragment_runtime.hw.fs.inputs[i] = {
            u16(regs.ps_inputs[i].input_offset), u16(regs.ps_inputs[i].use_default),
            u16(regs.ps_inputs[i].flat_shade), u16(regs.ps_inputs[i].default_value)};
    draw.blend.IndependentBlendEnable = TRUE;
    for (u32 i = 0; i < 8; ++i) {
        if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable)
            break;
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
        target.SrcBlendAlpha = AlphaBlendFactor(
            blend.separate_alpha_blend ? blend.alpha_src_factor : blend.color_src_factor);
        target.DestBlendAlpha = AlphaBlendFactor(
            blend.separate_alpha_blend ? blend.alpha_dst_factor : blend.color_dst_factor);
        target.BlendOpAlpha =
            BlendOperation(blend.separate_alpha_blend ? blend.alpha_func : blend.color_func);
        target.RenderTargetWriteMask = regs.color_target_mask.GetMask(i);
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
    }
    draw.blend_factor = regs.blend_constants;
    draw.border_colors = regs.ta_bc_base.Address();
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
    draw.depth.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                            D3D12_COMPARISON_FUNC_ALWAYS};
    draw.depth.BackFace = draw.depth.FrontFace;
    if (draw.depth.DepthEnable)
        draw.depth_image.emplace(regs.depth_buffer, regs.depth_view.NumSlices(),
                                 VAddr(regs.depth_htile_data_base.address) << 8,
                                 liverpool.last_db_extent, true);
    draw.depth_read_address = regs.depth_buffer.DepthAddress();
    draw.depth_base_layer = regs.depth_view.slice_start;
    const auto& vp = regs.viewports[0];
    const float xscale = regs.viewport_control.xscale_enable ? vp.xscale : 1;
    const float yscale = regs.viewport_control.yscale_enable ? vp.yscale : 1;
    const float xoffset = regs.viewport_control.xoffset_enable ? vp.xoffset : 0;
    const float yoffset = regs.viewport_control.yoffset_enable ? vp.yoffset : 0;
    const float zscale = regs.viewport_control.zscale_enable ? vp.zscale : 1;
    const float zoffset = regs.viewport_control.zoffset_enable ? vp.zoffset : 0;
    const float min_depth = regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW
                                ? zoffset - zscale
                                : zoffset;
    const float max_depth = zoffset + zscale;
    if (!std::isfinite(min_depth) || !std::isfinite(max_depth) || min_depth > max_depth)
        throw std::runtime_error("Invalid D3D12 guest viewport depth range");
    if (xscale <= 0 || yscale == 0 || !std::isfinite(xscale) || !std::isfinite(yscale) ||
        !std::isfinite(xoffset) || !std::isfinite(yoffset))
        throw std::runtime_error("D3D12 guest viewport orientation is not implemented");
    draw.flip_y = yscale > 0;
    draw.viewport = {
        xoffset - xscale,     yoffset - std::abs(yscale),      xscale * 2,
        std::abs(yscale) * 2, std::clamp(min_depth, 0.f, 1.f), std::clamp(max_depth, 0.f, 1.f)};
    if (regs.IsClipDisabled()) {
        draw.flip_y = true;
        draw.viewport.TopLeftX = draw.viewport.TopLeftY = 0;
        draw.viewport.Width = draw.viewport.Height = 16384;
    }
    if (polygon.enable_window_offset) {
        draw.viewport.TopLeftX += regs.window_offset.window_x_offset;
        draw.viewport.TopLeftY += regs.window_offset.window_y_offset;
    }
    const auto& scissor = regs.screen_scissor;
    const s16 wx =
        regs.window_scissor.window_offset_disable ? 0 : regs.window_offset.window_x_offset;
    const s16 wy =
        regs.window_scissor.window_offset_disable ? 0 : regs.window_offset.window_y_offset;
    const auto top_left = [](s16 screen, s16 window, s16 generic, s16 offset) {
        return std::max<s32>({0, screen, s16(window + offset), s16(generic + offset)});
    };
    const auto bottom_right = [](s16 screen, s16 window, s16 generic, s16 offset) {
        return std::max<s32>(0, std::min({screen, s16(window + offset), s16(generic + offset)}));
    };
    draw.scissor = {top_left(scissor.top_left_x, regs.window_scissor.top_left_x,
                             regs.generic_scissor.top_left_x, wx),
                    top_left(scissor.top_left_y, regs.window_scissor.top_left_y,
                             regs.generic_scissor.top_left_y, wy),
                    bottom_right(scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
                                 regs.generic_scissor.bottom_right_x, wx),
                    bottom_right(scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
                                 regs.generic_scissor.bottom_right_y, wy)};
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
    case AmdGpu::PrimitiveType::PatchPrimitive: {
        const auto points = regs.ls_hs_config.hs_input_control_points;
        if (!tessellation || !points || points > 32 || regs.enable_primitive_restart)
            throw std::runtime_error("Invalid native tessellation patch topology");
        draw.topology =
            D3D_PRIMITIVE_TOPOLOGY(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + points - 1);
        draw.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        break;
    }
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
    case AmdGpu::PrimitiveType::RectList:
        draw.rectangles = regs.primitive_type == AmdGpu::PrimitiveType::RectList;
        if (draw.rectangles && draw.rasterizer.FillMode == D3D12_FILL_MODE_WIREFRAME)
            throw std::runtime_error("Rectangle wireframe requires boundary edge emulation");
        if (draw.rectangles && has_geometry)
            throw std::runtime_error("D3D12 guest geometry cannot precede rectangle expansion");
        draw.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        draw.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        break;
    case AmdGpu::PrimitiveType::TriangleStrip:
        draw.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        draw.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        break;
    case AmdGpu::PrimitiveType::AdjLineList:
        draw.topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
        draw.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        break;
    case AmdGpu::PrimitiveType::AdjLineStrip:
        draw.topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ;
        draw.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        break;
    case AmdGpu::PrimitiveType::AdjTriangleList:
        draw.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ;
        draw.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        break;
    case AmdGpu::PrimitiveType::AdjTriangleStrip:
        draw.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
        draw.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        break;
    case AmdGpu::PrimitiveType::LineLoop:
        draw.topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        draw.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        break;
    case AmdGpu::PrimitiveType::TriangleFan:
        draw.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        draw.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        break;
    case AmdGpu::PrimitiveType::Polygon:
    case AmdGpu::PrimitiveType::QuadList:
    case AmdGpu::PrimitiveType::QuadStrip:
        if (has_geometry)
            throw std::runtime_error("D3D12 expanded primitives preceding geometry require lowering");
        draw.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
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
        if (regs.enable_primitive_restart &&
            (regs.primitive_type == AmdGpu::PrimitiveType::AdjLineStrip ||
             regs.primitive_type == AmdGpu::PrimitiveType::AdjTriangleStrip)) {
            const u32 cut = size == 2 ? 0xffff : UINT32_MAX;
            if (regs.primitive_restart_index != cut)
                throw std::runtime_error(
                    "D3D12 custom guest restart index requires primitive conversion");
            draw.strip_cut = size == 2 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF
                                       : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
        }
    } else
        draw.first_vertex = indirect ? indirect->first : regs.index_offset;
    const bool convert = regs.primitive_type == AmdGpu::PrimitiveType::LineLoop ||
                         regs.primitive_type == AmdGpu::PrimitiveType::TriangleFan ||
                         regs.primitive_type == AmdGpu::PrimitiveType::Polygon ||
                         regs.primitive_type == AmdGpu::PrimitiveType::QuadList ||
                         regs.primitive_type == AmdGpu::PrimitiveType::QuadStrip ||
                         (indexed && regs.enable_primitive_restart &&
                          regs.primitive_type != AmdGpu::PrimitiveType::AdjLineStrip &&
                          regs.primitive_type != AmdGpu::PrimitiveType::AdjTriangleStrip);
    if (convert) {
        if (draw.rasterizer.FillMode == D3D12_FILL_MODE_WIREFRAME &&
            (regs.primitive_type == AmdGpu::PrimitiveType::QuadList ||
             regs.primitive_type == AmdGpu::PrimitiveType::QuadStrip ||
             regs.primitive_type == AmdGpu::PrimitiveType::Polygon))
            throw std::runtime_error("D3D12 polygon wireframe requires edge emulation");
        if (draw.count > 4 * 1024 * 1024)
            throw std::runtime_error("Guest index conversion exceeds upload budget");
        std::vector<u32> indices(draw.count);
        if (indexed) {
            if (draw.index_format == DXGI_FORMAT_R32_UINT) {
                Read(draw.index_address,
                     {reinterpret_cast<u8*>(indices.data()), indices.size() * sizeof(u32)});
            } else {
                std::vector<u16> packed(draw.count);
                Read(draw.index_address,
                     {reinterpret_cast<u8*>(packed.data()), packed.size() * sizeof(u16)});
                std::copy(packed.begin(), packed.end(), indices.begin());
            }
        } else {
            for (u32 i = 0; i < draw.count; ++i)
                indices[i] = i;
            draw.base_vertex = draw.first_vertex;
        }
        const auto restart = indexed && regs.enable_primitive_restart
                                 ? std::optional<u32>{regs.primitive_restart_index}
                                 : std::nullopt;
        draw.converted_indices = ConvertPrimitiveIndices(regs.primitive_type, indices, restart);
        draw.count = draw.converted_indices.size();
        draw.strip_cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
        if (draw.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE)
            draw.topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        else if (draw.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE)
            draw.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
    runtime.Draw(draw);
}
void Rasterizer::DrawIndirect(bool indexed, VAddr address, u32 offset, u32 stride, u32 count,
                              VAddr count_address) {
    DrawIndirect(indexed, address, offset, stride, count, count_address, 0, 0);
}
void Rasterizer::DrawIndirect(bool indexed, VAddr address, u32 offset, u32 stride, u32 count,
                              VAddr count_address, u16 vertex_sgpr, u16 instance_sgpr) {
    Execute([&] {
        DrawIndirectImpl(indexed, address, offset, stride, count, count_address, vertex_sgpr,
                         instance_sgpr);
    });
}
void Rasterizer::DrawIndirectImpl(bool indexed, VAddr address, u32 offset, u32 stride, u32 count,
                                  VAddr count_address, u16 vertex_sgpr, u16 instance_sgpr) {
    Core::MemoryManager::GuestCopyScope scope{&memory};
    const u32 bytes = indexed ? 20 : 16;
    if (count > 4096 || (count > 1 && (stride < bytes || stride % 4)) ||
        address > UINT64_MAX - offset)
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
        DrawImpl(indexed, 0,
                 IndirectDraw{args[0], args[1], args[2], indexed ? args[4] : args[3],
                              indexed ? std::bit_cast<s32>(args[3]) : 0, vertex_sgpr,
                              instance_sgpr});
    }
}
void Rasterizer::FillBuffer(VAddr address, u32 size, u32 value, bool use_gds) {
    Execute([&] { FillBufferImpl(address, size, value, use_gds); });
}
void Rasterizer::FillBufferImpl(VAddr address, u32 size, u32 value, bool use_gds) {
    const auto gds = runtime.Gds();
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
    const auto gds = runtime.Gds();
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
    const auto gds = runtime.Gds();
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
u64 Rasterizer::Flush() {
    u64 ticket{};
    Execute([&] { ticket = runtime.Flush(); });
    return ticket;
}
void Rasterizer::Finish() {
    static_cast<void>(Flush());
}
void Rasterizer::CpSync() {
    Finish();
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
