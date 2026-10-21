#include "video_core/renderer_d3d12/d3d12_rasterizer.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
#include "common/hash.h"
#include "common/logging/log.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_d3d12/primitive_conversion.h"
#include "video_core/renderer_d3d12/profile.h"

namespace D3D12 {
namespace {
Profile profile;
struct Program {
    std::array<u32, 16> user_data;
    std::vector<u32> code;
    u64 hash;
    VAddr address;
    Shader::ShaderParams Params() const {
        return {user_data, code, hash, address};
    }
};
Program ReadProgramImpl(Core::MemoryManager& memory, const auto& regs);
Program ReadProgram(Core::MemoryManager& memory, const auto& regs) {
    return ProfileSample(profile, "read_programs", 384,
                         [&] { return ReadProgramImpl(memory, regs); });
}
Program ReadProgramImpl(Core::MemoryManager& memory, const auto& regs) {
    const VAddr address = regs.template Address<VAddr>();
    if (!address)
        throw std::runtime_error("Guest shader program address is null");
    std::array<u32, 2> prefix{};
    ProfileSample(profile, "prog_header", 384, [&] {
        memory.CopySparseMemory(address, reinterpret_cast<u8*>(prefix.data()), sizeof(prefix));
    });
    AmdGpu::BinaryInfo info{};
    bool found{};
    if (prefix[0] == 0xbeeb03ff) {
        const u64 offset = (u64(prefix[1]) + 1) * 8;
        if (offset > 16 * 1024 * 1024 || address > UINT64_MAX - offset)
            throw std::runtime_error("Guest shader header offset exceeds compiler budget");
        memory.CopySparseMemory(address + offset, reinterpret_cast<u8*>(&info), sizeof(info));
        found = info.Valid();
    }
    if (!found) {
        std::vector<u32> header(0x4000 + sizeof(AmdGpu::BinaryInfo) / 4);
        ProfileSample(profile, "prog_header_scan", 384, [&] {
            memory.CopySparseMemory(address, reinterpret_cast<u8*>(header.data()),
                                    header.size() * sizeof(u32));
        });
        for (u32 index = 0; index < 0x4000; ++index) {
            std::memcpy(&info, header.data() + index, sizeof(info));
            if (info.Valid()) {
                found = true;
                break;
            }
        }
    }
    if (!found || !info.length || info.length % 4 || info.length > 4 * 1024 * 1024)
        throw std::runtime_error("Guest shader binary metadata is invalid");
    Program result{regs.user_data, std::vector<u32>(info.length / 4), info.shader_hash, address};
    ProfileSample(profile, "prog_code", 384, [&] {
        memory.CopySparseMemory(address, reinterpret_cast<u8*>(result.code.data()), info.length);
    });
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
D3D12_FILL_MODE FillMode(AmdGpu::PolygonMode mode) {
    return mode == AmdGpu::PolygonMode::Line ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
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
void NormalizeMinMaxBlend(D3D12_BLEND_OP& operation, D3D12_BLEND& source,
                          D3D12_BLEND& destination, AmdGpu::NumberFormat format) {
    if (format != AmdGpu::NumberFormat::Unorm && format != AmdGpu::NumberFormat::Srgb)
        return;
    if (operation != D3D12_BLEND_OP_MIN && operation != D3D12_BLEND_OP_MAX)
        return;
    if ((source != D3D12_BLEND_ZERO && source != D3D12_BLEND_ONE) ||
        (destination != D3D12_BLEND_ZERO && destination != D3D12_BLEND_ONE) ||
        (source != D3D12_BLEND_ZERO && destination != D3D12_BLEND_ZERO))
        return;
    if (operation == D3D12_BLEND_OP_MIN)
        source = destination = D3D12_BLEND_ZERO;
    operation = D3D12_BLEND_OP_ADD;
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
D3D12_STENCIL_OP StencilOp(AmdGpu::StencilFunc op) {
    using F = AmdGpu::StencilFunc;
    switch (op) {
    case F::Keep:
        return D3D12_STENCIL_OP_KEEP;
    case F::Zero:
        return D3D12_STENCIL_OP_ZERO;
    case F::ReplaceTest:
    case F::ReplaceOp:
    case F::Ones:
        return D3D12_STENCIL_OP_REPLACE;
    case F::AddClamp:
        return D3D12_STENCIL_OP_INCR_SAT;
    case F::SubClamp:
        return D3D12_STENCIL_OP_DECR_SAT;
    case F::Invert:
        return D3D12_STENCIL_OP_INVERT;
    case F::AddWrap:
        return D3D12_STENCIL_OP_INCR;
    case F::SubWrap:
        return D3D12_STENCIL_OP_DECR;
    default:
        throw std::runtime_error("Guest stencil op has no D3D12 mapping");
    }
}
D3D12_LOGIC_OP LogicOp(AmdGpu::ColorControl::LogicOp op) {
    using Op = AmdGpu::ColorControl::LogicOp;
    switch (op) {
    case Op::Clear:
        return D3D12_LOGIC_OP_CLEAR;
    case Op::Nor:
        return D3D12_LOGIC_OP_NOR;
    case Op::AndInverted:
        return D3D12_LOGIC_OP_AND_INVERTED;
    case Op::CopyInverted:
        return D3D12_LOGIC_OP_COPY_INVERTED;
    case Op::AndReverse:
        return D3D12_LOGIC_OP_AND_REVERSE;
    case Op::Invert:
        return D3D12_LOGIC_OP_INVERT;
    case Op::BrushXor:
    case Op::Xor:
        return D3D12_LOGIC_OP_XOR;
    case Op::Nand:
        return D3D12_LOGIC_OP_NAND;
    case Op::And:
        return D3D12_LOGIC_OP_AND;
    case Op::Equiv:
        return D3D12_LOGIC_OP_EQUIV;
    case Op::Noop:
        return D3D12_LOGIC_OP_NOOP;
    case Op::OrInverted:
        return D3D12_LOGIC_OP_OR_INVERTED;
    case Op::Copy:
        return D3D12_LOGIC_OP_COPY;
    case Op::OrReverse:
        return D3D12_LOGIC_OP_OR_REVERSE;
    case Op::Or:
        return D3D12_LOGIC_OP_OR;
    case Op::Set:
        return D3D12_LOGIC_OP_SET;
    default:
        throw std::runtime_error("Invalid guest logic operation");
    }
}
D3D12_COMPARISON_FUNC InvertedDepthFunc(D3D12_COMPARISON_FUNC func) {
    switch (func) {
    case D3D12_COMPARISON_FUNC_NEVER:
        return D3D12_COMPARISON_FUNC_ALWAYS;
    case D3D12_COMPARISON_FUNC_LESS:
        return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case D3D12_COMPARISON_FUNC_EQUAL:
        return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case D3D12_COMPARISON_FUNC_LESS_EQUAL:
        return D3D12_COMPARISON_FUNC_GREATER;
    case D3D12_COMPARISON_FUNC_GREATER:
        return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case D3D12_COMPARISON_FUNC_NOT_EQUAL:
        return D3D12_COMPARISON_FUNC_EQUAL;
    case D3D12_COMPARISON_FUNC_GREATER_EQUAL:
        return D3D12_COMPARISON_FUNC_LESS;
    default:
        throw std::runtime_error("Invalid guest inverted depth function");
    }
}
u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    using Shader::Output;
    u32 num_outputs = 0;
    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[num_outputs++];
        misc_vec[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc_vec[1] = ctl.use_vtx_edge_flag
                          ? Output::EdgeFlag
                          : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc_vec[2] =
            ctl.use_vtx_kill_flag
                ? Output::KillFlag
                : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }
    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[num_outputs++];
        ccdist0[0] = ctl.IsClipDistEnabled(0)
                         ? Output::ClipDist0
                         : (ctl.IsCullDistEnabled(0) ? Output::CullDist0 : Output::None);
        ccdist0[1] = ctl.IsClipDistEnabled(1)
                         ? Output::ClipDist1
                         : (ctl.IsCullDistEnabled(1) ? Output::CullDist1 : Output::None);
        ccdist0[2] = ctl.IsClipDistEnabled(2)
                         ? Output::ClipDist2
                         : (ctl.IsCullDistEnabled(2) ? Output::CullDist2 : Output::None);
        ccdist0[3] = ctl.IsClipDistEnabled(3)
                         ? Output::ClipDist3
                         : (ctl.IsCullDistEnabled(3) ? Output::CullDist3 : Output::None);
    }
    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[num_outputs++];
        ccdist1[0] = ctl.IsClipDistEnabled(4)
                         ? Output::ClipDist4
                         : (ctl.IsCullDistEnabled(4) ? Output::CullDist4 : Output::None);
        ccdist1[1] = ctl.IsClipDistEnabled(5)
                         ? Output::ClipDist5
                         : (ctl.IsCullDistEnabled(5) ? Output::CullDist5 : Output::None);
        ccdist1[2] = ctl.IsClipDistEnabled(6)
                         ? Output::ClipDist6
                         : (ctl.IsCullDistEnabled(6) ? Output::CullDist6 : Output::None);
        ccdist1[3] = ctl.IsClipDistEnabled(7)
                         ? Output::ClipDist7
                         : (ctl.IsCullDistEnabled(7) ? Output::CullDist7 : Output::None);
    }
    return num_outputs;
}
} // namespace
namespace {
std::vector<ResourceCache::Range> ResolveBacking(Core::MemoryManager& memory, VAddr address,
                                                 u64 size) {
    if (!Core::MemoryManager::tls_in_guest_copy_scope)
        throw std::runtime_error("D3D12 backing resolution requires a guest memory lease");
    if (!size || size > 256ULL * 1024 * 1024 || address > UINT64_MAX - size)
        throw std::runtime_error("Invalid D3D12 backing range");
    std::vector<Core::MemoryManager::BackingSpan> spans((size + 4095) / 4096 + 1);
    const u32 count = memory.ResolveBackingSpans(address, size, spans.data(), spans.size(), false);
    if (!count)
        throw std::runtime_error("Guest GPU range lacks mapped physical backing");
    std::vector<ResourceCache::Range> result;
    u64 total{};
    for (u32 i = 0; i < count; ++i) {
        const auto start = reinterpret_cast<VAddr>(spans[i].ptr);
        if (!spans[i].size || spans[i].size > size - total || start > UINT64_MAX - spans[i].size)
            throw std::runtime_error("Invalid D3D12 physical backing span");
        for (const auto& previous : result)
            if (start < previous.address + previous.size &&
                previous.address < start + spans[i].size)
                throw std::runtime_error("D3D12 resource contains repeated physical backing");
        if (!result.empty() && result.back().address + result.back().size == start)
            result.back().size += spans[i].size;
        else
            result.push_back({start, spans[i].size});
        total += spans[i].size;
    }
    if (total != size)
        throw std::runtime_error("D3D12 physical backing resolution is incomplete");
    return result;
}

/**
 * Resolves the guest base address of each enabled stream-0 stream-out buffer from the
 * geometry stage user data. GNM addresses stream-out buffers through buffer descriptors
 * placed in stage user data (GFX6-VI has no VGT_STRMOUT_BUFFER_BASE registers); the
 * descriptor slot convention is not documented, so candidates are matched by stride
 * against the SO vertex stride register and every draw with a new configuration dumps
 * its full user data so a real game trace can confirm or correct the convention.
 */
std::array<VAddr, 4> ResolveStreamOutBases(
    std::span<const u32, Shader::ShaderParams::NumShaderUserData> user_data, u32 buffer_mask,
    u32 vtx_stride) {
    static constexpr u32 sharp_dwords = sizeof(AmdGpu::Buffer) / sizeof(u32);
    struct SoCandidate {
        u32 offset{};
        AmdGpu::Buffer sharp;
    };
    std::vector<SoCandidate> candidates;
    for (u32 index = 0; index + sharp_dwords <= user_data.size(); ++index) {
        AmdGpu::Buffer sharp;
        std::memcpy(&sharp, user_data.data() + index, sizeof(sharp));
        if (sharp.Valid() && sharp.base_address && sharp.num_records)
            candidates.push_back({index, sharp});
    }
    const u64 slots = std::popcount(buffer_mask);
    static std::mutex dump_mutex;
    static std::unordered_set<u64> dumped;
    const u64 user_data_hash = [&] {
        u64 hash = user_data.size();
        for (const u32 word : user_data)
            hash = HashCombine(hash, u64{word});
        return hash;
    }();
    bool first_config{false};
    {
        std::scoped_lock lock{dump_mutex};
        first_config = dumped.insert(user_data_hash).second;
    }
    const auto dump_user_data = [&]() -> std::string {
        if (!first_config)
            return "user data unchanged";
        std::string words;
        for (u32 i = 0; i < user_data.size(); ++i)
            words += fmt::format(" {}:{:#x}", i, user_data[i]);
        return words;
    };
    const auto dump_candidates = [&] {
        std::string result;
        for (const auto& candidate : candidates) {
            result +=
                fmt::format(" ud[{},{})={:#x} records={} stride={}", candidate.offset,
                            candidate.offset + sharp_dwords, u64(candidate.sharp.base_address),
                            candidate.sharp.num_records, candidate.sharp.stride);
        }
        return result;
    };
    // The stride register unit (dwords or bytes) is trace-resolved; a matching
    // descriptor accepts either interpretation.
    const u64 stride_bytes = u64(vtx_stride) * 4;
    const auto stride_score = [&](const AmdGpu::Buffer& sharp) {
        return std::min(
            std::min(u64(sharp.stride) > stride_bytes ? u64(sharp.stride) - stride_bytes
                                                      : stride_bytes - u64(sharp.stride),
                     u64(sharp.stride) > u64(vtx_stride) ? u64(sharp.stride) - vtx_stride
                                                         : u64(vtx_stride) - u64(sharp.stride)),
            UINT64_MAX);
    };
    std::sort(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) {
        const u64 score_a = stride_score(a.sharp);
        const u64 score_b = stride_score(b.sharp);
        return score_a != score_b ? score_a < score_b : a.offset < b.offset;
    });
    std::array<VAddr, 4> bases{};
    u32 assigned{};
    for (const auto& candidate : candidates) {
        if (assigned == slots)
            break;
        bases[assigned++] = candidate.sharp.base_address;
    }
    if (assigned != slots)
        throw std::runtime_error(fmt::format(
            "Guest stream-out buffers lack descriptor candidates: mask={:#x} stride={} slots={} "
            "candidates{} user_data{}",
            buffer_mask, vtx_stride, slots, dump_candidates(), dump_user_data()));
    LOG_INFO(
        Render_Vulkan,
        "Guest stream-out capture: mask={:#x} stride={} assigned{} user_data{} candidates{}",
        buffer_mask, vtx_stride,
        [&] {
            std::string words;
            for (u32 i = 0; i < assigned; ++i)
                words += fmt::format(" {}: {:#x}", i, bases[i]);
            return words;
        }(),
        dump_user_data(), dump_candidates());
    return bases;
}
} // namespace
Rasterizer::Rasterizer(AmdGpu::Liverpool& gpu, Core::MemoryManager& manager)
    : liverpool{gpu}, memory{manager} {
    runtime.Initialize([this](VAddr address, std::span<u8> data) { Read(address, data); },
                       [this](VAddr address, std::span<const u8> data) { Write(address, data); },
                       [this](VAddr address, u64 size) -> std::optional<VAddr> {
                           const auto spans = ResolveBacking(memory, address, size);
                           return spans.size() == 1 ? std::optional<VAddr>{spans[0].address}
                                                    : std::nullopt;
                       },
                       [this](VAddr first, u64 first_size, VAddr second, u64 second_size) {
                           const auto a = ResolveBacking(memory, first, first_size);
                           const auto b = ResolveBacking(memory, second, second_size);
                           for (const auto& left : a)
                               for (const auto& right : b)
                                   if (left.address < right.address + right.size &&
                                       right.address < left.address + left.size)
                                       return true;
                           return false;
                       });
    D3D12_FEATURE_DATA_D3D12_OPTIONS13 options{};
    alpha_blend_factor_supported =
        SUCCEEDED(runtime.Device().Device()->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS13, &options, sizeof(options))) &&
        options.AlphaBlendFactorSupported;
    runtime.SetMemoryAccess([this](std::span<const ResourceCache::Range> ranges) {
        std::vector<std::pair<VAddr, u64>> access;
        access.reserve(ranges.size());
        for (const auto& range : ranges)
            access.emplace_back(range.address, range.size);
        return memory.AcquireGpuAccess(access);
    });
    runtime.Resources().SetBackingResolver(
        [this](VAddr address, u64 size) { return ResolveBacking(memory, address, size); });
    memory.SetRasterizer(this);
    try {
        memory.EnableGpuImageTracking(true);
        runtime.SetImageResidency([this](std::span<const ImageCache::Watch> watches) {
            std::vector<Core::MemoryManager::GpuImageWatch> ranges;
            for (const auto& watch : watches)
                ranges.push_back({watch.address, watch.size, watch.dirty});
            memory.SetGpuImageWatches(ranges);
        });
        runtime.EnableAsync(
            [this](VAddr address, u64 size) { return ResolveBacking(memory, address, size); },
            [this](std::span<const ResourceCache::Range> ranges) {
                std::vector<std::pair<VAddr, u64>> access;
                access.reserve(ranges.size());
                for (const auto& range : ranges)
                    access.emplace_back(range.address, range.size);
                return memory.AcquireAsyncGpuAccess(access);
            });
    } catch (...) {
        memory.EnableGpuImageTracking(false);
        memory.SetRasterizer(nullptr);
        throw;
    }
}
void Rasterizer::SetBackground(bool background) {
    Core::MemoryManager::GuestCopyScope scope{&memory};
    runtime.SetBackground(background);
}
Rasterizer::~Rasterizer() {
    try {
        Core::MemoryManager::GuestCopyScope scope{&memory};
        runtime.SetImageResidency({});
    } catch (...) {
    }
    memory.EnableGpuImageTracking(false);
    memory.SetRasterizer(nullptr);
}
std::shared_ptr<VideoCore::ResidentFrame> Rasterizer::CapturePresentation(
    const VideoCore::ImageInfo& info) {
    Core::MemoryManager::GuestCopyScope scope{&memory};
    return runtime.CapturePresentation(info);
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
void Rasterizer::Dispatch(std::array<u32, 3> groups, VAddr indirect_address) {
    if (!logged_dispatch) {
        logged_dispatch = true;
        LOG_INFO(Core, "[UWP stage] first native dispatch");
    }
    Core::MemoryManager::GuestCopyScope scope{&memory};
    const auto& cs = liverpool.GetCsRegs();
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
    info.hw.cs.start_offsets = {cs.start_x, cs.start_y, cs.start_z};
    info.hw.cs.shared_memory_size = cs.SharedMemSize();
    runtime.Dispatch(program.Params(), info, groups, liverpool.regs.ta_bc_base.Address(), indirect_address);
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
    if (address + offset && !((address + offset) & 3)) {
        try {
            Dispatch({1, 1, 1}, address + offset);
            return;
        } catch (const GpuDrawFallback&) {
        }
    }
    std::array<u32, 3> groups;
    Read(address + offset, {reinterpret_cast<u8*>(groups.data()), sizeof(groups)});
    Dispatch(groups);
}
void Rasterizer::Draw(bool indexed, u32 index_offset) {
    Execute([&] {
        ProfileSample(profile, "draw", 128, [&] {
            try {
                DrawImpl(indexed, index_offset, {});
            } catch (const GpuDrawFallback&) {
                DrawImpl(indexed, index_offset, {}, false);
            }
        });
    });
}
void Rasterizer::DrawImpl(bool indexed, u32 index_offset, std::optional<IndirectDraw> indirect,
                          bool gpu_conversion) {
    if (!logged_draw) {
        logged_draw = true;
        LOG_INFO(Core, "[UWP stage] first native draw");
    }
    Core::MemoryManager::GuestCopyScope scope{&memory};
    const auto& regs = liverpool.regs;
    if (indirect && indirect->gpu &&
        regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Normal &&
        regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Disable)
        throw GpuDrawFallback{};
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        for (u32 i = 0; i < 2; ++i)
            if (regs.color_buffers[i])
                runtime.MaterializeColorClear(
                    regs.color_buffers[i],
                    VideoCore::ImageInfo{regs.color_buffers[i], liverpool.last_cb_extent[i]});
        const VideoCore::ImageInfo source{regs.color_buffers[0], liverpool.last_cb_extent[0]};
        const VideoCore::ImageInfo destination{regs.color_buffers[1], liverpool.last_cb_extent[1]};
        runtime.Resolve(source, destination, regs.color_buffers[0].BaseSlice(),
                        regs.color_buffers[1].BaseSlice());
        return;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::EliminateFastClear ||
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::FmaskDecompress) {
        for (u32 i = 0; i < std::size(regs.color_buffers); ++i)
            if (regs.color_buffers[i])
                runtime.MaterializeColorClear(
                    regs.color_buffers[i],
                    VideoCore::ImageInfo{regs.color_buffers[i], liverpool.last_cb_extent[i]});
        return;
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None)
        return;
    if (regs.num_interp > 32)
        throw std::runtime_error("Guest fragment interpolation count exceeds register bounds");
    const bool stencil_test_enable =
        regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid();
    const bool depth_bounds = regs.depth_buffer.DepthValid() &&
                              (regs.depth_control.depth_enable || stencil_test_enable) &&
                              regs.depth_control.depth_bounds_enable &&
                              !(regs.depth_bounds_min <= 0.f && regs.depth_bounds_max >= 1.f);
    if (regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Normal &&
        regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Disable)
        throw std::runtime_error(fmt::format(
            "Unsupported native D3D12 graphics: geometry={} local={} stencil={} bounds={} "
            "depth_fail_color={} depth_pass_disable={} mode={} rop={:#x}",
            regs.stage_enable.IsStageEnabled(u32(Shader::HwStage::Geometry)),
            regs.stage_enable.IsStageEnabled(u32(Shader::HwStage::Local)),
            bool(stencil_test_enable), bool(regs.depth_control.depth_bounds_enable),
            bool(regs.depth_control.enable_color_writes_on_depth_fail),
            bool(regs.depth_control.disable_color_writes_on_depth_pass),
            u32(regs.color_control.mode), u32(regs.color_control.rop3)));
    const bool on_depth_fail = regs.depth_control.enable_color_writes_on_depth_fail != 0u;
    const bool off_depth_pass = regs.depth_control.disable_color_writes_on_depth_pass != 0u;
    const bool depth_resolved = regs.depth_control.depth_enable && regs.depth_buffer.DepthValid();
    const bool depth_all_pass =
        !depth_resolved || regs.depth_control.depth_func == AmdGpu::CompareFunc::Always;
    const bool depth_all_fail =
        depth_resolved && regs.depth_control.depth_func == AmdGpu::CompareFunc::Never;
    bool color_writes_unconditional{};
    bool color_writes_suppressed{};
    bool depth_gated{};
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Normal &&
        (on_depth_fail || off_depth_pass)) {
        if (depth_all_pass) {
            color_writes_suppressed = off_depth_pass;
        } else if (depth_all_fail) {
            if (on_depth_fail) {
                if (stencil_test_enable)
                    throw std::runtime_error(
                        "Guest depth-fail color writes with stencil exceeded D3D12 "
                        "output merger state");
                color_writes_unconditional = true;
            } else {
                color_writes_suppressed = true;
            }
        } else {
            depth_gated = true;
            color_writes_suppressed = off_depth_pass;
        }
    }
    const bool has_geometry = regs.stage_enable.IsStageEnabled(u32(Shader::HwStage::Geometry));
    const bool tessellation = regs.stage_enable.IsStageEnabled(u32(Shader::HwStage::Local));
    u32 strmout_buffer_mask{};
    if (has_geometry && regs.vgt_gs_mode.onchip)
        throw std::runtime_error("D3D12 on-chip geometry is not implemented");
    if (has_geometry && regs.vgt_strmout_config.raw) {
        const auto& config = regs.vgt_strmout_config;
        if (config.streamout_1_en || config.streamout_2_en || config.streamout_3_en ||
            config.rast_stream != 0 || (config.use_rast_stream_mask && config.rast_stream_mask))
            throw std::runtime_error("Guest stream-out beyond stream 0 exceeds D3D12 support");
        const u32 buffer_mask = regs.vgt_strmout_buffer_config.stream_0_buf_en;
        if (!regs.vgt_strmout_buffers[0].vtx_stride)
            throw std::runtime_error("Guest stream-out buffers require a vertex stride");
        strmout_buffer_mask = buffer_mask;
    }
    std::array<VAddr, 4> strmout_bases{};
    const u32 clip_cull_distances =
        std::popcount(u32(regs.vs_output_control.clip_distance_enable)) +
        std::popcount(u32(regs.vs_output_control.cull_distance_enable));
    if (clip_cull_distances > 8)
        throw std::runtime_error("D3D12 guest clip/cull distance count exceeds output limits");
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
        if (iterations > draw.samples)
            throw std::runtime_error("Guest sample shading iterations exceed guest samples");
        draw.sample_shading = iterations > 1;
        draw.alpha_to_coverage =
            (values[0xa2dc] & 1) && !regs.depth_shader_control.alpha_to_mask_disable;
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
        geometry.emulate_depth_negative_one_to_one =
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        geometry.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
        geometry.num_outputs = MapOutputs(geometry.outputs, regs.vs_output_control);
        geometry.vs_copy = copy.code;
        geometry.vs_copy_hash = copy.hash;
        geometry.strmout_buffer_mask = strmout_buffer_mask;
        geometry.strmout_stride = regs.vgt_strmout_buffers[0].vtx_stride;
        if (strmout_buffer_mask) {
            strmout_bases = ResolveStreamOutBases(draw.geometry->user_data, strmout_buffer_mask,
                                                  regs.vgt_strmout_buffers[0].vtx_stride);
        }
    } else if (!tessellation) {
        draw.vertex_runtime.hw.vs.user_clip_plane_mask =
            regs.clipper_control.user_clip_plane_enable;
        draw.vertex_runtime.hw.vs.num_outputs =
            MapOutputs(draw.vertex_runtime.hw.vs.outputs, regs.vs_output_control);
        draw.vertex_runtime.hw.vs.emulate_depth_negative_one_to_one =
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        draw.vertex_runtime.hw.vs.clip_disable = regs.IsClipDisabled();
    }
    if (tessellation) {
        if (regs.primitive_type != AmdGpu::PrimitiveType::PatchPrimitive)
            throw std::runtime_error("Invalid native tessellation primitive");
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
            draw.domain_runtime.hw.vs.user_clip_plane_mask =
                regs.clipper_control.user_clip_plane_enable;
            draw.domain_runtime.hw.vs.num_outputs =
                MapOutputs(draw.domain_runtime.hw.vs.outputs, regs.vs_output_control);
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
    const bool logic_op = regs.color_control.rop3 != AmdGpu::ColorControl::LogicOp::Copy;
    for (u32 i = 0; i < 8; ++i) {
        if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable)
            break;
        const auto& color = regs.color_buffers[i];
        if (!color) {
            continue;
        }
        draw.colors.emplace_back(color, liverpool.last_cb_extent[i]);
        draw.color_clear_buffers.push_back(color);
        draw.color_slots.push_back(i);
        draw.color_base_slice.push_back(color.BaseSlice());
        draw.fragment_runtime.hw.fs.color_buffers[i] = {color.GetDataFmt(),
                                                        color.GetNumberFmt(),
                                                        color.GetNumberConversion(),
                                                        regs.color_export_format.GetFormat(i),
                                                        0,
                                                        color.Swizzle()};
        const auto& blend = regs.blend_control[i];
        auto& target = draw.blend.RenderTarget[i];
        if (logic_op) {
            target.BlendEnable = FALSE;
            target.LogicOpEnable = TRUE;
            target.LogicOp = LogicOp(regs.color_control.rop3);
        } else {
            target.BlendEnable = blend.enable && !color.info.blend_bypass;
            target.LogicOpEnable = FALSE;
            target.LogicOp = D3D12_LOGIC_OP_NOOP;
            target.SrcBlend = BlendFactor(blend.color_src_factor);
            target.DestBlend = BlendFactor(blend.color_dst_factor);
            target.BlendOp = BlendOperation(blend.color_func);
            target.SrcBlendAlpha = AlphaBlendFactor(
                blend.separate_alpha_blend ? blend.alpha_src_factor : blend.color_src_factor);
            target.DestBlendAlpha = AlphaBlendFactor(
                blend.separate_alpha_blend ? blend.alpha_dst_factor : blend.color_dst_factor);
            target.BlendOpAlpha =
                BlendOperation(blend.separate_alpha_blend ? blend.alpha_func : blend.color_func);
            NormalizeMinMaxBlend(target.BlendOp, target.SrcBlend, target.DestBlend,
                                 color.GetNumberFmt());
            NormalizeMinMaxBlend(target.BlendOpAlpha, target.SrcBlendAlpha, target.DestBlendAlpha,
                                 color.GetNumberFmt());
        }
        target.RenderTargetWriteMask =
            color_writes_suppressed ? 0u : regs.color_target_mask.GetMask(i);
    }
    draw.blend_factor = regs.blend_constants;
    bool constant_color_rgb{}, constant_alpha_rgb{};
    for (auto& target : draw.blend.RenderTarget) {
        const auto normalize_alpha = [](D3D12_BLEND& factor) {
            if (factor == D3D12_BLEND_ALPHA_FACTOR)
                factor = D3D12_BLEND_BLEND_FACTOR;
            else if (factor == D3D12_BLEND_INV_ALPHA_FACTOR)
                factor = D3D12_BLEND_INV_BLEND_FACTOR;
        };
        normalize_alpha(target.SrcBlendAlpha);
        normalize_alpha(target.DestBlendAlpha);
        for (auto* factor : {&target.SrcBlend, &target.DestBlend}) {
            if (target.BlendEnable && (target.RenderTargetWriteMask & 7)) {
                constant_color_rgb |= *factor == D3D12_BLEND_BLEND_FACTOR ||
                                      *factor == D3D12_BLEND_INV_BLEND_FACTOR;
                constant_alpha_rgb |= *factor == D3D12_BLEND_ALPHA_FACTOR ||
                                      *factor == D3D12_BLEND_INV_ALPHA_FACTOR;
            }
            if (!alpha_blend_factor_supported)
                normalize_alpha(*factor);
        }
    }
    if (!alpha_blend_factor_supported && constant_alpha_rgb) {
        if (constant_color_rgb)
            throw std::runtime_error(
                "D3D12 device cannot combine constant color and constant alpha RGB blending");
        draw.blend_factor.fill(regs.blend_constants[3]);
    }
    draw.border_colors = regs.ta_bc_base.Address();
    const auto& polygon = regs.polygon_control;
    if (polygon.cull_front && polygon.cull_back)
        return;
    AmdGpu::PolygonMode front_mode =
        polygon.enable_polygon_mode ? polygon.polygon_mode_front : AmdGpu::PolygonMode::Fill;
    AmdGpu::PolygonMode back_mode =
        polygon.enable_polygon_mode ? polygon.polygon_mode_back : AmdGpu::PolygonMode::Fill;
    const bool front_visible = !polygon.cull_front;
    const bool back_visible = !polygon.cull_back;
    AmdGpu::PolygonMode polygon_mode = front_visible ? front_mode : back_mode;
    const bool mixed_mode = front_visible && back_visible && front_mode != back_mode;
    const bool front_point = front_visible && front_mode == AmdGpu::PolygonMode::Point;
    const bool back_point = back_visible && back_mode == AmdGpu::PolygonMode::Point;
    const bool point_face = front_point || back_point;
    const float front_bias_offset =
        polygon.enable_polygon_offset_front ? regs.poly_offset.front_offset : 0.f;
    const float back_bias_offset =
        polygon.enable_polygon_offset_back ? regs.poly_offset.back_offset : 0.f;
    const float front_bias_scale =
        polygon.enable_polygon_offset_front ? regs.poly_offset.front_scale : 0.f;
    const float back_bias_scale =
        polygon.enable_polygon_offset_back ? regs.poly_offset.back_scale : 0.f;
    const bool bias_split = polygon.NeedsBias() && (front_bias_offset != back_bias_offset ||
                                                    front_bias_scale != back_bias_scale);
    const bool face_passes =
        mixed_mode || (point_face && (polygon.cull_front || polygon.cull_back)) || bias_split;
    const bool point_cpu = point_face && !face_passes;
    if (point_face && (tessellation || has_geometry))
        throw std::runtime_error("D3D12 point fill requires vertex-only primitive lowering");
    if (bias_split && (tessellation || has_geometry))
        throw std::runtime_error(
            "D3D12 per-face depth bias requires per-face passes conflicting with guest "
            "tessellation or geometry side effects");
    if (mixed_mode && (tessellation || has_geometry))
        throw std::runtime_error(
            "D3D12 mixed guest polygon modes require per-face passes conflicting with "
            "guest tessellation or geometry side effects");
    if ((mixed_mode || bias_split) && (regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
                                       regs.primitive_type == AmdGpu::PrimitiveType::Polygon ||
                                       regs.primitive_type == AmdGpu::PrimitiveType::QuadList ||
                                       regs.primitive_type == AmdGpu::PrimitiveType::QuadStrip))
        throw std::runtime_error(
            "D3D12 per-face guest fill or depth bias on polygon primitives requires per-face "
            "primitive lowering");
    const bool line_primitive =
        regs.primitive_type == AmdGpu::PrimitiveType::LineList ||
        regs.primitive_type == AmdGpu::PrimitiveType::LineStrip ||
        regs.primitive_type == AmdGpu::PrimitiveType::LineLoop ||
        regs.primitive_type == AmdGpu::PrimitiveType::AdjLineList ||
        regs.primitive_type == AmdGpu::PrimitiveType::AdjLineStrip ||
        (tessellation && regs.primitive_type == AmdGpu::PrimitiveType::PatchPrimitive &&
         regs.tess_config.topology == AmdGpu::TessellationTopology::Line);
    const float line_width = regs.line_control.Width();
    const float stipple_scale = regs.line_stipple_scale.Scale();
    const float line_pattern_scale =
        stipple_scale > 0.f && std::isfinite(stipple_scale) ? stipple_scale * 0.5f : 1.f;
    const bool line_stipple = regs.mode_control.line_stripple_enable;
    const bool line_expand = (line_width != 1.f || line_stipple) &&
                             (line_primitive || polygon_mode == AmdGpu::PolygonMode::Line);
    if (line_expand && has_geometry)
        throw std::runtime_error(
            "D3D12 wide or stippled guest lines require guest geometry lowering");
    draw.rasterizer.FillMode = FillMode(polygon_mode);
    draw.rasterizer.CullMode = polygon.cull_front  ? D3D12_CULL_MODE_FRONT
                               : polygon.cull_back ? D3D12_CULL_MODE_BACK
                                                   : D3D12_CULL_MODE_NONE;
    draw.rasterizer.FrontCounterClockwise =
        polygon.front_face == AmdGpu::FrontFace::CounterClockwise;
    draw.rasterizer.DepthClipEnable = regs.clipper_control.ZclipEnable();
    if (polygon.NeedsBias()) {
        const bool front = polygon.enable_polygon_offset_front;
        const float offset = front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset;
        const float scale = front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale;
        draw.rasterizer.DepthBias = static_cast<INT>(offset);
        draw.rasterizer.DepthBiasClamp = regs.poly_offset.depth_bias;
        draw.rasterizer.SlopeScaledDepthBias = scale / 16.f;
    }
    draw.provoking_last = polygon.provoking_vtx_last == AmdGpu::ProvokingVtxLast::Last;
    draw.depth.DepthEnable = regs.depth_control.depth_enable && regs.depth_buffer.DepthValid();
    draw.depth.DepthWriteMask = (draw.depth.DepthEnable && regs.depth_control.depth_write_enable &&
                                 !regs.depth_render_control.depth_clear_enable)
                                    ? D3D12_DEPTH_WRITE_MASK_ALL
                                    : D3D12_DEPTH_WRITE_MASK_ZERO;
    draw.depth.DepthFunc = D3D12_COMPARISON_FUNC(u32(regs.depth_control.depth_func) + 1);
    if (color_writes_unconditional) {
        draw.depth.DepthEnable = FALSE;
        draw.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    }
    draw.depth.StencilEnable = stencil_test_enable;
    draw.depth.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    draw.depth.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    draw.depth.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                            D3D12_COMPARISON_FUNC_ALWAYS};
    draw.depth.BackFace = draw.depth.FrontFace;
    draw.stencil_ref = 0;
    draw.dsv_flags = D3D12_DSV_FLAG_NONE;
    D3D12_DEPTH_STENCILOP_DESC guest_front{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                           D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    D3D12_DEPTH_STENCILOP_DESC guest_back = guest_front;
    u32 front_ref_value{}, back_ref_value{}, front_mask{}, back_mask{}, front_write{}, back_write{};
    if (stencil_test_enable) {
        const auto& sc = regs.stencil_control;
        guest_front = {StencilOp(sc.stencil_fail_front), StencilOp(sc.stencil_zfail_front),
                       StencilOp(sc.stencil_zpass_front),
                       D3D12_COMPARISON_FUNC(u32(regs.depth_control.stencil_ref_func) + 1)};
        if (regs.depth_control.backface_enable) {
            guest_back = {StencilOp(sc.stencil_fail_back), StencilOp(sc.stencil_zfail_back),
                          StencilOp(sc.stencil_zpass_back),
                          D3D12_COMPARISON_FUNC(u32(regs.depth_control.stencil_bf_func) + 1)};
        }
        const auto& front_ref = regs.stencil_ref_front;
        const auto& back_ref =
            regs.depth_control.backface_enable ? regs.stencil_ref_back : regs.stencil_ref_front;
        const auto uses_op_val = [](AmdGpu::StencilFunc fail, AmdGpu::StencilFunc zpass,
                                    AmdGpu::StencilFunc zfail) {
            return fail == AmdGpu::StencilFunc::ReplaceOp ||
                   zpass == AmdGpu::StencilFunc::ReplaceOp ||
                   zfail == AmdGpu::StencilFunc::ReplaceOp;
        };
        const bool front_op =
            uses_op_val(sc.stencil_fail_front, sc.stencil_zpass_front, sc.stencil_zfail_front);
        const bool back_op =
            regs.depth_control.backface_enable
                ? uses_op_val(sc.stencil_fail_back, sc.stencil_zpass_back, sc.stencil_zfail_back)
                : front_op;
        front_ref_value = front_op ? front_ref.stencil_op_val : front_ref.stencil_test_val;
        back_ref_value = back_op ? back_ref.stencil_op_val : back_ref.stencil_test_val;
        front_mask = front_ref.stencil_mask;
        back_mask = back_ref.stencil_mask;
        const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
        front_write = stencil_clear ? 0u : front_ref.stencil_write_mask;
        back_write = stencil_clear
                         ? 0u
                         : (regs.depth_control.backface_enable ? back_ref.stencil_write_mask
                                                               : front_ref.stencil_write_mask);
        if (!face_passes) {
            const bool back_matters = regs.depth_control.backface_enable && !polygon.cull_back;
            const auto consumes_ref = [](const D3D12_DEPTH_STENCILOP_DESC& face) {
                return (face.StencilFunc != D3D12_COMPARISON_FUNC_ALWAYS &&
                        face.StencilFunc != D3D12_COMPARISON_FUNC_NEVER) ||
                       face.StencilFailOp == D3D12_STENCIL_OP_REPLACE ||
                       face.StencilPassOp == D3D12_STENCIL_OP_REPLACE ||
                       face.StencilDepthFailOp == D3D12_STENCIL_OP_REPLACE;
            };
            const auto consumes_mask = [](const D3D12_DEPTH_STENCILOP_DESC& face) {
                return face.StencilFunc != D3D12_COMPARISON_FUNC_ALWAYS &&
                       face.StencilFunc != D3D12_COMPARISON_FUNC_NEVER;
            };
            u32 stencil_ref_value = front_ref_value;
            if (front_ref_value != back_ref_value) {
                const bool front_needs = consumes_ref(guest_front);
                const bool back_needs = back_matters && consumes_ref(guest_back);
                if (front_needs && back_needs)
                    throw std::runtime_error(
                        "D3D12 guest front/back stencil refs require a single value");
                stencil_ref_value = front_needs ? front_ref_value : back_ref_value;
            }
            u32 solo_write = front_write;
            u32 solo_read = front_mask;
            if (back_matters) {
                if (front_visible) {
                    if (back_write != front_write)
                        throw std::runtime_error(
                            "D3D12 guest front/back stencil write masks require a single value");
                    if (front_mask != back_mask &&
                        (consumes_mask(guest_front) || consumes_mask(guest_back)))
                        throw std::runtime_error(
                            "D3D12 guest front/back stencil compare masks require a single value");
                } else {
                    solo_write = back_write;
                    if (consumes_mask(guest_back))
                        solo_read = back_mask;
                }
            }
            draw.depth.FrontFace = guest_front;
            draw.depth.BackFace = guest_back;
            draw.depth.StencilReadMask = solo_read;
            draw.depth.StencilWriteMask = static_cast<UINT8>(solo_write);
            draw.stencil_ref = stencil_ref_value;
            if (regs.depth_view.stencil_read_only && solo_write != 0) {
                throw std::runtime_error("D3D12 guest stencil write with read-only view");
            }
        } else if (regs.depth_view.stencil_read_only && (front_write | back_write) != 0) {
            throw std::runtime_error("D3D12 guest stencil write with read-only view");
        }
        if (regs.depth_view.stencil_read_only) {
            draw.dsv_flags |= D3D12_DSV_FLAG_READ_ONLY_STENCIL;
        }
        if (regs.depth_view.z_read_only) {
            draw.dsv_flags |= D3D12_DSV_FLAG_READ_ONLY_DEPTH;
            draw.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        }
    }
    const auto fill_pass_stencil = [&](CommandRuntime::DrawRequest& pass, bool face_front) {
        pass.depth.FrontFace = face_front ? guest_front : guest_back;
        pass.depth.BackFace = pass.depth.FrontFace;
        pass.depth.StencilReadMask = face_front ? front_mask : back_mask;
        pass.depth.StencilWriteMask = static_cast<UINT8>(face_front ? front_write : back_write);
        pass.stencil_ref = face_front ? front_ref_value : back_ref_value;
    };
    if (draw.depth.DepthEnable || stencil_test_enable)
        draw.depth_image.emplace(regs.depth_buffer, regs.depth_view.NumSlices(),
                                 VAddr(regs.depth_htile_data_base.address) << 8,
                                 liverpool.last_db_extent, true);
    draw.depth_read_address = regs.depth_buffer.DepthAddress();
    draw.stencil_read_address = regs.depth_buffer.StencilAddress();
    draw.depth_base_layer = regs.depth_view.slice_start;
    draw.depth_bounds = depth_bounds;
    draw.bounds_min = regs.depth_bounds_min;
    draw.bounds_max = regs.depth_bounds_max;
    draw.clear_depth = regs.depth_render_control.depth_clear_enable &&
                       regs.depth_control.depth_enable && regs.depth_control.depth_write_enable;
    draw.clear_stencil = regs.depth_render_control.stencil_clear_enable;
    draw.depth_clear = regs.depth_clear;
    draw.stencil_clear = regs.stencil_clear;
    const auto& vp = regs.viewports[0];
    const bool clip_disabled = regs.IsClipDisabled();
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
    if (xscale == 0 || yscale == 0 || !std::isfinite(xscale) || !std::isfinite(yscale) ||
        !std::isfinite(xoffset) || !std::isfinite(yoffset))
        throw std::runtime_error("D3D12 guest viewport orientation is not implemented");
    draw.flip_y = yscale > 0;
    draw.flip_x = xscale < 0;
    draw.viewports[0] = {xoffset - std::abs(xscale),
                         yoffset - std::abs(yscale),
                         std::abs(xscale) * 2,
                         std::abs(yscale) * 2,
                         std::clamp(min_depth, 0.f, 1.f),
                         std::clamp(max_depth, 0.f, 1.f)};
    if (clip_disabled) {
        draw.flip_y = true;
        draw.flip_x = false;
        draw.viewports[0].TopLeftX = draw.viewports[0].TopLeftY = 0;
        draw.viewports[0].Width = draw.viewports[0].Height = 16384;
    }
    if (polygon.enable_window_offset) {
        draw.viewports[0].TopLeftX += regs.window_offset.window_x_offset;
        draw.viewports[0].TopLeftY += regs.window_offset.window_y_offset;
    }
    const bool multi_viewport = regs.vs_output_control.vs_out_misc_enable &&
                                regs.vs_output_control.use_vtx_viewport_idx && !clip_disabled;
    draw.viewport_count = multi_viewport ? AmdGpu::NUM_VIEWPORTS : 1;
    if (multi_viewport) {
        for (u32 index = 1; index < AmdGpu::NUM_VIEWPORTS; ++index) {
            const auto& ivp = regs.viewports[index];
            const float ixscale = regs.viewport_control.xscale_enable ? ivp.xscale : 1;
            const float iyscale = regs.viewport_control.yscale_enable ? ivp.yscale : 1;
            const float ixoffset = regs.viewport_control.xoffset_enable ? ivp.xoffset : 0;
            const float iyoffset = regs.viewport_control.yoffset_enable ? ivp.yoffset : 0;
            const float izscale = regs.viewport_control.zscale_enable ? ivp.zscale : 1;
            const float izoffset = regs.viewport_control.zoffset_enable ? ivp.zoffset : 0;
            const float imin_depth = regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW
                                         ? izoffset - izscale
                                         : izoffset;
            const float imax_depth = izoffset + izscale;
            D3D12_VIEWPORT out{};
            if (!std::isfinite(imin_depth) || !std::isfinite(imax_depth) ||
                imin_depth > imax_depth) {
                out.MinDepth = 0.f;
                out.MaxDepth = 1.f;
                draw.viewports[index] = out;
                continue;
            }
            out.MinDepth = std::clamp(imin_depth, 0.f, 1.f);
            out.MaxDepth = std::clamp(imax_depth, 0.f, 1.f);
            if (ixscale == 0 || iyscale == 0 || !std::isfinite(ixscale) ||
                !std::isfinite(iyscale) || !std::isfinite(ixoffset) || !std::isfinite(iyoffset)) {
                draw.viewports[index] = out;
                continue;
            }
            out = {ixoffset - std::abs(ixscale),
                   iyoffset - std::abs(iyscale),
                   std::abs(ixscale) * 2,
                   std::abs(iyscale) * 2,
                   out.MinDepth,
                   out.MaxDepth};
            if (polygon.enable_window_offset) {
                out.TopLeftX += regs.window_offset.window_x_offset;
                out.TopLeftY += regs.window_offset.window_y_offset;
            }
            draw.viewports[index] = out;
        }
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
    const auto clip_scissor = [&](u32 index) {
        D3D12_RECT rect{top_left(scissor.top_left_x, regs.window_scissor.top_left_x,
                                 regs.generic_scissor.top_left_x, wx),
                        top_left(scissor.top_left_y, regs.window_scissor.top_left_y,
                                 regs.generic_scissor.top_left_y, wy),
                        bottom_right(scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
                                     regs.generic_scissor.bottom_right_x, wx),
                        bottom_right(scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
                                     regs.generic_scissor.bottom_right_y, wy)};
        if (regs.mode_control.vport_scissor_enable) {
            const auto& vp_scissor = regs.viewport_scissors[index];
            rect.left = std::max<LONG>(rect.left, vp_scissor.top_left_x);
            rect.top = std::max<LONG>(rect.top, vp_scissor.top_left_y);
            rect.right = std::min<LONG>(rect.right, vp_scissor.bottom_right_x);
            rect.bottom = std::min<LONG>(rect.bottom, vp_scissor.bottom_right_y);
        }
        return rect;
    };
    draw.scissors[0] = clip_scissor(0);
    if (multi_viewport) {
        for (u32 index = 1; index < AmdGpu::NUM_VIEWPORTS; ++index)
            draw.scissors[index] = clip_scissor(index);
    }
    draw.push = {xoffset, yoffset, xscale, yscale, {}};
    static_assert(sizeof(regs.clip_user_data) == sizeof(draw.clip_planes));
    std::memcpy(draw.clip_planes.data(), regs.clip_user_data.data(), sizeof(draw.clip_planes));
    if (line_expand) {
        draw.line_width = line_width;
        if (line_stipple) {
            draw.line_pattern = u16(regs.line_stipple.Pattern());
            draw.line_pattern_repeat = u16(regs.line_stipple.RepeatCount());
            draw.line_pattern_msb = regs.line_stipple.PatternBitOrder() != 0;
            draw.line_pattern_scale = line_pattern_scale;
        }
        for (u32 k = 0; k < AmdGpu::NUM_VIEWPORTS; ++k) {
            draw.line_viewport_dims[k * 2] = draw.viewports[k].Width;
            draw.line_viewport_dims[k * 2 + 1] = draw.viewports[k].Height;
        }
    }
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
        draw.rectangles = regs.primitive_type == AmdGpu::PrimitiveType::RectList &&
                          polygon_mode == AmdGpu::PolygonMode::Fill;
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
            throw std::runtime_error(
                "D3D12 expanded primitives preceding geometry require lowering");
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
        const u32 size = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16 ? 2 : 4;
        const VAddr base = regs.index_base_address.Address();
        if (base > UINT64_MAX - u64(index_offset) * size)
            throw std::runtime_error("Guest index buffer address overflow");
        draw.index_address = base + u64(index_offset) * size;
        draw.index_format = size == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        draw.index_swap = regs.index_buffer_type.swap_mode;
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
    const bool polygonal = regs.primitive_type == AmdGpu::PrimitiveType::TriangleList ||
                           regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
                           regs.primitive_type == AmdGpu::PrimitiveType::TriangleStrip ||
                           regs.primitive_type == AmdGpu::PrimitiveType::TriangleFan ||
                           regs.primitive_type == AmdGpu::PrimitiveType::Polygon ||
                           regs.primitive_type == AmdGpu::PrimitiveType::QuadList ||
                           regs.primitive_type == AmdGpu::PrimitiveType::QuadStrip;
    const bool contour = polygon_mode == AmdGpu::PolygonMode::Line &&
                         (regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
                          regs.primitive_type == AmdGpu::PrimitiveType::Polygon ||
                          regs.primitive_type == AmdGpu::PrimitiveType::QuadList ||
                          regs.primitive_type == AmdGpu::PrimitiveType::QuadStrip);
    const bool remap_provoking =
        draw.provoking_last && !point_face && !contour &&
        std::any_of(regs.ps_inputs.data(), regs.ps_inputs.data() + regs.num_interp,
                    [](const auto& input) { return input.flat_shade; });
    if (remap_provoking && (has_geometry || tessellation))
        throw std::runtime_error(
            "D3D12 last-provoking vertex with flat shading and geometry or tessellation "
            "requires vertex reordering");
    if (remap_provoking) {
        switch (regs.primitive_type) {
        case AmdGpu::PrimitiveType::AdjLineList:
        case AmdGpu::PrimitiveType::AdjLineStrip:
        case AmdGpu::PrimitiveType::AdjTriangleList:
        case AmdGpu::PrimitiveType::AdjTriangleStrip:
            throw std::runtime_error(
                "D3D12 last-provoking vertex with flat shading and adjacency primitives requires "
                "vertex reordering");
        default:
            break;
        }
    }
    const bool provoking_lines =
        remap_provoking && (regs.primitive_type == AmdGpu::PrimitiveType::LineList ||
                            regs.primitive_type == AmdGpu::PrimitiveType::LineStrip);
    const bool wire_expand_conv =
        line_expand && !line_primitive && polygon_mode == AmdGpu::PolygonMode::Line;
    const bool convert = regs.primitive_type == AmdGpu::PrimitiveType::LineLoop ||
                         regs.primitive_type == AmdGpu::PrimitiveType::TriangleFan ||
                         regs.primitive_type == AmdGpu::PrimitiveType::Polygon ||
                         regs.primitive_type == AmdGpu::PrimitiveType::QuadList ||
                         regs.primitive_type == AmdGpu::PrimitiveType::QuadStrip || point_cpu ||
                         contour || provoking_lines || (remap_provoking && polygonal) ||
                         wire_expand_conv ||
                         (indexed && regs.enable_primitive_restart &&
                          regs.primitive_type != AmdGpu::PrimitiveType::AdjLineStrip &&
                          regs.primitive_type != AmdGpu::PrimitiveType::AdjTriangleStrip);
    if (indirect && indirect->gpu) {
        if (convert || has_geometry || tessellation || strmout_buffer_mask || face_passes ||
            depth_gated || point_face || line_expand ||
            (indexed && draw.index_swap != AmdGpu::IndexSwapMode::None) ||
            regs.enable_primitive_restart || draw.rectangles || draw.clear_depth ||
            draw.clear_stencil ||
            std::ranges::any_of(draw.color_clear_buffers, [](const auto& color) {
                return color.info.fast_clear && color.cmask_base_address;
            }))
            throw GpuDrawFallback{};
        draw.indirect = indirect->gpu;
    }
    if (convert && gpu_conversion && !indirect && !regs.enable_primitive_restart && !has_geometry &&
        !tessellation && !face_passes && !depth_gated && !point_face && !point_cpu && !contour &&
        !wire_expand_conv && !line_expand && !strmout_buffer_mask) {
        u32 mode{}, output{};
        using P = AmdGpu::PrimitiveType;
        switch (regs.primitive_type) {
        case P::LineLoop:
            mode = 1;
            output = draw.count > 1 ? draw.count * 2 : 0;
            break;
        case P::TriangleFan:
        case P::Polygon:
            mode = 2;
            output = draw.count > 2 ? (draw.count - 2) * 3 : 0;
            break;
        case P::QuadList:
            mode = 3;
            output = (draw.count / 4) * 6;
            break;
        case P::QuadStrip:
            mode = 4;
            output = draw.count > 3 ? ((draw.count - 2) / 2) * 6 : 0;
            break;
        case P::TriangleStrip:
            mode = 5;
            output = draw.count > 2 ? (draw.count - 2) * 3 : 0;
            break;
        case P::TriangleList:
            mode = 6;
            output = (draw.count / 3) * 3;
            break;
        case P::LineStrip:
            mode = 7;
            output = draw.count > 1 ? (draw.count - 1) * 2 : 0;
            break;
        case P::LineList:
            mode = 8;
            output = (draw.count / 2) * 2;
            break;
        default:
            break;
        }
        if (mode && draw.count <= 4 * 1024 * 1024 && output <= 4 * 1024 * 1024 &&
            (!indexed ||
             (draw.index_address % (draw.index_format == DXGI_FORMAT_R16_UINT ? 2 : 4) == 0 &&
              (draw.index_swap == AmdGpu::IndexSwapMode::None ||
               (u64(draw.count) * (draw.index_format == DXGI_FORMAT_R16_UINT ? 2 : 4)) %
                       (draw.index_swap == AmdGpu::IndexSwapMode::Swap16 ? 2 : 4) ==
                   0)))) {
            draw.source_index_count = draw.count;
            draw.expansion = GpuCommands::Expansion{mode, draw.count, output, remap_provoking};
            if (!indexed)
                draw.base_vertex = draw.first_vertex;
            draw.count = output;
            draw.strip_cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
            draw.topology = mode == 1 || mode == 7 || mode == 8
                                ? D3D_PRIMITIVE_TOPOLOGY_LINELIST
                                : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            draw.topology_type = mode == 1 || mode == 7 || mode == 8
                                     ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                     : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        }
    }
    if (!convert && gpu_conversion && !indirect && indexed &&
        draw.index_swap != AmdGpu::IndexSwapMode::None && !has_geometry && !tessellation &&
        !face_passes && !depth_gated && !line_expand && !strmout_buffer_mask &&
        !regs.enable_primitive_restart && draw.count <= 4 * 1024 * 1024) {
        const u32 bytes = draw.index_format == DXGI_FORMAT_R16_UINT ? 2 : 4;
        const u32 unit = draw.index_swap == AmdGpu::IndexSwapMode::Swap16 ? 2 : 4;
        if (draw.index_address % bytes == 0 && (u64(draw.count) * bytes) % unit == 0) {
            draw.source_index_count = draw.count;
            draw.expansion = GpuCommands::Expansion{9, draw.count, draw.count, false};
        }
    }
    if (convert && !draw.expansion) {
        if (draw.count > 4 * 1024 * 1024)
            throw std::runtime_error("Guest index conversion exceeds upload budget");
        std::vector<u32> indices(draw.count);
        if (indexed) {
            if (draw.index_format == DXGI_FORMAT_R32_UINT) {
                Read(draw.index_address,
                     {reinterpret_cast<u8*>(indices.data()), indices.size() * sizeof(u32)});
                SwapIndexBytes({reinterpret_cast<u8*>(indices.data()), indices.size() * 4},
                               regs.index_buffer_type.swap_mode);
            } else {
                std::vector<u16> packed(draw.count);
                Read(draw.index_address,
                     {reinterpret_cast<u8*>(packed.data()), packed.size() * sizeof(u16)});
                SwapIndexBytes({reinterpret_cast<u8*>(packed.data()), packed.size() * 2},
                               regs.index_buffer_type.swap_mode);
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
        if (point_cpu && polygonal) {
            draw.converted_indices = ConvertPolygonPoints(regs.primitive_type, indices, restart);
        } else if (contour) {
            if (has_geometry)
                throw std::runtime_error(
                    "D3D12 expanded primitives preceding geometry require lowering");
            draw.converted_indices = ConvertPolygonEdges(regs.primitive_type, indices, restart);
        } else {
            draw.converted_indices = ConvertPrimitiveIndices(regs.primitive_type, indices, restart, remap_provoking);
        }
        draw.count = draw.converted_indices.size();
        draw.strip_cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
        if (point_cpu && polygonal)
            draw.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        else if (contour)
            draw.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        if (draw.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE)
            draw.topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        else if (draw.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE)
            draw.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        else
            draw.topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    }
    if (remap_provoking) {
        auto& indices = draw.converted_indices;
        if (draw.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE) {
            for (size_t i = 0; i + 2 < indices.size(); i += 3) {
                const u32 provoker = indices[i + 2];
                indices[i + 2] = indices[i + 1];
                indices[i + 1] = indices[i];
                indices[i] = provoker;
            }
        } else if (draw.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE) {
            for (size_t i = 0; i + 1 < indices.size(); i += 2)
                std::swap(indices[i], indices[i + 1]);
        }
    }
    draw.depth_gated = depth_gated;
    const u64 stride_bytes = u64(draw.geometry_runtime.hw.gs.strmout_stride) * 4;
    if (strmout_buffer_mask) {
        const auto partitions = CommandRuntime::GsInvocations(draw.topology, draw.count);
        if (!partitions)
            throw std::runtime_error("Guest stream-out draw has no geometry partitions");
        const auto& gs_info = draw.geometry_runtime.hw.gs;
        const u64 worst_vertices = partitions * u64(draw.instances) * u64(gs_info.num_invocations) *
                                   u64(gs_info.output_vertices);
        u32 ordinal{};
        u32 mask = strmout_buffer_mask;
        while (mask) {
            const u32 buffer = std::countr_zero(mask);
            mask &= mask - 1;
            const u64 capacity_bytes = u64(regs.vgt_strmout_buffers[buffer].size) * 4;
            const u64 start_bytes = u64(regs.vgt_strmout_buffers[buffer].offset) * 4;
            if (start_bytes < capacity_bytes) {
                const u64 window = std::min({capacity_bytes - start_bytes,
                                             worst_vertices * stride_bytes, u64(16) * 1024 * 1024});
                draw.strmout[ordinal] = {strmout_bases[buffer] + start_bytes, window};
            }
            ++ordinal;
        }
    }
    const auto submit = [&](CommandRuntime::DrawRequest& pass) {
        const bool pass_lines = pass.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE ||
                                pass.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        if (line_expand && (pass_lines || pass.rasterizer.FillMode == D3D12_FILL_MODE_WIREFRAME)) {
            pass.line_keep_front = pass.rasterizer.CullMode != D3D12_CULL_MODE_FRONT;
            pass.rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
            pass.rasterizer.CullMode = D3D12_CULL_MODE_NONE;
            pass.line_expand = true;
            if (pass.depth.StencilEnable)
                pass.depth.BackFace = pass.depth.FrontFace;
        }
        if (depth_gated) {
            CommandRuntime::DrawRequest fail_pass{pass};
            fail_pass.strmout = {};
            fail_pass.depth.DepthFunc = InvertedDepthFunc(fail_pass.depth.DepthFunc);
            fail_pass.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            fail_pass.depth.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                         D3D12_STENCIL_OP_KEEP,
                                         fail_pass.depth.FrontFace.StencilFunc};
            fail_pass.depth.BackFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                        D3D12_STENCIL_OP_KEEP,
                                        fail_pass.depth.BackFace.StencilFunc};
            for (u32 i = 0; i < 8; ++i) {
                fail_pass.blend.RenderTarget[i].RenderTargetWriteMask =
                    regs.color_target_mask.GetMask(i);
            }
            runtime.Draw(fail_pass);
        }
        runtime.Draw(pass);
    };
    if (face_passes) {
        const auto build_pass = [&](CommandRuntime::DrawRequest& pass, bool face_front,
                                    AmdGpu::PolygonMode mode) {
            if (mode == AmdGpu::PolygonMode::Point) {
                pass.point_fill = true;
                pass.point_keep_front = face_front;
                pass.rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
                pass.rasterizer.CullMode = D3D12_CULL_MODE_NONE;
            } else {
                pass.rasterizer.FillMode = FillMode(mode);
                pass.rasterizer.CullMode =
                    face_front ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_FRONT;
            }
            if (stencil_test_enable)
                fill_pass_stencil(pass, face_front);
            if (bias_split) {
                const bool enabled = face_front ? polygon.enable_polygon_offset_front
                                                : polygon.enable_polygon_offset_back;
                pass.rasterizer.DepthBias = static_cast<INT>(
                    enabled ? (face_front ? front_bias_offset : back_bias_offset) : 0.f);
                pass.rasterizer.DepthBiasClamp = regs.poly_offset.depth_bias;
                pass.rasterizer.SlopeScaledDepthBias =
                    (enabled ? (face_front ? front_bias_scale : back_bias_scale) : 0.f) / 16.f;
            }
        };
        const bool dual = mixed_mode || (bias_split && !polygon.cull_front && !polygon.cull_back);
        if (dual) {
            build_pass(draw, true, mixed_mode ? front_mode : polygon_mode);
            CommandRuntime::DrawRequest back_pass{draw};
            build_pass(back_pass, false, mixed_mode ? back_mode : polygon_mode);
            submit(draw);
            submit(back_pass);
        } else {
            build_pass(draw, !polygon.cull_front, polygon_mode);
            submit(draw);
        }
    } else {
        submit(draw);
    }
    if (strmout_buffer_mask) {
        const u32 written = runtime.StrmoutFilledSize();
        u32 mask = strmout_buffer_mask;
        while (mask) {
            const u32 buffer = std::countr_zero(mask);
            mask &= mask - 1;
            const u64 capacity_vertices = u64(regs.vgt_strmout_buffers[buffer].size) /
                                          draw.geometry_runtime.hw.gs.strmout_stride;
            auto& filled = liverpool.regs.vgt_strmout_buffer_filled_size[buffer];
            filled = u32(std::min(u64(filled) + written, capacity_vertices));
        }
    }
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
    if (count && !(address + offset & 3) && !(count_address & 3)) {
        const auto first = address + offset;
        const u64 last = u64(count - 1) * stride;
        if (first > UINT64_MAX - last || first + last > UINT64_MAX - bytes)
            throw std::runtime_error("Guest indirect draw address overflow");
        const auto capacity = indexed ? liverpool.regs.max_index_size : 1u;
        const auto mapped = [&](VAddr base, u64 size) {
            if (!base || base > UINT64_MAX - size)
                return false;
            std::vector<Core::MemoryManager::BackingSpan> spans((size + 4095) / 4096 + 1);
            const auto length =
                memory.ResolveBackingSpans(base, size, spans.data(), spans.size(), false);
            u64 total{};
            for (u32 index = 0; index < length; ++index)
                total += spans[index].size;
            return total == size;
        };
        if (capacity && capacity <= 4 * 1024 * 1024 && last + bytes <= 16 * 1024 * 1024 &&
            mapped(first, last + bytes) && (!count_address || mapped(count_address, 4)) &&
            (!indexed || mapped(liverpool.regs.index_base_address.Address(),
                                u64(capacity) * (liverpool.regs.index_buffer_type.index_type ==
                                                         AmdGpu::IndexType::Index16
                                                     ? 2
                                                     : 4)))) {
            try {
                IndirectDraw draw{capacity, 1, 0, 0, 0, vertex_sgpr, instance_sgpr};
                draw.gpu = GpuCommands::Arguments{first, count_address, stride, count, indexed};
                DrawImpl(indexed, 0, draw);
                return;
            } catch (const GpuDrawFallback&) {
            }
        }
    }
    if (count_address) {
        u32 actual;
        Read(count_address, {reinterpret_cast<u8*>(&actual), sizeof(actual)});
        count = std::min(count, actual);
    }
    if (!count)
        return;
    const VAddr first = address + offset;
    const u64 last = u64(count - 1) * stride;
    if (first > UINT64_MAX - last || first + last > UINT64_MAX - (bytes - 1))
        throw std::runtime_error("Guest indirect draw address overflow");
    std::vector<std::array<u32, 5>> arguments(count);
    for (u32 draw = 0; draw < count; ++draw)
        Read(first + u64(draw) * stride, {reinterpret_cast<u8*>(arguments[draw].data()), bytes});
    for (const auto& args : arguments) {
        DrawImpl(indexed, 0,
                 IndirectDraw{args[0], args[1], args[2], indexed ? args[4] : args[3],
                              indexed ? std::bit_cast<s32>(args[3]) : 0, vertex_sgpr,
                              instance_sgpr},
                 false);
    }
}
void Rasterizer::FillBuffer(VAddr address, u32 size, u32 value, bool use_gds) {
    Execute([&] { FillBufferImpl(address, size, value, use_gds); });
}
void Rasterizer::FillBufferImpl(VAddr address, u32 size, u32 value, bool use_gds) {
    Core::MemoryManager::GuestCopyScope scope{&memory};
    const auto gds = runtime.Gds();
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
    const auto gds = runtime.Gds();
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
        Core::MemoryManager::GuestCopyScope scope{&memory};
        const auto gds = runtime.Gds();
        if (offset > gds.size() - 4)
            throw std::runtime_error("Guest GDS read range overflow");
        std::memcpy(&value, gds.data() + offset, 4);
    });
    return value;
}
std::shared_ptr<void> Rasterizer::AcquireCpuAccess(VAddr address, u64 size, bool write) {
    Core::MemoryManager::GuestCopyScope scope{&memory};
    return runtime.AcquireCpuAccess(address, size, write);
}
std::shared_ptr<void> Rasterizer::AcquireMemoryMappingAccess() {
    Core::MemoryManager::GuestCopyScope scope{&memory};
    return runtime.AcquireMemoryMappingAccess();
}
void Rasterizer::MapMemory(VAddr address, u64 size) {
    runtime.Resources().MapMemory(address, size);
}
u64 Rasterizer::Flush() {
    Core::MemoryManager::GuestCopyScope scope{&memory};
    u64 ticket{};
    Execute([&] { ProfileSample(profile, "flush", 64, [&] { ticket = runtime.Flush(); }); });
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
    Core::MemoryManager::GuestCopyScope scope{&memory};
    const auto access = runtime.AcquireCpuAccess(address, size, true);
    return runtime.Resources().InvalidateMemory(address, size, locks);
}
} // namespace D3D12
