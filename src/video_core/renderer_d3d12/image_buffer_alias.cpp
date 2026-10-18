#include "video_core/renderer_d3d12/image_buffer_alias.h"

#include <algorithm>
#include <bit>
#include <optional>
#include <stdexcept>
#include "shader_recompiler/ir/passes/buffer_format.h"
#include "shader_recompiler/ir/passes/resource_pass.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/ir/reinterpret.h"
#include "video_core/renderer_d3d12/resource_cache.h"
#include "video_core/renderer_d3d12/shader_cache.h"
#include "video_core/texture_cache/cpu_tiling.h"

namespace D3D12 {
namespace {
namespace IR = Shader::IR;
namespace Optimization = Shader::Optimization;

struct ImageAlias {
    VideoCore::ImageInfo info;
    AmdGpu::Image sharp;
    Shader::ImageResource resource;
    u32 data, addresses;
};

u32 AddBuffer(Shader::Info& info, VAddr address, u32 size, bool written, Shader::BufferType type) {
    if (info.buffers.size() == Shader::NUM_BUFFERS || address >= (1ULL << 40))
        throw std::runtime_error("Shared image exceeds shader buffer capacity");
    AmdGpu::Buffer sharp{};
    sharp.base_address = address;
    sharp.num_records = size;
    const u32 index = info.buffers.size();
    info.buffers.push_back({
        .sharp_fetch = {.immediates = std::bit_cast<std::array<u32, 4>>(sharp), .load_mask = 0},
        .used_types =
            type == Shader::BufferType::Guest ? IR::Type::U32 | IR::Type::F32 : IR::Type::U32,
        .buffer_type = type,
        .is_written = written,
        .is_coherent = type == Shader::BufferType::Guest,
    });
    return index;
}

bool ImageInstruction(IR::Opcode opcode) {
    return opcode >= IR::Opcode::ImageSampleRaw && opcode <= IR::Opcode::ImageAtomicCmpSwap32;
}

IR::U32 Mip(IR::IREmitter& ir, const ImageAlias& image, const IR::Value& lod) {
    const u32 base = image.sharp.base_level + (image.resource.mip_fallback_mode ==
                                                       Shader::MipStorageFallbackMode::ConstantIndex
                                                   ? image.resource.constant_mip_index
                                                   : 0);
    return lod.IsEmpty() ? ir.Imm32(base) : IR::U32{ir.IAdd(IR::U32{lod}, ir.Imm32(base))};
}

IR::U32 Address(IR::IREmitter& ir, const ImageAlias& image, const IR::Value& coords,
                const IR::Value& lod, const IR::Value& sample) {
    const auto type = image.sharp.GetViewType(image.resource.is_array);
    const bool one_d =
        type == AmdGpu::ImageType::Color1D || type == AmdGpu::ImageType::Color1DArray;
    const bool array =
        type == AmdGpu::ImageType::Color1DArray || type == AmdGpu::ImageType::Color2DArray ||
        type == AmdGpu::ImageType::Color2DMsaaArray || type == AmdGpu::ImageType::Cube;
    const auto x = type == AmdGpu::ImageType::Color1D ? IR::U32{coords}
                                                      : IR::U32{ir.CompositeExtract(coords, 0)};
    const auto y = one_d ? ir.Imm32(0u) : IR::U32{ir.CompositeExtract(coords, 1)};
    const auto slice = array || image.info.props.is_volume
                           ? IR::U32{ir.CompositeExtract(coords, one_d ? 1 : 2)}
                           : ir.Imm32(0u);
    const auto mip = Mip(ir, image, lod);
    auto valid = ir.ILessThan(mip, ir.Imm32(image.info.resources.levels), false);
    if (!lod.IsEmpty())
        valid = ir.LogicalAnd(valid, ir.ILessThan(
            IR::U32{lod}, ir.Imm32(image.sharp.NumViewLevels(image.resource.is_array)), false));
    valid =
        ir.LogicalAnd(valid, ir.ILessThan(mip, ir.Imm32(u32(image.sharp.last_level) + 1), false));
    if (!image.info.props.is_volume)
        valid = ir.LogicalAnd(
            valid,
            ir.ILessThan(slice,
                         ir.Imm32(array ? image.sharp.NumViewLayers(image.resource.is_array) : 1u),
                         false));

    IR::U32 index = ir.Imm32(0u);
    u32 first{};
    for (u32 level = 0; level < image.info.resources.levels; ++level) {
        const auto& layout = image.info.mips_layout[level];
        const auto selected = ir.IEqual(mip, ir.Imm32(level));
        auto inside = ir.LogicalAnd(
            ir.ILessThan(x, ir.Imm32(std::max(1u, image.info.size.width >> level)), false),
            ir.ILessThan(y, ir.Imm32(std::max(1u, image.info.size.height >> level)), false));
        inside = ir.LogicalAnd(inside, ir.LogicalAnd(
            ir.ILessThan(x, ir.Imm32(u32(layout.pitch)), false),
            ir.ILessThan(y, ir.Imm32(u32(layout.height)), false)));
        if (image.info.props.is_volume)
            inside = ir.LogicalAnd(
                inside,
                ir.ILessThan(slice, ir.Imm32(std::max(1u, image.info.size.depth >> level)), false));
        valid = ir.LogicalAnd(valid, ir.LogicalOr(ir.LogicalNot(selected), inside));
        const auto absolute_slice = image.info.props.is_volume
                                        ? slice
                                        : IR::U32{ir.IAdd(slice, ir.Imm32(image.sharp.base_array))};
        const auto sample_index =
            sample.IsEmpty()
                ? ir.Imm32(0u)
                : IR::U32{ir.BitwiseAnd(IR::U32{sample}, ir.Imm32(image.info.num_samples - 1))};
        const IR::U32 plane{
            ir.IAdd(ir.IMul(absolute_slice, ir.Imm32(image.info.num_samples)), sample_index)};
        const u32 planes = layout.size /
                           (u64(layout.pitch) * layout.height * (image.info.num_bits / 8));
        valid = ir.LogicalAnd(valid, ir.LogicalOr(
            ir.LogicalNot(selected), ir.ILessThan(plane, ir.Imm32(planes), false)));
        const IR::U32 row{ir.IAdd(ir.IMul(plane, ir.Imm32(u32(layout.height))), y)};
        const IR::U32 offset{
            ir.IAdd(ir.IAdd(ir.IMul(row, ir.Imm32(u32(layout.pitch))), x), ir.Imm32(first))};
        index = IR::U32{ir.Select(selected, offset, index)};
        first += layout.size / (image.info.num_bits / 8);
    }
    const IR::U32 safe_index{ir.Select(valid, index, ir.Imm32(0u))};
    const auto byte_offset = ir.LoadBufferU32(1, ir.Imm32(image.addresses), safe_index, {});
    return IR::U32{ir.Select(valid, byte_offset, ir.Imm32(0xffff0000u))};
}

Optimization::FormatInfo Format(const ImageAlias& image) {
    const auto number = image.sharp.GetNumberFmt();
    const auto format = image.sharp.GetDataFmt();
    const auto components = AmdGpu::NumComponents(format);
    auto swizzle = AmdGpu::IdentityMapping;
    if (components < 4)
        swizzle.a = AmdGpu::CompSwizzle::One;
    if (components < 3)
        swizzle.b = AmdGpu::CompSwizzle::Zero;
    if (components < 2)
        swizzle.g = AmdGpu::CompSwizzle::Zero;
    return {format, number == AmdGpu::NumberFormat::Srgb ? AmdGpu::NumberFormat::Unorm : number,
            swizzle, AmdGpu::NumberConversion::None, components};
}

IR::Value Dimensions(IR::IREmitter& ir, const ImageAlias& image, const IR::Inst& inst) {
    const auto type = image.sharp.GetViewType(image.resource.is_array);
    const auto mip =
        Mip(ir, image,
            image.resource.is_written || image.resource.is_feedback ? IR::Value{} : inst.Arg(1));
    const auto shift = ir.UMin(mip, ir.Imm32(15u));
    const auto dimension = [&](u32 size) {
        return ir.UMax(IR::U32{ir.ShiftRightLogical(ir.Imm32(size), shift)}, ir.Imm32(1u));
    };
    const auto width = dimension(image.info.size.width);
    const auto height = dimension(image.info.size.height);
    const auto layers = ir.Imm32(image.sharp.NumViewLayers(image.resource.is_array));
    const auto levels =
        ir.Imm32(inst.Arg(2).U1() ? image.sharp.NumViewLevels(image.resource.is_array) : 0u);
    const auto zero = ir.Imm32(0u);
    switch (type) {
    case AmdGpu::ImageType::Color1D:
        return ir.CompositeConstruct(width, zero, zero, levels);
    case AmdGpu::ImageType::Color1DArray:
        return ir.CompositeConstruct(width, layers, zero, levels);
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
        return ir.CompositeConstruct(width, height, zero, levels);
    case AmdGpu::ImageType::Color3D:
        return ir.CompositeConstruct(width, height, dimension(image.info.size.depth), levels);
    default:
        return ir.CompositeConstruct(width, height, layers, levels);
    }
}

IR::Value Atomic(IR::IREmitter& ir, IR::Inst& inst, IR::Value handle, IR::U32 address) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::ImageAtomicIAdd32:
        return ir.BufferAtomicIAdd(handle, address, inst.Arg(2), {});
    case IR::Opcode::ImageAtomicSMin32:
    case IR::Opcode::ImageAtomicUMin32:
        return ir.BufferAtomicIMin(handle, address, inst.Arg(2),
                                   inst.GetOpcode() == IR::Opcode::ImageAtomicSMin32, {});
    case IR::Opcode::ImageAtomicSMax32:
    case IR::Opcode::ImageAtomicUMax32:
        return ir.BufferAtomicIMax(handle, address, inst.Arg(2),
                                   inst.GetOpcode() == IR::Opcode::ImageAtomicSMax32, {});
    case IR::Opcode::ImageAtomicAnd32:
        return ir.BufferAtomicAnd(handle, address, inst.Arg(2), {});
    case IR::Opcode::ImageAtomicOr32:
        return ir.BufferAtomicOr(handle, address, inst.Arg(2), {});
    case IR::Opcode::ImageAtomicXor32:
        return ir.BufferAtomicXor(handle, address, inst.Arg(2), {});
    case IR::Opcode::ImageAtomicExchange32:
        return ir.BufferAtomicSwap(handle, address, inst.Arg(2), {});
    case IR::Opcode::ImageAtomicCmpSwap32:
        return ir.BufferAtomicCmpSwap(handle, address, inst.Arg(2), inst.Arg(3), {});
    default:
        throw std::runtime_error("Shared image atomic requires an equivalent raw buffer operation");
    }
}
} // namespace

bool LowerImageBufferAliases(CompiledShader& shader, IR::Program& program,
                             const ResourceCache* resources) {
    auto& info = shader.info;
    const auto overlaps = [&](VAddr first, u64 size, VAddr second, u64 second_size) {
        return resources ? resources->Aliases(first, size, second, second_size)
                         : size && second_size && first <= UINT64_MAX - size &&
                               second <= UINT64_MAX - second_size && first < second + second_size &&
                               second < first + size;
    };
    std::vector<bool> selected(info.images.size());
    std::vector<VideoCore::ImageInfo> layouts;
    for (const auto& image : info.images)
        layouts.emplace_back(image.GetSharp(info), image);
    const u32 original_buffers = info.buffers.size();
    for (u32 i = 0; i < info.images.size(); ++i) {
        for (const auto& buffer : info.buffers) {
            if (buffer.buffer_type != Shader::BufferType::Guest ||
                !(buffer.is_written || info.images[i].is_written))
                continue;
            const auto sharp = buffer.GetSharp(info);
            if (overlaps(layouts[i].guest_address, layouts[i].guest_size, sharp.base_address,
                         u64(sharp.num_records) * std::max<u32>(1, sharp.stride)))
                selected[i] = true;
        }
    }
    if (std::ranges::none_of(selected, [](bool value) { return value; }))
        return false;
    for (bool changed = true; changed;) {
        changed = false;
        for (u32 i = 0; i < selected.size(); ++i)
            for (u32 j = 0; j < selected.size(); ++j)
                if (!selected[i] && selected[j] &&
                    overlaps(layouts[i].guest_address, layouts[i].guest_size,
                             layouts[j].guest_address, layouts[j].guest_size)) {
                    selected[i] = true;
                    changed = true;
                }
    }
    std::vector<bool> coherent(original_buffers);
    std::vector<std::optional<ImageAlias>> aliases(info.images.size());
    u64 table_bytes{};
    for (u32 i = 0; i < selected.size(); ++i) {
        if (!selected[i])
            continue;
        const auto& layout = layouts[i];
        const auto sharp = info.images[i].GetSharp(info);
        if (info.images[i].is_depth || layout.props.is_block)
            throw std::runtime_error(
                "Shared depth or compressed image views require raw decoding");
        switch (sharp.GetDataFmt()) {
        case AmdGpu::DataFormat::Format8:
        case AmdGpu::DataFormat::Format8_8:
        case AmdGpu::DataFormat::Format16:
        case AmdGpu::DataFormat::Format8_8_8_8:
        case AmdGpu::DataFormat::Format16_16:
        case AmdGpu::DataFormat::Format16_16_16_16:
        case AmdGpu::DataFormat::Format32:
        case AmdGpu::DataFormat::Format32_32:
        case AmdGpu::DataFormat::Format32_32_32:
        case AmdGpu::DataFormat::Format32_32_32_32:
        case AmdGpu::DataFormat::Format10_11_11:
        case AmdGpu::DataFormat::Format2_10_10_10:
            break;
        default:
            throw std::runtime_error("Unsupported shared image storage format");
        }
        for (u32 b = 0; b < original_buffers; ++b) {
            auto& buffer = info.buffers[b];
            const auto buffer_sharp = buffer.GetSharp(info);
            if (buffer.buffer_type == Shader::BufferType::Guest &&
                overlaps(layout.guest_address, layout.guest_size, buffer_sharp.base_address,
                         u64(buffer_sharp.num_records) * std::max<u32>(1, buffer_sharp.stride))) {
                buffer.is_coherent = true;
                coherent[b] = true;
            }
        }
        const u64 bytes = u64(layout.guest_size) / (layout.num_bits / 8) * sizeof(u32);
        if (info.buffers.size() + 2 > Shader::NUM_BUFFERS ||
            table_bytes + bytes > 64ULL * 1024 * 1024)
            throw std::runtime_error("Shared image address tables exceed shader resource budget");
        table_bytes += bytes;
        auto table = VideoCore::BuildImageAddressTable(layout);
        const auto resource = info.images[i];
        const u32 data = AddBuffer(info, layout.guest_address, layout.guest_size,
                                   resource.is_written, Shader::BufferType::Guest);
        const u32 addresses = AddBuffer(info, 1, table.size() * sizeof(u32), false,
                                        Shader::BufferType::ImageAddressTable);
        shader.image_address_tables.emplace(addresses, std::move(table));
        aliases[i] = ImageAlias{layout, sharp, resource, data, addresses};
    }
    std::vector<u32> remap(info.images.size());
    u32 retained{};
    for (u32 i = 0; i < selected.size(); ++i)
        if (!selected[i])
            remap[i] = retained++;
    for (auto* block : program.blocks) {
        for (auto& inst : block->Instructions()) {
            const auto opcode = inst.GetOpcode();
            if (Optimization::IsBufferInstruction(inst) && inst.Arg(0).IsImmediate() &&
                inst.Arg(0).U32() < original_buffers && coherent[inst.Arg(0).U32()]) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                ir.DeviceMemoryBarrier();
            }
            if (!ImageInstruction(opcode))
                continue;
            const u32 handle = inst.Arg(0).U32();
            const u32 index = handle & 0xffff;
            if (!aliases[index]) {
                inst.SetArg(0, IR::Value{(handle & 0xffff0000) | remap[index]});
                continue;
            }
            const auto& image = *aliases[index];
            IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
            if (opcode == IR::Opcode::ImageQueryDimensions) {
                inst.ReplaceUsesWithAndRemove(Dimensions(ir, image, inst));
                continue;
            }
            if (opcode != IR::Opcode::ImageRead && opcode != IR::Opcode::ImageWrite &&
                !Optimization::IsImageAtomicInstruction(inst))
                throw std::runtime_error(
                    "Shared image sampling/query requires raw shader lowering");
            const auto lod = opcode == IR::Opcode::ImageRead || opcode == IR::Opcode::ImageWrite
                                 ? inst.Arg(2)
                                 : IR::Value{};
            const auto sample = opcode == IR::Opcode::ImageRead || opcode == IR::Opcode::ImageWrite
                                    ? inst.Arg(3)
                                    : IR::Value{};
            const auto byte_address = Address(ir, image, inst.Arg(1), lod, sample);
            const auto address =
                ir.ShiftRightLogical(byte_address, ir.Imm32(image.info.num_bits == 8    ? 0u
                                                            : image.info.num_bits == 16 ? 1u
                                                                                        : 2u));
            const auto data = ir.Imm32(image.data);
            ir.DeviceMemoryBarrier();
            if (opcode == IR::Opcode::ImageRead) {
                auto value =
                    Optimization::LoadBufferFormat(ir, data, IR::U32{address}, {}, Format(image));
                const auto in_bounds =
                    ir.ILessThan(byte_address, ir.Imm32(image.info.guest_size), false);
                const auto component = [&](u32 index) {
                    return ir.Select(in_bounds, ir.CompositeExtract(value, index), ir.Imm32(0.f));
                };
                value = ir.CompositeConstruct(component(0), component(1), component(2), component(3));
                if (!image.resource.is_written && !image.resource.is_feedback) {
                    if (image.sharp.GetNumberFmt() == AmdGpu::NumberFormat::Srgb)
                        value = IR::ApplyForceDegamma(ir, value);
                    value = IR::ApplySwizzle(ir, value, image.sharp.DstSelect(),
                                             AmdGpu::IsInteger(image.sharp.GetNumberFmt()));
                }
                inst.ReplaceUsesWithAndRemove(value);
            } else if (opcode == IR::Opcode::ImageWrite) {
                auto format = Format(image);
                format.swizzle = AmdGpu::IdentityMapping;
                Optimization::StoreBufferFormat(ir, data, IR::U32{address}, inst.Arg(4), {},
                                                format);
                inst.Invalidate();
            } else {
                if (image.info.num_bits != 32 ||
                    image.sharp.GetDataFmt() != AmdGpu::DataFormat::Format32)
                    throw std::runtime_error("Shared image atomics require one 32-bit component");
                inst.ReplaceUsesWithAndRemove(Atomic(ir, inst, data, IR::U32{address}));
            }
        }
    }
    for (u32 i = info.images.size(); i > 0; --i)
        if (selected[i - 1])
            info.images.erase(info.images.begin() + i - 1);
    return true;
}
} // namespace D3D12
