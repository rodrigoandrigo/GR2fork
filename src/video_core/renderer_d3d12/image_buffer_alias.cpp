#include "video_core/renderer_d3d12/image_buffer_alias.h"

#include <algorithm>
#include <bit>
#include <iterator>
#include <optional>
#include <stdexcept>
#include "shader_recompiler/ir/passes/buffer_format.h"
#include "shader_recompiler/ir/passes/resource_pass.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/ir/reinterpret.h"
#include "video_core/renderer_d3d12/bptc.h"
#include "video_core/renderer_d3d12/image_cache.h"
#include "video_core/renderer_d3d12/msaa_transfer.h"
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
    u32 bptc_table_start{};
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
    return opcode >= IR::Opcode::ImageSampleRaw && opcode <= IR::Opcode::ImageAtomicFCmpSwap32;
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
        valid = ir.LogicalAnd(
            valid,
            ir.ILessThan(IR::U32{lod}, ir.Imm32(image.sharp.NumViewLevels(image.resource.is_array)),
                         false));
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
    const bool block_image = image.info.props.is_block;
    for (u32 level = 0; level < image.info.resources.levels; ++level) {
        const auto& layout = image.info.mips_layout[level];
        const u32 pitch_blocks = u32(layout.pitch) >> (block_image ? 2u : 0u);
        const u32 height_blocks = u32(layout.height) >> (block_image ? 2u : 0u);
        const auto selected = ir.IEqual(mip, ir.Imm32(level));
        auto inside = ir.LogicalAnd(
            ir.ILessThan(x, ir.Imm32(std::max(1u, image.info.size.width >> level)), false),
            ir.ILessThan(y, ir.Imm32(std::max(1u, image.info.size.height >> level)), false));
        inside = ir.LogicalAnd(inside,
                               ir.LogicalAnd(ir.ILessThan(x, ir.Imm32(u32(layout.pitch)), false),
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
        const u32 planes =
            layout.size / (u64(pitch_blocks) * height_blocks * (image.info.num_bits / 8));
        valid = ir.LogicalAnd(valid, ir.LogicalOr(ir.LogicalNot(selected),
                                                  ir.ILessThan(plane, ir.Imm32(planes), false)));
        const auto bx = block_image ? IR::U32{ir.ShiftRightLogical(x, ir.Imm32(2u))} : x;
        const auto by = block_image ? IR::U32{ir.ShiftRightLogical(y, ir.Imm32(2u))} : y;
        const IR::U32 row{ir.IAdd(ir.IMul(plane, ir.Imm32(height_blocks)), by)};
        const IR::U32 offset{
            ir.IAdd(ir.IAdd(ir.IMul(row, ir.Imm32(pitch_blocks)), bx), ir.Imm32(first))};
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

struct PackedChannels {
    std::array<u32, 4> bits{}, shifts{};
};

std::optional<PackedChannels> PackedFormat(AmdGpu::DataFormat format) {
    switch (format) {
    case AmdGpu::DataFormat::Format5_6_5:
        return PackedChannels{{5, 6, 5, 0}, {11, 5, 0, 0}};
    case AmdGpu::DataFormat::Format1_5_5_5:
        return PackedChannels{{5, 5, 5, 1}, {10, 5, 0, 15}};
    case AmdGpu::DataFormat::Format4_4_4_4:
        return PackedChannels{{4, 4, 4, 4}, {8, 4, 0, 12}};
    case AmdGpu::DataFormat::Format8_24:
        return PackedChannels{{24, 8, 0, 0}, {0, 24, 0, 0}};
    case AmdGpu::DataFormat::Format24_8:
        return PackedChannels{{8, 24, 0, 0}, {0, 8, 0, 0}};
    default:
        return {};
    }
}

IR::Value LoadTexel(IR::IREmitter& ir, const ImageAlias& image, IR::U32 address) {
    const auto format = Format(image);
    if (format.data_format == AmdGpu::DataFormat::FormatGB_GR ||
        format.data_format == AmdGpu::DataFormat::FormatBG_RG) {
        const auto raw = IR::U32{ir.LoadBufferU32(
            1, ir.Imm32(image.data), IR::U32{ir.ShiftRightLogical(address, ir.Imm32(1u))}, {})};
        const bool gb_gr = format.data_format == AmdGpu::DataFormat::FormatGB_GR;
        const auto green_shift =
            IR::U32{ir.IAdd(ir.IMul(ir.BitwiseAnd(address, ir.Imm32(1u)), ir.Imm32(16u)),
                            ir.Imm32(gb_gr ? 0u : 8u))};
        const auto channel = [&](IR::U32 shift) {
            return IR::F32{
                ir.FPMul(ir.ConvertUToF(32, 32, ir.BitFieldExtract(raw, shift, ir.Imm32(8u))),
                         ir.Imm32(1.f / 255.f))};
        };
        return ir.CompositeConstruct(channel(ir.Imm32(gb_gr ? 24u : 16u)), channel(green_shift),
                                     channel(ir.Imm32(gb_gr ? 8u : 0u)), ir.Imm32(1.f));
    }
    if (format.data_format == AmdGpu::DataFormat::FormatX24_8_32) {
        const auto depth = IR::F32{ir.LoadBufferF32(1, ir.Imm32(image.data), address, {})};
        const auto stencil_word =
            ir.LoadBufferU32(1, ir.Imm32(image.data), IR::U32{ir.IAdd(address, ir.Imm32(1u))}, {});
        const auto stencil =
            ir.BitCast<IR::F32>(IR::U32{ir.BitwiseAnd(IR::U32{stencil_word}, ir.Imm32(255u))});
        return ir.CompositeConstruct(depth, stencil, ir.Imm32(0.f), ir.Imm32(1.f));
    }
    const auto packed = PackedFormat(format.data_format);
    if (!packed && format.data_format != AmdGpu::DataFormat::Format5_9_9_9)
        return Optimization::LoadBufferFormat(ir, ir.Imm32(image.data), address, {}, format);
    const auto raw =
        image.info.num_bits == 16
            ? IR::U32{ir.UConvert(32, ir.LoadBufferU16(ir.Imm32(image.data), address, {}))}
            : IR::U32{ir.LoadBufferU32(1, ir.Imm32(image.data), address, {})};
    std::array<IR::F32, 4> channels{ir.Imm32(0.f), ir.Imm32(0.f), ir.Imm32(0.f), ir.Imm32(1.f)};
    if (AmdGpu::IsInteger(format.num_format))
        channels[3] = ir.BitCast<IR::F32>(ir.Imm32(1u));
    for (u32 c = 0; c < (packed ? 4u : 3u); ++c) {
        if (packed && !packed->bits[c])
            continue;
        const u32 bits = packed ? packed->bits[c] : 9u;
        const u32 shift = packed ? packed->shifts[c] : c * 9u;
        const bool signed_components = format.num_format == AmdGpu::NumberFormat::Sint ||
                                       format.num_format == AmdGpu::NumberFormat::Snorm;
        const auto value =
            ir.BitFieldExtract(raw, ir.Imm32(shift), ir.Imm32(bits), signed_components);
        const auto scale =
            packed ? ir.Imm32(1.f / float((1u << bits) - 1))
                   : ir.BitCast<IR::F32>(IR::U32{ir.ShiftLeftLogical(
                         ir.IAdd(ir.ShiftRightLogical(raw, ir.Imm32(27u)), ir.Imm32(103u)),
                         ir.Imm32(23u))});
        if (AmdGpu::IsInteger(format.num_format)) {
            channels[c] = ir.BitCast<IR::F32>(value);
        } else if (signed_components) {
            const auto limit = std::max(1u, (1u << (bits - 1)) - 1);
            channels[c] = IR::F32{ir.FPMax(
                ir.FPDiv(ir.ConvertSToF(32, 32, value), ir.Imm32(float(limit))), ir.Imm32(-1.f))};
        } else {
            channels[c] = IR::F32{ir.FPMul(ir.ConvertUToF(32, 32, value), scale)};
        }
    }
    return ir.CompositeConstruct(channels[0], channels[1], channels[2], channels[3]);
}

void StoreTexel(IR::IREmitter& ir, const ImageAlias& image, IR::U32 address, IR::Value value) {
    const auto data_format = image.sharp.GetDataFmt();
    if (data_format == AmdGpu::DataFormat::Format8_24 ||
        data_format == AmdGpu::DataFormat::Format24_8 ||
        data_format == AmdGpu::DataFormat::FormatX24_8_32 ||
        data_format == AmdGpu::DataFormat::Format5_9_9_9 ||
        data_format == AmdGpu::DataFormat::FormatGB_GR ||
        data_format == AmdGpu::DataFormat::FormatBG_RG)
        throw std::runtime_error("Guest data format does not support typed image stores");
    const auto packed = PackedFormat(data_format);
    if (!packed) {
        auto format = Format(image);
        format.swizzle = AmdGpu::IdentityMapping;
        Optimization::StoreBufferFormat(ir, ir.Imm32(image.data), address, value, {}, format);
        return;
    }
    auto raw = ir.Imm32(0u);
    for (u32 c = 0; c < 4; ++c) {
        if (!packed->bits[c])
            continue;
        const auto channel = IR::F32{ir.CompositeExtract(value, c)};
        IR::U32 integer;
        const auto number = image.sharp.GetNumberFmt();
        if (AmdGpu::IsInteger(number)) {
            integer = ir.BitCast<IR::U32>(channel);
        } else if (number == AmdGpu::NumberFormat::Snorm) {
            const auto clamped =
                IR::F32{ir.FPMin(ir.FPMax(channel, ir.Imm32(-1.f)), ir.Imm32(1.f))};
            const auto limit = std::max(1u, (1u << (packed->bits[c] - 1)) - 1);
            integer = ir.ConvertFToS(
                32, IR::F32{ir.FPRoundEven(IR::F32{ir.FPMul(clamped, ir.Imm32(float(limit)))})});
        } else {
            const auto scaled = IR::F32{
                ir.FPMul(ir.FPSaturate(channel), ir.Imm32(float((1u << packed->bits[c]) - 1)))};
            integer = ir.ConvertFToU(32, IR::F32{ir.FPRoundEven(scaled)});
        }
        integer = IR::U32{ir.BitwiseAnd(integer, ir.Imm32((1u << packed->bits[c]) - 1))};
        raw = IR::U32{ir.BitwiseOr(raw, ir.ShiftLeftLogical(integer, ir.Imm32(packed->shifts[c])))};
    }
    ir.StoreBufferU16(ir.Imm32(image.data), address, ir.UConvert(16, raw), {});
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
    case IR::Opcode::ImageAtomicFCmpSwap32:
        return ir.BufferAtomicFCmpSwap(handle, address, inst.Arg(2), inst.Arg(3), {});
    case IR::Opcode::ImageAtomicInc32:
        return ir.BufferAtomicInc(handle, address, inst.Arg(2), {});
    case IR::Opcode::ImageAtomicDec32:
        return ir.BufferAtomicDec(handle, address, inst.Arg(2), {});
    case IR::Opcode::ImageAtomicFMin32:
        return ir.BufferAtomicFMin(handle, address, inst.Arg(2), {});
    case IR::Opcode::ImageAtomicFMax32:
        return ir.BufferAtomicFMax(handle, address, inst.Arg(2), {});
    default:
        throw std::runtime_error("Shared image atomic requires an equivalent raw buffer operation");
    }
}

IR::U32 BlendChannel(IR::IREmitter& ir, const IR::U32& a, const IR::U32& b, const IR::U32& weight,
                     u32 den, bool is_signed) {
    const auto word = ir.IAdd(ir.IAdd(ir.IMul(a, ir.Imm32(den)), ir.IMul(weight, ir.ISub(b, a))),
                              IR::U32{ir.Imm32(den / 2)});
    return IR::U32{ir.IDiv(word, ir.Imm32(den), is_signed)};
}

IR::U32 Sign8(IR::IREmitter& ir, const IR::U32& value) {
    return IR::U32{
        ir.ShiftRightArithmetic(ir.ShiftLeftLogical(value, ir.Imm32(24u)), ir.Imm32(24u))};
}

IR::U32 ExpandBits(IR::IREmitter& ir, const IR::U32& value, u32 bits) {
    return IR::U32{ir.BitwiseAnd(ir.BitwiseOr(ir.ShiftLeftLogical(value, ir.Imm32(8u - bits)),
                                              ir.ShiftRightLogical(value, ir.Imm32(bits - 3u))),
                                 ir.Imm32(0xffu))};
}

std::array<IR::U32, 3> Rgb565Channels(IR::IREmitter& ir, const IR::U32& color) {
    return {
        ExpandBits(
            ir, IR::U32{ir.BitwiseAnd(ir.ShiftRightLogical(color, ir.Imm32(11u)), ir.Imm32(31u))},
            5),
        ExpandBits(ir,
                   IR::U32{ir.BitwiseAnd(ir.ShiftRightLogical(color, ir.Imm32(5u)), ir.Imm32(63u))},
                   6),
        ExpandBits(ir, IR::U32{ir.BitwiseAnd(color, ir.Imm32(31u))}, 5)};
}

IR::U32 Bc1Color(IR::IREmitter& ir, const IR::U32& color_word, const IR::U32& index,
                 const IR::U1& color3) {
    const auto as = Rgb565Channels(ir, IR::U32{ir.BitwiseAnd(color_word, ir.Imm32(0xffffu))});
    const auto bs = Rgb565Channels(ir, IR::U32{ir.ShiftRightLogical(color_word, ir.Imm32(16u))});
    std::array<IR::U32, 3> mixed{};
    for (u32 channel = 0; channel < 3; ++channel) {
        const auto third =
            BlendChannel(ir, as[channel], bs[channel], IR::U32{ir.Imm32(1u)}, 3, false);
        const auto second =
            BlendChannel(ir, as[channel], bs[channel], IR::U32{ir.Imm32(2u)}, 3, false);
        const auto dark = IR::U32{ir.Select(color3, IR::U32{ir.Imm32(0u)}, second)};
        mixed[channel] =
            IR::U32{ir.Select(ir.IEqual(index, ir.Imm32(0u)), as[channel],
                              ir.Select(ir.IEqual(index, ir.Imm32(1u)), bs[channel],
                                        ir.Select(ir.IEqual(index, ir.Imm32(2u)), third, dark)))};
    }
    return IR::U32{
        ir.BitwiseOr(mixed[0], ir.BitwiseOr(ir.ShiftLeftLogical(mixed[1], ir.Imm32(8u)),
                                            ir.ShiftLeftLogical(mixed[2], ir.Imm32(16u))))};
}

IR::U32 AlphaMachinery(IR::IREmitter& ir, const IR::U32& first_word, const IR::U32& second_word,
                       const IR::U32& index, bool is_signed) {
    const auto a0raw = IR::U32{ir.BitwiseAnd(first_word, ir.Imm32(0xffu))};
    const auto a1raw =
        IR::U32{ir.BitwiseAnd(ir.ShiftRightLogical(first_word, ir.Imm32(8u)), ir.Imm32(0xffu))};
    const auto a0 = is_signed ? Sign8(ir, a0raw) : a0raw;
    const auto a1 = is_signed ? Sign8(ir, a1raw) : a1raw;
    const auto extended = ir.ILessThan(a1, a0, is_signed);
    const auto bit = IR::U32{ir.IAdd(ir.ShiftLeftLogical(index, ir.Imm32(3u)), ir.Imm32(16u))};
    const auto overflow = ir.ILessThanEqual(IR::U32{ir.Imm32(32u)}, bit, false);
    const auto span = IR::U32{ir.Select(overflow, ir.ISub(bit, ir.Imm32(32u)), bit)};
    const auto word = IR::U32{ir.Select(overflow, IR::Value{second_word}, IR::Value{first_word})};
    const auto selection = IR::U32{ir.BitwiseAnd(ir.ShiftRightLogical(word, span), ir.Imm32(7u))};
    const auto weight = IR::U32{ir.ISub(selection, IR::U32{ir.Imm32(1u)})};
    const auto eight = BlendChannel(ir, a0, a1, weight, 7, is_signed);
    const auto six = BlendChannel(ir, a0, a1, weight, 5, is_signed);
    const auto ring = IR::U32{ir.Imm32(is_signed ? 127u : 255u)};
    const auto six_value =
        IR::U32{ir.Select(ir.IEqual(selection, ir.Imm32(6u)), IR::U32{ir.Imm32(0u)},
                          ir.Select(ir.IEqual(selection, ir.Imm32(7u)), ring, six))};
    const auto value = IR::U32{
        ir.Select(ir.IEqual(selection, ir.Imm32(0u)), a0,
                  ir.Select(ir.IEqual(selection, ir.Imm32(1u)), a1,
                            IR::U32{ir.Select(extended, IR::Value{eight}, IR::Value{six_value})}))};
    return is_signed ? IR::U32{ir.BitwiseAnd(value, ir.Imm32(0xffu))} : value;
}

IR::Value DecodeBcnTexel(IR::IREmitter& ir, AmdGpu::DataFormat data_format,
                         AmdGpu::NumberFormat num_format, const IR::Value& words, const IR::U32& x,
                         const IR::U32& y) {
    const auto index =
        IR::U32{ir.IAdd(ir.BitwiseAnd(x, ir.Imm32(3u)),
                        ir.ShiftLeftLogical(ir.BitwiseAnd(y, ir.Imm32(3u)), ir.Imm32(2u)))};
    const auto unpack = [&](const IR::U32& packed) { return ir.Unpack4x8(num_format, packed); };
    const auto texel_2 = [&](const IR::U32& word) {
        return IR::U32{ir.BitwiseAnd(
            ir.ShiftRightLogical(word, ir.ShiftLeftLogical(index, ir.Imm32(1u))), ir.Imm32(3u))};
    };
    const IR::U32 w0{ir.CompositeExtract(words, 0)};
    const IR::U32 w1{ir.CompositeExtract(words, 1)};
    switch (data_format) {
    case AmdGpu::DataFormat::FormatBc1: {
        const auto punch3 =
            ir.LogicalNot(ir.ILessThan(IR::U32{ir.ShiftRightLogical(w0, ir.Imm32(16u))},
                                       IR::U32{ir.BitwiseAnd(w0, ir.Imm32(0xffffu))}, false));
        const auto texel = texel_2(w1);
        const auto color = Bc1Color(ir, w0, texel, punch3);
        const auto alpha = IR::U32{ir.Select(ir.LogicalAnd(ir.IEqual(texel, ir.Imm32(3u)), punch3),
                                             IR::U32{ir.Imm32(0u)}, IR::U32{ir.Imm32(255u)})};
        return unpack(IR::U32{ir.BitwiseOr(color, ir.ShiftLeftLogical(alpha, ir.Imm32(24u)))});
    }
    case AmdGpu::DataFormat::FormatBc2: {
        const IR::U32 w2{ir.CompositeExtract(words, 2)};
        const IR::U32 w3{ir.CompositeExtract(words, 3)};
        const auto alpha_word = IR::U32{ir.Select(
            ir.ILessThanEqual(IR::U32{ir.Imm32(8u)}, index, false), IR::Value{w1}, IR::Value{w0})};
        const auto alpha4 = IR::U32{ir.IMul(
            ir.BitwiseAnd(ir.ShiftRightLogical(
                              alpha_word, ir.ShiftLeftLogical(ir.BitwiseAnd(index, ir.Imm32(7u)),
                                                              ir.Imm32(2u))),
                          ir.Imm32(15u)),
            ir.Imm32(17u))};
        return unpack(IR::U32{ir.BitwiseOr(Bc1Color(ir, w2, texel_2(w3), IR::U1{ir.Imm1(false)}),
                                           ir.ShiftLeftLogical(alpha4, ir.Imm32(24u)))});
    }
    case AmdGpu::DataFormat::FormatBc3: {
        const IR::U32 w2{ir.CompositeExtract(words, 2)};
        const IR::U32 w3{ir.CompositeExtract(words, 3)};
        const auto alpha = AlphaMachinery(ir, w0, w1, index, false);
        return unpack(IR::U32{ir.BitwiseOr(Bc1Color(ir, w2, texel_2(w3), IR::U1{ir.Imm1(false)}),
                                           ir.ShiftLeftLogical(alpha, ir.Imm32(24u)))});
    }
    case AmdGpu::DataFormat::FormatBc4:
        return unpack(AlphaMachinery(ir, w0, w1, index, num_format == AmdGpu::NumberFormat::Snorm));
    case AmdGpu::DataFormat::FormatBc5: {
        const IR::U32 w2{ir.CompositeExtract(words, 2)};
        const IR::U32 w3{ir.CompositeExtract(words, 3)};
        const bool is_signed = num_format == AmdGpu::NumberFormat::Snorm;
        const auto red = AlphaMachinery(ir, w0, w1, index, is_signed);
        const auto green = AlphaMachinery(ir, w2, w3, index, is_signed);
        return unpack(IR::U32{ir.BitwiseOr(red, ir.ShiftLeftLogical(green, ir.Imm32(8u)))});
    }
    default:
        throw std::runtime_error("Unsupported compressed shared image data format");
    }
}

constexpr u32 SamplerEntryWords = SharedSamplerWords;

IR::Value DecodeCompressed(IR::IREmitter& ir, const ImageAlias& image, const IR::Value& words,
                           IR::U32 x, IR::U32 y) {
    const auto format = Format(image);
    if (format.data_format == AmdGpu::DataFormat::FormatBc6 ||
        format.data_format == AmdGpu::DataFormat::FormatBc7) {
        const auto pixel =
            IR::U32{ir.IAdd(ir.BitwiseAnd(x, ir.Imm32(3u)),
                            ir.ShiftLeftLogical(ir.BitwiseAnd(y, ir.Imm32(3u)), ir.Imm32(2u)))};
        Bptc::Decoder decoder{ir, words, ir.Imm32(image.addresses),
                              ir.Imm32(image.bptc_table_start), pixel};
        return format.data_format == AmdGpu::DataFormat::FormatBc7
                   ? decoder.Bc7()
                   : decoder.Bc6(format.num_format == AmdGpu::NumberFormat::Snorm);
    }
    return DecodeBcnTexel(ir, format.data_format, format.num_format, words, x, y);
}

bool IsSampledInstruction(IR::Opcode opcode) {
    return opcode == IR::Opcode::ImageSampleImplicitLod ||
           opcode == IR::Opcode::ImageSampleExplicitLod ||
           opcode == IR::Opcode::ImageSampleDrefImplicitLod ||
           opcode == IR::Opcode::ImageSampleDrefExplicitLod ||
           opcode == IR::Opcode::ImageGradient || opcode == IR::Opcode::ImageGather ||
           opcode == IR::Opcode::ImageGatherDref || opcode == IR::Opcode::ImageQueryLod;
}

IR::F32 WrapCoord(IR::IREmitter& ir, const IR::U32& mode, const IR::F32& coord) {
    const auto one = ir.Imm32(1.f);
    const auto fract = IR::F32{ir.FPFract(coord)};
    const auto half = IR::F32{ir.FPMul(coord, ir.Imm32(0.5f))};
    const auto folded = IR::F32{ir.FPSub(coord, ir.FPMul(ir.FPFloor(half), ir.Imm32(2.f)))};
    const auto mirrored = IR::F32{ir.Select(ir.FPLessThanEqual(folded, one), folded,
                                            IR::F32{ir.FPSub(ir.Imm32(2.f), folded)})};
    const auto abs_coord = IR::F32{ir.FPAbs(coord)};
    const auto mode_wrap = ir.Imm32(u32(AmdGpu::ClampMode::Wrap));
    const auto mode_mirror = ir.Imm32(u32(AmdGpu::ClampMode::Mirror));
    const auto mode_clamp = ir.Imm32(u32(AmdGpu::ClampMode::ClampLastTexel));
    const auto mode_once = ir.Imm32(u32(AmdGpu::ClampMode::MirrorOnceLastTexel));
    const auto mode_half = ir.Imm32(u32(AmdGpu::ClampMode::ClampHalfBorder));
    const auto mode_once_half = ir.Imm32(u32(AmdGpu::ClampMode::MirrorOnceHalfBorder));
    const auto mode_once_border = ir.Imm32(u32(AmdGpu::ClampMode::MirrorOnceBorder));
    const auto clamped = IR::F32{ir.FPSaturate(coord)};
    const auto mirror_clamped = IR::F32{ir.FPSaturate(abs_coord)};
    return IR::F32{ir.Select(
        ir.IEqual(mode, mode_wrap), fract,
        ir.Select(
            ir.IEqual(mode, mode_mirror), mirrored,
            ir.Select(
                ir.LogicalOr(ir.IEqual(mode, mode_clamp), ir.IEqual(mode, mode_half)), clamped,
                ir.Select(ir.LogicalOr(ir.IEqual(mode, mode_once), ir.IEqual(mode, mode_once_half)),
                          mirror_clamped,
                          ir.Select(ir.IEqual(mode, mode_once_border), abs_coord, coord)))))};
}

IR::U32 SampleSize(IR::IREmitter& ir, const ImageAlias& image, const IR::Value& mip, u32 axis) {
    const u32 size = axis == 0   ? image.info.size.width
                     : axis == 1 ? image.info.size.height
                                 : image.info.size.depth;
    IR::U32 selected_size{ir.Imm32(0u)};
    for (u32 level = 0; level < image.info.resources.levels; ++level) {
        selected_size = IR::U32{ir.Select(ir.IEqual(IR::U32{mip}, ir.Imm32(level)),
                                          ir.Imm32(std::max(1u, size >> level)), selected_size)};
    }
    return IR::U32{ir.UMax(selected_size, ir.Imm32(1u))};
}

IR::U1 SampleCompare(IR::IREmitter& ir, const IR::U32& func, const IR::F32& dref,
                     const IR::F32& texel) {
    IR::U1 result{ir.Imm1(false)};
    const auto pick = [&](u32 code, IR::U1 condition) {
        result = IR::U1{ir.Select(ir.IEqual(func, ir.Imm32(code)), condition, result)};
    };
    pick(u32(AmdGpu::DepthCompare::Less), IR::U1{ir.FPLessThan(dref, texel)});
    pick(u32(AmdGpu::DepthCompare::Equal),
         IR::U1{ir.LogicalNot(IR::U1{ir.FPNotEqual(dref, texel)})});
    pick(u32(AmdGpu::DepthCompare::LessEqual), IR::U1{ir.FPLessThanEqual(dref, texel)});
    pick(u32(AmdGpu::DepthCompare::Greater), IR::U1{ir.FPGreaterThan(dref, texel)});
    pick(u32(AmdGpu::DepthCompare::NotEqual), IR::U1{ir.FPNotEqual(dref, texel)});
    pick(u32(AmdGpu::DepthCompare::GreaterEqual), IR::U1{ir.FPGreaterThanEqual(dref, texel)});
    return IR::U1{ir.Select(ir.IEqual(func, ir.Imm32(u32(AmdGpu::DepthCompare::Always))),
                            IR::U1{ir.Imm1(true)}, result)};
}

IR::U32 SampleFixLowIndex(IR::IREmitter& ir, const IR::U32& mode, const IR::U32& base,
                          const IR::U32& dims) {
    const auto underflow = IR::U1{ir.ILessThan(base, ir.Imm32(0u), true)};
    const auto is_wrap = ir.IEqual(mode, ir.Imm32(u32(AmdGpu::ClampMode::Wrap)));
    const auto is_mirror = ir.IEqual(mode, ir.Imm32(u32(AmdGpu::ClampMode::Mirror)));
    const auto is_border =
        ir.IGreaterThanEqual(mode, ir.Imm32(u32(AmdGpu::ClampMode::ClampHalfBorder)), false);
    const auto underflow_fix =
        IR::U32{ir.Select(is_wrap, IR::U32{ir.IAdd(base, dims)},
                          ir.Select(is_mirror, IR::U32{ir.BitwiseNot(base)},
                                    ir.Select(is_border, base, ir.Imm32(0u))))};
    return IR::U32{ir.Select(underflow, underflow_fix, base)};
}

IR::U32 SampleFixHighIndex(IR::IREmitter& ir, const IR::U32& mode, const IR::U32& next,
                           const IR::U32& dims) {
    const auto overflow = IR::U1{ir.IEqual(next, dims)};
    const auto is_wrap = ir.IEqual(mode, ir.Imm32(u32(AmdGpu::ClampMode::Wrap)));
    const auto is_border =
        ir.IGreaterThanEqual(mode, ir.Imm32(u32(AmdGpu::ClampMode::ClampHalfBorder)), false);
    const auto overflow_fix = IR::U32{ir.Select(
        is_wrap, ir.Imm32(0u), ir.Select(is_border, next, IR::U32{ir.ISub(dims, ir.Imm32(1u))}))};
    return IR::U32{ir.Select(overflow, overflow_fix, next)};
}

struct SampleAxis {
    IR::U32 low, high;
    IR::F32 frac;
};

SampleAxis BuildSampleAxis(IR::IREmitter& ir, const IR::U32& mode, const IR::F32& coord,
                           const IR::Value& offset, const IR::U32& dims, const IR::U1& linear,
                           const IR::U1& trunc) {
    const IR::F32 dims_f{ir.ConvertUToF(32, 32, dims)};
    IR::F32 u = coord;
    if (!offset.IsEmpty()) {
        const auto offset_f = IR::F32{ir.ConvertSToF(32, 32, offset)};
        u = IR::F32{ir.FPAdd(u, IR::F32{ir.FPMul(offset_f, IR::F32{ir.FPRecip(dims_f)})})};
    }
    const auto s = IR::F32{ir.FPMul(WrapCoord(ir, mode, u), dims_f)};
    const auto shift = IR::F32{ir.Select(linear, ir.Imm32(0.5f), ir.Imm32(0.f))};
    const auto f = IR::F32{ir.FPSub(s, shift)};
    const auto base_f = IR::F32{ir.FPFloor(f)};
    const auto frac_raw = IR::F32{ir.FPSub(f, base_f)};
    const auto low = IR::U32{ir.ConvertFToS(32, base_f)};
    const auto next = IR::U32{ir.IAdd(low, ir.Imm32(1u))};
    const auto fixed_low = SampleFixLowIndex(ir, mode, low, dims);
    const auto fixed_high = SampleFixHighIndex(ir, mode, next, dims);
    const auto half_border =
        ir.LogicalOr(ir.IEqual(mode, ir.Imm32(u32(AmdGpu::ClampMode::ClampHalfBorder))),
                     ir.IEqual(mode, ir.Imm32(u32(AmdGpu::ClampMode::MirrorOnceHalfBorder))));
    const auto point_mode =
        IR::U32{ir.Select(half_border, ir.Imm32(u32(AmdGpu::ClampMode::ClampLastTexel)), mode)};
    const auto mirror_period =
        IR::F32{ir.FPSub(u, ir.FPMul(ir.FPFloor(ir.FPMul(u, ir.Imm32(0.5f))), ir.Imm32(2.f)))};
    const auto mirror_descending = ir.LogicalOr(
        ir.LogicalAnd(ir.IEqual(mode, ir.Imm32(u32(AmdGpu::ClampMode::Mirror))),
                      ir.FPGreaterThan(mirror_period, ir.Imm32(1.f))),
        ir.LogicalAnd(
            ir.LogicalOr(
                ir.IEqual(mode, ir.Imm32(u32(AmdGpu::ClampMode::MirrorOnceLastTexel))),
                ir.LogicalOr(
                    ir.IEqual(mode, ir.Imm32(u32(AmdGpu::ClampMode::MirrorOnceHalfBorder))),
                    ir.IEqual(mode, ir.Imm32(u32(AmdGpu::ClampMode::MirrorOnceBorder))))),
            ir.FPLessThan(u, ir.Imm32(0.f))));
    const auto mirror_boundary =
        ir.LogicalAnd(mirror_descending, ir.LogicalNot(ir.FPNotEqual(s, ir.FPFloor(s))));
    const auto point_low =
        IR::U32{ir.Select(mirror_boundary, IR::U32{ir.ISub(fixed_low, ir.Imm32(1u))}, fixed_low)};
    const auto point = SampleFixHighIndex(ir, point_mode, point_low, dims);
    return {IR::U32{ir.Select(linear, fixed_low, point)},
            IR::U32{ir.Select(linear, fixed_high, point)},
            IR::F32{ir.Select(linear, frac_raw, ir.Imm32(0.f))}};
}

IR::Value Sample(IR::IREmitter& ir, IR::Inst& inst, const ImageAlias& image, u32 sampler_table,
                 const AmdGpu::Sampler& sampler, bool fragment_stage) {
    if (sampler_table == 0xffff)
        throw std::runtime_error("Missing lowered sampler table");
    if (!sampler.Valid())
        throw std::runtime_error("Invalid shared-image sampler");
    const auto opcode = inst.GetOpcode();
    const auto inst_info = inst.Flags<IR::TextureInstInfo>();
    const auto type = image.sharp.GetViewType(image.resource.is_array);
    if (type != AmdGpu::ImageType::Color1D && type != AmdGpu::ImageType::Color1DArray &&
        type != AmdGpu::ImageType::Color2D && type != AmdGpu::ImageType::Color2DArray &&
        type != AmdGpu::ImageType::Color3D && type != AmdGpu::ImageType::Cube)
        throw std::runtime_error("Shared image sampling supports planar volume layouts only");
    const bool cube = type == AmdGpu::ImageType::Cube;
    const bool query = opcode == IR::Opcode::ImageQueryLod;
    const bool integer = AmdGpu::IsInteger(image.sharp.GetNumberFmt());
    const bool gather = opcode == IR::Opcode::ImageGather || opcode == IR::Opcode::ImageGatherDref;
    const bool point_filter =
        sampler.xy_min_filter == AmdGpu::Filter::Point &&
        sampler.xy_mag_filter == AmdGpu::Filter::Point &&
        (type != AmdGpu::ImageType::Color3D || !(sampler.z_filter.Value() & 1u));
    const bool one_d = type == AmdGpu::ImageType::Color1D;
    const bool one_d_array = type == AmdGpu::ImageType::Color1DArray;
    const bool is_3d = type == AmdGpu::ImageType::Color3D;
    const bool is_array = cube || one_d_array || type == AmdGpu::ImageType::Color2DArray;
    const bool is_dref = opcode == IR::Opcode::ImageSampleDrefImplicitLod ||
                         opcode == IR::Opcode::ImageSampleDrefExplicitLod ||
                         opcode == IR::Opcode::ImageGatherDref;
    const bool explicit_lod = opcode == IR::Opcode::ImageSampleExplicitLod ||
                              opcode == IR::Opcode::ImageSampleDrefExplicitLod ||
                              (gather && (inst_info.has_lod || inst_info.force_level0));
    const bool gradient = opcode == IR::Opcode::ImageGradient;
    if (integer && !query && !gather &&
        (sampler.xy_min_filter != AmdGpu::Filter::Point ||
         sampler.xy_mag_filter != AmdGpu::Filter::Point ||
         sampler.mip_filter == AmdGpu::MipFilter::Linear ||
         (is_3d && (sampler.z_filter.Value() & 1u))))
        throw std::runtime_error("Integer shared image samples require point filtering");
    const u32 axes = one_d || one_d_array ? 1u : is_3d ? 3u : 2u;
    const IR::Value coords = inst.Arg(1);
    const IR::Value offset = query ? IR::Value{}
                                   : inst.Arg(gather                ? 2u
                                              : gradient || is_dref ? 4u
                                                                    : 3u);
    const IR::F32 dref = is_dref ? IR::F32{inst.Arg(gather ? 3u : 2u)} : IR::F32{};
    const u32 view_base =
        image.sharp.base_level +
        (image.resource.mip_fallback_mode == Shader::MipStorageFallbackMode::ConstantIndex
             ? image.resource.constant_mip_index
             : 0);
    if (view_base >= image.info.resources.levels)
        throw std::runtime_error("Shared image base mip exceeds storage");
    const u32 view_levels = std::min(image.sharp.NumViewLevels(image.resource.is_array),
                                     image.info.resources.levels - view_base);
    if (!view_levels)
        throw std::runtime_error("Shared image sampling has an empty mip view");
    const auto entry = ir.Imm32((inst.Arg(0).U32() >> 16) * SamplerEntryWords);
    const auto word = [&](u32 index) {
        return IR::U32{ir.LoadBufferU32(1, ir.Imm32(sampler_table),
                                        IR::U32{ir.IAdd(entry, ir.Imm32(index))}, {})};
    };
    const auto field = [&](u32 shift, u32 mask) {
        return IR::U32{
            ir.BitwiseAnd(ir.ShiftRightLogical(word(5), ir.Imm32(shift)), ir.Imm32(mask))};
    };
    const auto real = [&](u32 index) { return ir.BitCast<IR::F32>(word(index)); };
    const auto params = word(0);
    std::array<IR::F32, 4> border;
    for (u32 c = 0; c < 4; ++c) {
        border[c] = real(6 + c);
        const auto number = image.sharp.GetNumberFmt();
        if (number == AmdGpu::NumberFormat::Unorm || number == AmdGpu::NumberFormat::Srgb)
            border[c] = IR::F32{ir.FPSaturate(border[c])};
        else if (number == AmdGpu::NumberFormat::Snorm)
            border[c] = IR::F32{ir.FPMin(ir.FPMax(border[c], ir.Imm32(-1.f)), ir.Imm32(1.f))};
        else if (integer && sampler.border_color_type != AmdGpu::BorderColor::Custom) {
            const IR::U32 component{number == AmdGpu::NumberFormat::Sint
                                        ? ir.ConvertFToS(32, border[c])
                                        : ir.ConvertFToU(32, border[c])};
            border[c] = ir.BitCast<IR::F32>(component);
        }
    }
    const auto border_color = ir.CompositeConstruct(border[0], border[1], border[2], border[3]);
    const auto wrap_x = IR::U32{ir.BitwiseAnd(params, ir.Imm32(7u))};
    const auto wrap_y =
        IR::U32{ir.BitwiseAnd(ir.ShiftRightLogical(params, ir.Imm32(3u)), ir.Imm32(7u))};
    const auto wrap_z =
        IR::U32{ir.BitwiseAnd(ir.ShiftRightLogical(params, ir.Imm32(6u)), ir.Imm32(7u))};
    const auto trunc =
        IR::U1{ir.INotEqual(ir.BitwiseAnd(params, ir.Imm32(1u << 14)), ir.Imm32(0u))};
    const auto cmp_func =
        IR::U32{ir.BitwiseAnd(ir.ShiftRightLogical(params, ir.Imm32(11u)), ir.Imm32(7u))};
    const auto mip_filter = field(4, 3);
    const auto reduction = is_dref || gather ? ir.Imm32(0u) : field(6, 3);
    const auto base_coord_axis = [&](u32 axis) -> IR::F32 {
        const auto value = one_d ? IR::F32{coords} : IR::F32{ir.CompositeExtract(coords, axis)};
        return cube ? IR::F32{ir.FPSub(value, ir.Imm32(1.f))} : value;
    };
    std::array<IR::F32, 3> dx{ir.Imm32(0.f), ir.Imm32(0.f), ir.Imm32(0.f)};
    std::array<IR::F32, 3> dy = dx;
    auto xx = ir.Imm32(0.f), yy = ir.Imm32(0.f), xy = ir.Imm32(0.f);
    for (u32 axis = 0; axis < axes; ++axis) {
        if (gradient) {
            dx[axis] =
                axes == 1 ? IR::F32{inst.Arg(2)} : IR::F32{ir.CompositeExtract(inst.Arg(2), axis)};
            dy[axis] =
                axes == 1 ? IR::F32{inst.Arg(3)} : IR::F32{ir.CompositeExtract(inst.Arg(3), axis)};
        } else if (!explicit_lod && fragment_stage) {
            dx[axis] = ir.FPDdx(base_coord_axis(axis));
            dy[axis] = ir.FPDdy(base_coord_axis(axis));
        }
        const u32 size = axis == 0   ? image.info.size.width
                         : axis == 1 ? image.info.size.height
                                     : image.info.size.depth;
        const auto scale = ir.Imm32(float(std::max(1u, size >> std::min(view_base, 31u))));
        if (gradient && (inst_info.is_unnormalized || sampler.force_unnormalized)) {
            dx[axis] = IR::F32{ir.FPDiv(dx[axis], scale)};
            dy[axis] = IR::F32{ir.FPDiv(dy[axis], scale)};
        }
        const auto x = IR::F32{ir.FPMul(dx[axis], scale)};
        const auto y = IR::F32{ir.FPMul(dy[axis], scale)};
        xx = IR::F32{ir.FPAdd(xx, ir.FPMul(x, x))};
        yy = IR::F32{ir.FPAdd(yy, ir.FPMul(y, y))};
        xy = IR::F32{ir.FPAdd(xy, ir.FPMul(x, y))};
    }
    const auto isotropic = ir.FPSqrt(IR::F32{ir.FPMax(xx, yy)});
    auto lod = explicit_lod
                   ? (gather ? (inst_info.force_level0 ? ir.Imm32(0.f)
                                                       : IR::F32{inst.Arg(is_dref ? 4u : 3u)})
                             : IR::F32{inst.Arg(is_dref ? 3u : 2u)})
                   : ir.FPLog2(IR::F32{ir.FPMax(isotropic, ir.Imm32(0x1p-64f))});
    lod = IR::F32{ir.FPAdd(lod, real(4))};
    if (!explicit_lod) {
        if (!gradient && !query) {
            const auto bias = gather
                                  ? (inst_info.has_bias ? inst.Arg(is_dref ? 4u : 3u) : IR::Value{})
                                  : inst.Arg(is_dref ? 3u : 2u);
            if (!bias.IsEmpty())
                lod = IR::F32{ir.FPAdd(lod, IR::F32{bias})};
        }
    }
    const auto last_lod = ir.Imm32(float(view_levels - 1));
    const auto min_lod = real(2);
    const auto max_lod = IR::F32{ir.FPMax(min_lod, real(3))};
    const auto clamp_lod = [&](IR::F32 value) {
        value = IR::F32{ir.FPMin(ir.FPMax(value, min_lod), max_lod)};
        value = IR::F32{ir.Select(ir.IEqual(mip_filter, ir.Imm32(0u)), min_lod, value)};
        if (gather && !inst.Arg(is_dref ? 5u : 4u).IsEmpty())
            value = IR::F32{ir.FPMax(value, IR::F32{inst.Arg(is_dref ? 5u : 4u)})};
        if (gradient && !inst.Arg(5).IsEmpty())
            value = IR::F32{ir.FPMax(value, IR::F32{inst.Arg(5)})};
        if (opcode == IR::Opcode::ImageSampleImplicitLod && !inst.Arg(4).IsEmpty())
            value = IR::F32{ir.FPMax(value, IR::F32{inst.Arg(4)})};
        value = IR::F32{ir.FPMax(
            value, ir.Imm32(std::max(0.f, float(image.sharp.min_lod) / 256.f - float(view_base))))};
        return value;
    };
    const auto unclamped_lod = lod;
    const u32 max_taps = !gather && !explicit_lod && (gradient || fragment_stage) &&
                                 (AmdGpu::IsAnisoFilter(sampler.xy_min_filter) ||
                                  AmdGpu::IsAnisoFilter(sampler.xy_mag_filter))
                             ? u32(sampler.MaxAniso())
                             : 1u;
    auto tap_count = ir.Imm32(1u);
    std::array<IR::F32, 3> direction{ir.Imm32(0.f), ir.Imm32(0.f), ir.Imm32(0.f)};
    if (max_taps > 1) {
        const auto delta = IR::F32{ir.FPSub(xx, yy)};
        const auto disc = ir.FPSqrt(
            IR::F32{ir.FPAdd(ir.FPMul(delta, delta), ir.FPMul(ir.Imm32(4.f), ir.FPMul(xy, xy)))});
        const auto major2 = IR::F32{ir.FPMul(ir.Imm32(0.5f), ir.FPAdd(ir.FPAdd(xx, yy), disc))};
        const auto minor2 = IR::F32{
            ir.FPMax(ir.FPMul(ir.Imm32(0.5f), ir.FPSub(ir.FPAdd(xx, yy), disc)), ir.Imm32(0.f))};
        const auto major = ir.FPSqrt(major2);
        const auto minor = IR::F32{ir.FPMaxTri(
            ir.FPSqrt(minor2), ir.FPDiv(major, ir.Imm32(float(max_taps))), ir.Imm32(1.f))};
        const auto selected_filter = IR::U32{
            ir.Select(ir.FPGreaterThan(clamp_lod(lod), ir.Imm32(0.f)), field(2, 3), field(0, 3))};
        const auto enabled = IR::U1{
            ir.LogicalAnd(ir.INotEqual(ir.BitwiseAnd(selected_filter, ir.Imm32(2u)), ir.Imm32(0u)),
                          ir.FPGreaterThan(major, ir.Imm32(1.f)))};
        tap_count = IR::U32{
            ir.Select(enabled,
                      ir.UMin(ir.ConvertFToU(32, IR::F32{ir.FPCeil(ir.FPDiv(major, minor))}),
                              ir.Imm32(max_taps)),
                      ir.Imm32(1u))};
        lod = IR::F32{ir.Select(
            enabled,
            ir.FPAdd(lod, ir.FPSub(ir.FPLog2(minor),
                                   ir.FPLog2(IR::F32{ir.FPMax(isotropic, ir.Imm32(0x1p-64f))}))),
            lod)};
        const auto x_dominant = IR::U1{ir.FPGreaterThanEqual(xx, yy)};
        auto vx = IR::F32{ir.Select(x_dominant, ir.FPSub(major2, yy), xy)};
        auto vy = IR::F32{ir.Select(x_dominant, xy, ir.FPSub(major2, xx))};
        const auto length2 = IR::F32{ir.FPAdd(ir.FPMul(vx, vx), ir.FPMul(vy, vy))};
        const auto nonzero = IR::U1{ir.FPGreaterThan(length2, ir.Imm32(0x1p-64f))};
        const auto inverse =
            IR::F32{ir.FPRecipSqrt(IR::F32{ir.FPMax(length2, ir.Imm32(0x1p-64f))})};
        vx = IR::F32{ir.Select(nonzero, ir.FPMul(vx, inverse),
                               ir.Select(x_dominant, ir.Imm32(1.f), ir.Imm32(0.f)))};
        vy = IR::F32{ir.Select(nonzero, ir.FPMul(vy, inverse),
                               ir.Select(x_dominant, ir.Imm32(0.f), ir.Imm32(1.f)))};
        for (u32 axis = 0; axis < axes; ++axis)
            direction[axis] = IR::F32{ir.Select(
                enabled, ir.FPAdd(ir.FPMul(dx[axis], vx), ir.FPMul(dy[axis], vy)), ir.Imm32(0.f))};
    }
    const auto filtered_lod = clamp_lod(lod);
    if (query)
        return ir.CompositeConstruct(IR::F32{ir.FPMin(filtered_lod, last_lod)}, unclamped_lod,
                                     ir.Imm32(0.f), ir.Imm32(0.f));
    const auto minifying = IR::U1{ir.FPGreaterThan(filtered_lod, ir.Imm32(0.f))};
    const auto filter = IR::U32{ir.Select(minifying, field(2, 3), field(0, 3))};
    const auto linear_xy =
        gather ? IR::U1{ir.Imm1(true)}
               : IR::U1{ir.INotEqual(ir.BitwiseAnd(filter, ir.Imm32(1u)), ir.Imm32(0u))};
    const auto linear_z =
        IR::U1{ir.INotEqual(ir.BitwiseAnd(field(11, 3), ir.Imm32(1u)), ir.Imm32(0u))};
    const bool point_preclamp =
        sampler.mip_filter == AmdGpu::MipFilter::Point && sampler.mip_point_preclamp;
    lod = IR::F32{
        ir.FPMin(point_preclamp ? clamp_lod(IR::F32{ir.FPAdd(lod, ir.Imm32(0.5f))}) : filtered_lod,
                 last_lod)};
    const auto mip_linear = IR::U1{ir.IEqual(mip_filter, ir.Imm32(2u))};
    const auto selected_lod = IR::F32{
        ir.Select(mip_linear, lod, point_preclamp ? lod : IR::F32{ir.FPAdd(lod, ir.Imm32(0.5f))})};
    const auto mip_lo = IR::U32{ir.ConvertFToU(32, IR::F32{ir.FPFloor(selected_lod)})};
    const auto mip_hi = IR::U32{ir.UMin(ir.IAdd(mip_lo, ir.Imm32(1u)), ir.Imm32(view_levels - 1))};
    const auto mip_frac = IR::F32{ir.Select(mip_linear, ir.FPFract(lod), ir.Imm32(0.f))};
    IR::U32 slice_u{};
    if (is_array) {
        auto layer = IR::F32{ir.CompositeExtract(coords, one_d_array ? 1u : 2u)};
        if (cube)
            layer = IR::F32{
                ir.FPSub(layer, ir.FPMul(ir.FPFloor(IR::F32{ir.FPMul(layer, ir.Imm32(0.125f))}),
                                         ir.Imm32(2.f)))};
        if (!cube && (inst_info.is_unnormalized || sampler.force_unnormalized))
            layer = IR::F32{ir.FPMul(
                layer, ir.Imm32(float(image.sharp.NumViewLayers(image.resource.is_array))))};
        const auto last_layer =
            ir.Imm32(float(image.sharp.NumViewLayers(image.resource.is_array) - 1));
        layer = IR::F32{ir.FPMin(ir.FPMax(layer, ir.Imm32(0.f)), last_layer)};
        slice_u = IR::U32{ir.ConvertFToU(32, IR::F32{ir.FPFloor(ir.FPAdd(layer, ir.Imm32(0.5f)))})};
    }
    const auto offset_axis = [&](u32 axis) -> IR::Value {
        if (offset.IsEmpty())
            return {};
        return axes == 1 ? offset : IR::Value{ir.CompositeExtract(offset, axis)};
    };
    const auto combine = [&](IR::F32 a, IR::F32 b, IR::F32 weight) {
        const auto average = IR::F32{ir.FPAdd(a, ir.FPMul(b, weight))};
        const auto reduced =
            IR::F32{ir.Select(ir.IEqual(reduction, ir.Imm32(1u)), ir.FPMin(a, b), ir.FPMax(a, b))};
        return IR::F32{ir.Select(ir.FPGreaterThan(weight, ir.Imm32(0.f)),
                                 ir.Select(ir.IEqual(reduction, ir.Imm32(0u)), average, reduced),
                                 a)};
    };
    const auto sample_level = [&](IR::U32 raw_mip,
                                  const std::array<IR::F32, 3>& position) -> IR::Value {
        const auto coord_axis = [&](u32 axis) { return position[axis]; };
        const auto mip_u = Mip(ir, image, raw_mip);
        const SampleAxis axis_x =
            BuildSampleAxis(ir, wrap_x, coord_axis(0), offset_axis(0),
                            SampleSize(ir, image, mip_u, 0), linear_xy, trunc);
        const SampleAxis axis_y =
            axes == 1 ? SampleAxis{.low = ir.Imm32(0u), .high = ir.Imm32(0u), .frac = ir.Imm32(0.f)}
                      : BuildSampleAxis(ir, wrap_y, coord_axis(1), offset_axis(1),
                                        SampleSize(ir, image, mip_u, 1), linear_xy, trunc);
        const auto make_coords = [&](IR::U32 x, IR::U32 y, IR::U32 z) -> IR::Value {
            if (one_d)
                return IR::Value{x};
            if (one_d_array)
                return ir.CompositeConstruct(x, slice_u);
            if (is_3d)
                return ir.CompositeConstruct(x, y, z);
            if (is_array)
                return ir.CompositeConstruct(x, y, slice_u);
            return ir.CompositeConstruct(x, y);
        };

        const u32 element_shift = image.info.props.is_block ? 2u
                                                            : (image.info.num_bits == 8    ? 0u
                                                               : image.info.num_bits == 16 ? 1u
                                                                                           : 2u);
        const u32 bcn_words = image.info.num_bits == 64 ? 2u : 4u;
        const auto data_handle = ir.Imm32(image.data);

        const auto load_tap = [&](IR::U32 tx, IR::U32 ty,
                                  IR::U32 tz) -> std::pair<IR::U1, IR::Value> {
            const auto byte_address = Address(ir, image, make_coords(tx, ty, tz), raw_mip, {});
            const auto valid_tap =
                IR::U1{ir.ILessThan(byte_address, ir.Imm32(image.info.guest_size), false)};
            const auto address =
                IR::U32{ir.ShiftRightLogical(byte_address, ir.Imm32(element_shift))};
            IR::Value value;
            if (!image.info.props.is_block) {
                value = LoadTexel(ir, image, address);
            } else {
                const auto words = ir.LoadBufferU32(bcn_words, data_handle, address, {});
                value = DecodeCompressed(ir, image, words, tx, ty);
            }
            if (image.sharp.GetNumberFmt() == AmdGpu::NumberFormat::Srgb)
                value = IR::ApplyForceDegamma(ir, value);
            return {valid_tap, value};
        };

        if (gather) {
            const auto component = [&](IR::U32 x, IR::U32 y) {
                const auto [valid, texel] = load_tap(x, y, ir.Imm32(0u));
                const auto channel = [&](u32 c) {
                    return ir.Select(valid, ir.CompositeExtract(texel, c),
                                     ir.CompositeExtract(border_color, c));
                };
                const auto value = IR::ApplySwizzle(
                    ir, ir.CompositeConstruct(channel(0), channel(1), channel(2), channel(3)),
                    image.sharp.DstSelect(), integer);
                const u32 comp = is_dref ? 0u : inst_info.gather_comp.Value();
                const auto sample = IR::F32{ir.CompositeExtract(value, comp)};
                return is_dref ? IR::F32{ir.Select(SampleCompare(ir, cmp_func, dref, sample),
                                                   ir.Imm32(1.f), ir.Imm32(0.f))}
                               : sample;
            };
            return ir.CompositeConstruct(
                component(axis_x.low, axis_y.high), component(axis_x.high, axis_y.high),
                component(axis_x.high, axis_y.low), component(axis_x.low, axis_y.low));
        }
        std::array<IR::U32, 2> xs{axis_x.low, axis_x.high};
        std::array<IR::F32, 2> wx{IR::F32{ir.FPSub(ir.Imm32(1.f), axis_x.frac)}, axis_x.frac};
        std::array<IR::U32, 2> ys{axis_y.low, axis_y.high};
        std::array<IR::F32, 2> wy{IR::F32{ir.FPSub(ir.Imm32(1.f), axis_y.frac)}, axis_y.frac};
        SampleAxis axis_z{.low = ir.Imm32(0u), .high = ir.Imm32(0u), .frac = ir.Imm32(0.f)};
        if (is_3d) {
            const auto coord_z = coord_axis(2);
            const auto offset_z =
                offset.IsEmpty() ? IR::Value{} : IR::Value{ir.CompositeExtract(offset, 2)};
            axis_z = BuildSampleAxis(ir, wrap_z, coord_z, offset_z, SampleSize(ir, image, mip_u, 2),
                                     linear_z, trunc);
        }
        if (!is_dref && (integer || (point_filter && !gather))) {
            const auto [valid, value] = load_tap(axis_x.low, axis_y.low, axis_z.low);
            const auto component = [&](u32 c) {
                return ir.Select(valid, ir.CompositeExtract(value, c),
                                 ir.CompositeExtract(border_color, c));
            };
            return ir.CompositeConstruct(component(0), component(1), component(2), component(3));
        }
        std::array<IR::U32, 2> zs{axis_z.low, axis_z.high};
        std::array<IR::F32, 2> wz{IR::F32{ir.FPSub(ir.Imm32(1.f), axis_z.frac)}, axis_z.frac};

        std::array<IR::F32, 4> accum{ir.Imm32(0.f), ir.Imm32(0.f), ir.Imm32(0.f), ir.Imm32(0.f)};
        IR::F32 scalar_accum{ir.Imm32(0.f)};
        for (u32 j = 0; j < 2u; ++j) {
            for (u32 i = 0; i < 2u; ++i) {
                for (u32 k = 0; k < (is_3d ? 2u : 1u); ++k) {
                    const auto tap_weight =
                        IR::F32{ir.FPMul(wx[i], IR::F32{ir.FPMul(wy[j], wz[k])})};
                    const auto [tap_valid, tap_value] = load_tap(xs[i], ys[j], zs[k]);
                    if (is_dref) {
                        const auto depth =
                            IR::F32{ir.Select(tap_valid, IR::F32{ir.CompositeExtract(tap_value, 0)},
                                              IR::F32{ir.CompositeExtract(border_color, 0)})};
                        const auto compared =
                            IR::F32{ir.Select(SampleCompare(ir, cmp_func, dref, depth),
                                              ir.Imm32(1.f), ir.Imm32(0.f))};
                        scalar_accum = combine(scalar_accum, compared, tap_weight);
                    } else {
                        for (u32 comp = 0; comp < 4u; ++comp) {
                            const auto texel = IR::F32{
                                ir.Select(tap_valid, IR::F32{ir.CompositeExtract(tap_value, comp)},
                                          IR::F32{ir.CompositeExtract(border_color, comp)})};
                            if (i == 0 && j == 0 && k == 0)
                                accum[comp] = IR::F32{ir.Select(ir.IEqual(reduction, ir.Imm32(0u)),
                                                                ir.Imm32(0.f), texel)};
                            accum[comp] = combine(accum[comp], texel, tap_weight);
                        }
                    }
                }
            }
        }
        const IR::Value value =
            is_dref ? IR::Value{ir.CompositeConstruct(scalar_accum, ir.Imm32(0.f), ir.Imm32(0.f),
                                                      ir.Imm32(0.f))}
                    : IR::Value{ir.CompositeConstruct(accum[0], accum[1], accum[2], accum[3])};
        return value;
    };
    if (gather && !inst_info.has_lod && !inst_info.has_bias && !inst_info.has_lod_clamp)
        return sample_level(
            ir.Imm32(0u),
            {base_coord_axis(0), axes > 1 ? base_coord_axis(1) : ir.Imm32(0.f), ir.Imm32(0.f)});
    if (!is_dref &&
        (integer || (point_filter && !gather && sampler.mip_filter != AmdGpu::MipFilter::Linear))) {
        auto value =
            sample_level(mip_lo, {base_coord_axis(0), axes > 1 ? base_coord_axis(1) : ir.Imm32(0.f),
                                  axes > 2 ? base_coord_axis(2) : ir.Imm32(0.f)});
        if (!gather && !image.resource.is_written && !image.resource.is_feedback)
            value = IR::ApplySwizzle(ir, value, image.sharp.DstSelect(), integer);
        return value;
    }
    const auto inv_taps = IR::F32{ir.FPRecip(IR::F32{ir.ConvertUToF(32, 32, tap_count)})};
    std::array<IR::F32, 4> result{ir.Imm32(0.f), ir.Imm32(0.f), ir.Imm32(0.f), ir.Imm32(0.f)};
    for (u32 tap = 0; tap < max_taps; ++tap) {
        std::array<IR::F32, 3> position{ir.Imm32(0.f), ir.Imm32(0.f), ir.Imm32(0.f)};
        const auto distance =
            IR::F32{ir.FPSub(ir.FPMul(ir.Imm32(float(tap) + 0.5f), inv_taps), ir.Imm32(0.5f))};
        for (u32 axis = 0; axis < axes; ++axis)
            position[axis] =
                IR::F32{ir.FPAdd(base_coord_axis(axis), ir.FPMul(direction[axis], distance))};
        const auto low = sample_level(mip_lo, position);
        const auto high =
            sampler.mip_filter == AmdGpu::MipFilter::Linear ? sample_level(mip_hi, position) : low;
        const auto weight = IR::F32{
            ir.Select(ir.ILessThan(ir.Imm32(tap), tap_count, false), inv_taps, ir.Imm32(0.f))};
        for (u32 c = 0; c < 4; ++c) {
            const auto a = IR::F32{ir.CompositeExtract(low, c)};
            const auto b = IR::F32{ir.CompositeExtract(high, c)};
            const auto base = IR::F32{ir.Select(ir.IEqual(reduction, ir.Imm32(0u)),
                                                ir.FPMul(a, ir.FPSub(ir.Imm32(1.f), mip_frac)), a)};
            const auto filtered = combine(base, b, mip_frac);
            if (tap == 0)
                result[c] =
                    IR::F32{ir.Select(ir.IEqual(reduction, ir.Imm32(0u)), ir.Imm32(0.f), filtered)};
            result[c] = combine(result[c], filtered, weight);
        }
    }
    const auto value = ir.CompositeConstruct(result[0], result[1], result[2], result[3]);
    if (!gather && !image.resource.is_written && !image.resource.is_feedback)
        return IR::ApplySwizzle(ir, value, image.sharp.DstSelect(),
                                AmdGpu::IsInteger(image.sharp.GetNumberFmt()));
    return value;
}

} // namespace

bool LowerIndexFetch(CompiledShader& shader, IR::Program& program, const IndexFetch& indices) {
    if (!indices.address)
        return false;
    if (shader.info.sw_stage != Shader::SwStage::Vertex ||
        (indices.element_size != 2 && indices.element_size != 4) || !indices.size)
        throw std::runtime_error("Invalid D3D12 programmable index fetch");
    const auto handle = IR::Value{
        AddBuffer(shader.info, indices.address, indices.size, false, Shader::BufferType::Guest)};
    for (auto* block : program.blocks) {
        for (auto& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::GetAttributeU32 ||
                inst.Arg(0).Attribute() != IR::Attribute::VertexId)
                continue;
            IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
            const auto vertex = ir.ISub(ir.GetAttributeU32(IR::Attribute::VertexId),
                                        ir.GetAttributeU32(IR::Attribute::BaseVertex));
            const auto base = ir.IMul(vertex, ir.Imm32(indices.element_size));
            auto index = ir.Imm32(0u);
            for (u32 byte = 0; byte < indices.element_size; ++byte) {
                const auto offset =
                    ir.BitwiseXor(ir.IAdd(base, ir.Imm32(byte)), ir.Imm32(indices.byte_xor));
                const auto valid = ir.ILessThan(offset, ir.Imm32(indices.size), false);
                const auto safe_offset = IR::U32{ir.Select(valid, offset, ir.Imm32(0u))};
                const auto loaded = ir.UConvert(32, ir.LoadBufferU8(handle, safe_offset, {}));
                const auto value = IR::U32{ir.Select(valid, loaded, ir.Imm32(0u))};
                index = ir.BitwiseOr(index, ir.ShiftLeftLogical(value, ir.Imm32(byte * 8)));
            }
            const auto valid_index = ir.ILessThan(
                vertex, ir.Imm32(std::min(indices.count, indices.size / indices.element_size)),
                false);
            index = IR::U32{ir.Select(valid_index, index, ir.Imm32(0u))};
            inst.ReplaceUsesWithAndRemove(
                ir.IAdd(index, ir.GetAttributeU32(IR::Attribute::BaseVertex)));
        }
    }
    return true;
}
VideoCore::ImageInfo ShaderImageLayout(const AmdGpu::Image& sharp,
                                       const Shader::ImageResource& resource) {
    const auto format = sharp.GetDataFmt();
    if (format == AmdGpu::DataFormat::FormatGB_GR || format == AmdGpu::DataFormat::FormatBG_RG) {
        if (sharp.GetNumberFmt() != AmdGpu::NumberFormat::Unorm &&
            sharp.GetNumberFmt() != AmdGpu::NumberFormat::Srgb)
            throw std::runtime_error("Packed pair images require normalized components");
        auto storage = sharp;
        storage.data_format = u64(AmdGpu::DataFormat::Format16);
        storage.num_format = u64(AmdGpu::NumberFormat::Unorm);
        return VideoCore::ImageInfo{storage, false};
    }
    if (PackedFormat(format) && format != AmdGpu::DataFormat::Format8_24 &&
        format != AmdGpu::DataFormat::Format24_8 &&
        sharp.GetNumberFmt() != AmdGpu::NumberFormat::Unorm) {
        const auto number = sharp.GetNumberFmt();
        if (number != AmdGpu::NumberFormat::Snorm && number != AmdGpu::NumberFormat::Uint &&
            number != AmdGpu::NumberFormat::Sint && number != AmdGpu::NumberFormat::Srgb)
            throw std::runtime_error("Unsupported packed image number format");
        auto storage = sharp;
        storage.data_format = u64(AmdGpu::DataFormat::Format16);
        storage.num_format = u64(AmdGpu::NumberFormat::Unorm);
        return VideoCore::ImageInfo{storage, false};
    }
    if (format != AmdGpu::DataFormat::Format8_24 && format != AmdGpu::DataFormat::Format24_8 &&
        format != AmdGpu::DataFormat::FormatX24_8_32)
        return VideoCore::ImageInfo{sharp, resource};
    const auto number = sharp.GetNumberFmt();
    if ((format != AmdGpu::DataFormat::FormatX24_8_32 && number != AmdGpu::NumberFormat::Unorm &&
         number != AmdGpu::NumberFormat::Uint && number != AmdGpu::NumberFormat::Snorm &&
         number != AmdGpu::NumberFormat::Sint) ||
        (format == AmdGpu::DataFormat::FormatX24_8_32 && number != AmdGpu::NumberFormat::Float &&
         number != AmdGpu::NumberFormat::Uint))
        throw std::runtime_error("Unsupported packed depth number format");
    auto storage = sharp;
    storage.data_format =
        u64(format == AmdGpu::DataFormat::FormatX24_8_32 ? AmdGpu::DataFormat::Format32_32
                                                         : AmdGpu::DataFormat::Format32);
    storage.num_format = u64(AmdGpu::NumberFormat::Uint);
    VideoCore::ImageInfo layout{storage, false};
    layout.props.is_depth = resource.is_depth;
    return layout;
}

std::vector<ShaderResourceRange> GetShaderResourceRanges(const Shader::Info& info) {
    std::vector<ShaderResourceRange> ranges;
    for (const auto& image : info.images) {
        const auto layout = ShaderImageLayout(image.GetSharp(info), image);
        ranges.push_back({layout.guest_address, layout.guest_size, info.sw_stage,
                          image.is_written || image.is_atomic});
    }
    for (const auto& buffer : info.buffers) {
        if (buffer.buffer_type != Shader::BufferType::Guest)
            continue;
        const auto sharp = buffer.GetSharp(info);
        ranges.push_back({sharp.base_address,
                          u64(sharp.num_records) * std::max<u32>(1, sharp.stride), info.sw_stage,
                          buffer.is_written});
    }
    return ranges;
}
bool LowerImageBufferAliases(CompiledShader& shader, IR::Program& program,
                             const ResourceCache* resources,
                             std::span<const ShaderResourceRange> stage_resources,
                             ID3D12Device* device) {
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
        layouts.push_back(ShaderImageLayout(image.GetSharp(info), image));
    const auto supports_view = [&](u32 index, D3D12_FORMAT_SUPPORT1 usage) {
        if (!device)
            return true;
        const auto& resource = info.images[index];
        DXGI_FORMAT format{};
        try {
            format = ConvertFormat(layouts[index].pixel_format);
            if (layouts[index].num_samples > 1) {
                GetMsaaFormat(format, layouts[index].props.is_depth);
                D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{format,
                                                                      layouts[index].num_samples};
                if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                                       &quality, sizeof(quality))) ||
                    !quality.NumQualityLevels)
                    return false;
            }
        } catch (const std::runtime_error&) {
            return false;
        }
        if (resource.is_depth)
            format = resource.GetSharp(info).GetDataFmt() == AmdGpu::DataFormat::Format16
                         ? DXGI_FORMAT_R16_UNORM
                         : DXGI_FORMAT_R32_FLOAT;
        if ((resource.is_written || resource.is_feedback) &&
            format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
            format = DXGI_FORMAT_R8G8B8A8_UNORM;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support,
                                               sizeof(support))))
            return false;
        if ((support.Support1 & usage) != usage)
            return false;
        if (resource.is_written || resource.is_feedback || resource.is_atomic)
            return (support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) &&
                   (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) &&
                   (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
        return true;
    };
    const u32 original_buffers = info.buffers.size();
    for (u32 i = 0; i < info.images.size(); ++i) {
        const auto sharp = info.images[i].GetSharp(info);
        const auto format = sharp.GetDataFmt();
        const auto type = sharp.GetViewType(info.images[i].is_array);
        const bool storage =
            info.images[i].is_written || info.images[i].is_feedback || info.images[i].is_atomic;
        const bool multisampled =
            type == AmdGpu::ImageType::Color2DMsaa || type == AmdGpu::ImageType::Color2DMsaaArray;
        const auto dimension =
            type == AmdGpu::ImageType::Color1D || type == AmdGpu::ImageType::Color1DArray
                ? D3D12_FORMAT_SUPPORT1_TEXTURE1D
            : type == AmdGpu::ImageType::Color3D ? D3D12_FORMAT_SUPPORT1_TEXTURE3D
                                                 : D3D12_FORMAT_SUPPORT1_TEXTURE2D;
        selected[i] =
            (multisampled && (storage || sharp.NumSamples() == 1)) ||
            (storage && type == AmdGpu::ImageType::Cube) || info.images[i].is_atomic ||
            !supports_view(i, dimension) || format == AmdGpu::DataFormat::FormatGB_GR ||
            format == AmdGpu::DataFormat::FormatBG_RG || format == AmdGpu::DataFormat::Format8_24 ||
            format == AmdGpu::DataFormat::Format24_8 ||
            format == AmdGpu::DataFormat::FormatX24_8_32 ||
            (PackedFormat(format).has_value() &&
             info.images[i].GetSharp(info).GetNumberFmt() != AmdGpu::NumberFormat::Unorm) ||
            (info.images[i].is_written &&
             (PackedFormat(format).has_value() || format == AmdGpu::DataFormat::Format5_9_9_9 ||
              (resources &&
               resources->HasRepeatedBacking(layouts[i].guest_address, layouts[i].guest_size))));
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
    for (auto* block : program.blocks) {
        for (const auto& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::ImageRead) {
                const u32 index = inst.Arg(0).U32();
                const auto sharp = info.images[index].GetSharp(info);
                const auto usage = sharp.NumSamples() > 1 ? D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD
                                                          : D3D12_FORMAT_SUPPORT1_SHADER_LOAD;
                if (!supports_view(index, usage))
                    selected[index] = true;
            }
            if (!IsSampledInstruction(inst.GetOpcode()))
                continue;
            const u32 handle = inst.Arg(0).U32();
            const bool gather = inst.GetOpcode() == IR::Opcode::ImageGather ||
                                inst.GetOpcode() == IR::Opcode::ImageGatherDref;
            const bool comparison = inst.GetOpcode() == IR::Opcode::ImageSampleDrefImplicitLod ||
                                    inst.GetOpcode() == IR::Opcode::ImageSampleDrefExplicitLod ||
                                    inst.GetOpcode() == IR::Opcode::ImageGatherDref;
            const auto usage = gather ? (comparison ? D3D12_FORMAT_SUPPORT1_SHADER_GATHER_COMPARISON
                                                    : D3D12_FORMAT_SUPPORT1_SHADER_GATHER)
                               : comparison ? D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON
                                            : D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;
            if (!supports_view(handle & 0xffff, usage))
                selected[handle & 0xffff] = true;
            const auto sampler = info.samplers[handle >> 16].GetSharp(info);
            const auto manual_mode = [](AmdGpu::ClampMode mode) {
                return mode == AmdGpu::ClampMode::ClampHalfBorder ||
                       mode == AmdGpu::ClampMode::MirrorOnceHalfBorder ||
                       mode == AmdGpu::ClampMode::MirrorOnceBorder;
            };
            if (manual_mode(sampler.clamp_x) || manual_mode(sampler.clamp_y) ||
                manual_mode(sampler.clamp_z) ||
                sampler.border_color_type == AmdGpu::BorderColor::Custom ||
                AmdGpu::IsInteger(info.images[handle & 0xffff].GetSharp(info).GetNumberFmt()))
                selected[handle & 0xffff] = true;
            if (inst.GetOpcode() == IR::Opcode::ImageGather ||
                inst.GetOpcode() == IR::Opcode::ImageGatherDref) {
                const auto flags = inst.Flags<IR::TextureInstInfo>();
                if (flags.has_lod || flags.has_bias || flags.has_lod_clamp)
                    selected[handle & 0xffff] = true;
            }
        }
    }
    for (u32 i = 0; i < info.images.size(); ++i)
        for (u32 j = 0; j < i; ++j)
            if ((info.images[i].is_written || info.images[j].is_written) &&
                overlaps(layouts[i].guest_address, layouts[i].guest_size, layouts[j].guest_address,
                         layouts[j].guest_size)) {
                selected[i] = true;
                selected[j] = true;
            }
    std::vector<bool> coherent(original_buffers);
    for (const auto& peer : stage_resources) {
        if (peer.stage == info.sw_stage)
            continue;
        for (u32 i = 0; i < info.images.size(); ++i)
            if ((info.images[i].is_written || peer.written) &&
                overlaps(layouts[i].guest_address, layouts[i].guest_size, peer.address, peer.size))
                selected[i] = true;
        for (u32 b = 0; b < original_buffers; ++b) {
            auto& buffer = info.buffers[b];
            if (buffer.buffer_type != Shader::BufferType::Guest ||
                !(buffer.is_written || peer.written))
                continue;
            const auto sharp = buffer.GetSharp(info);
            if (overlaps(sharp.base_address,
                         u64(sharp.num_records) * std::max<u32>(1, sharp.stride), peer.address,
                         peer.size)) {
                buffer.is_coherent = true;
                coherent[b] = true;
            }
        }
    }
    if (std::ranges::none_of(selected, [](bool value) { return value; }) &&
        std::ranges::none_of(coherent, [](bool value) { return value; }))
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
    bool needs_samplers{};
    for (auto* block : program.blocks) {
        for (const auto& inst : block->Instructions()) {
            if (!IsSampledInstruction(inst.GetOpcode()))
                continue;
            if (inst.Arg(0).IsImmediate() && selected[inst.Arg(0).U32() & 0xffff])
                needs_samplers = true;
        }
    }
    u32 sampler_table = 0xffff;
    if (needs_samplers) {
        if (info.samplers.empty())
            throw std::runtime_error("Sampled shared image requires guest sampler sharp");
        sampler_table =
            AddBuffer(info, 1, u32(info.samplers.size()) * SamplerEntryWords * sizeof(u32), false,
                      Shader::BufferType::SamplerTable);
    }
    std::vector<std::optional<ImageAlias>> aliases(info.images.size());
    u64 table_bytes{};
    for (u32 i = 0; i < selected.size(); ++i) {
        if (!selected[i])
            continue;
        const auto& layout = layouts[i];
        const auto sharp = info.images[i].GetSharp(info);
        const auto data_fmt = sharp.GetDataFmt();
        if (layout.props.is_block) {
            if (data_fmt < AmdGpu::DataFormat::FormatBc1 ||
                data_fmt > AmdGpu::DataFormat::FormatBc7)
                throw std::runtime_error("Shared compressed image views support BC1-BC7");
            if (layout.num_samples != 1 || (sharp.GetNumberFmt() != AmdGpu::NumberFormat::Unorm &&
                                            sharp.GetNumberFmt() != AmdGpu::NumberFormat::Snorm &&
                                            sharp.GetNumberFmt() != AmdGpu::NumberFormat::Srgb))
                throw std::runtime_error(
                    "Shared compressed image views require single-sample normalized formats");
        }
        switch (data_fmt) {
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
        case AmdGpu::DataFormat::Format5_6_5:
        case AmdGpu::DataFormat::Format1_5_5_5:
        case AmdGpu::DataFormat::Format4_4_4_4:
        case AmdGpu::DataFormat::Format5_9_9_9:
        case AmdGpu::DataFormat::Format8_24:
        case AmdGpu::DataFormat::Format24_8:
        case AmdGpu::DataFormat::FormatX24_8_32:
        case AmdGpu::DataFormat::FormatGB_GR:
        case AmdGpu::DataFormat::FormatBG_RG:
            break;
        case AmdGpu::DataFormat::FormatBc1:
        case AmdGpu::DataFormat::FormatBc2:
        case AmdGpu::DataFormat::FormatBc3:
        case AmdGpu::DataFormat::FormatBc4:
        case AmdGpu::DataFormat::FormatBc5:
        case AmdGpu::DataFormat::FormatBc6:
        case AmdGpu::DataFormat::FormatBc7:
            break;
        default:
            if (info.images[i].is_depth)
                throw std::runtime_error(
                    "Shared depth image views require Format16 or Format32 data");
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
        const bool bptc =
            data_fmt == AmdGpu::DataFormat::FormatBc6 || data_fmt == AmdGpu::DataFormat::FormatBc7;
        const u64 bytes =
            (u64(layout.guest_size) / (layout.num_bits / 8) + (bptc ? Bptc::Tables.size() : 0)) *
            sizeof(u32);
        if (info.buffers.size() + 2 > Shader::NUM_BUFFERS ||
            table_bytes + bytes > 64ULL * 1024 * 1024)
            throw std::runtime_error("Shared image address tables exceed shader resource budget");
        table_bytes += bytes;
        auto table = VideoCore::BuildImageAddressTable(layout);
        const u32 bptc_start = table.size();
        if (bptc)
            table.insert(table.end(), Bptc::Tables.begin(), Bptc::Tables.end());
        const auto resource = info.images[i];
        const u32 data = AddBuffer(info, layout.guest_address, layout.guest_size,
                                   resource.is_written, Shader::BufferType::Guest);
        const u32 addresses = AddBuffer(info, 1, table.size() * sizeof(u32), false,
                                        Shader::BufferType::ImageAddressTable);
        shader.image_address_tables.emplace(addresses, std::move(table));
        aliases[i] = ImageAlias{layout, sharp, resource, data, addresses, bptc_start};
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
                if (Optimization::IsBufferStore(inst)) {
                    IR::IREmitter after{*block,
                                        std::next(IR::Block::InstructionList::s_iterator_to(inst))};
                    after.DeviceMemoryBarrier();
                }
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
            const auto sampled = IsSampledInstruction(opcode);
            if (opcode != IR::Opcode::ImageRead && opcode != IR::Opcode::ImageWrite &&
                !Optimization::IsImageAtomicInstruction(inst) && !sampled)
                throw std::runtime_error("Shared image gather/query requires raw shader lowering");
            if (sampled) {
                ir.DeviceMemoryBarrier();
                inst.ReplaceUsesWithAndRemove(
                    Sample(ir, inst, image, sampler_table,
                           info.samplers.at(inst.Arg(0).U32() >> 16).GetSharp(info),
                           info.sw_stage == Shader::SwStage::Fragment));
                continue;
            }
            const auto lod = opcode == IR::Opcode::ImageRead || opcode == IR::Opcode::ImageWrite
                                 ? inst.Arg(2)
                                 : IR::Value{};
            const auto sample = [&]() -> IR::Value {
                if (opcode == IR::Opcode::ImageRead || opcode == IR::Opcode::ImageWrite)
                    return inst.Arg(3);
                const auto type = image.sharp.GetViewType(image.resource.is_array);
                if (type == AmdGpu::ImageType::Color2DMsaa)
                    return ir.CompositeExtract(inst.Arg(1), 2);
                if (type == AmdGpu::ImageType::Color2DMsaaArray)
                    return ir.CompositeExtract(inst.Arg(1), 3);
                return {};
            }();
            const auto byte_address = Address(ir, image, inst.Arg(1), lod, sample);
            const auto element_shift = image.info.props.is_block
                                           ? 2u
                                           : (image.info.num_bits == 8    ? 0u
                                              : image.info.num_bits == 16 ? 1u
                                                                          : 2u);
            const auto address = ir.ShiftRightLogical(byte_address, ir.Imm32(element_shift));
            const auto data = ir.Imm32(image.data);
            ir.DeviceMemoryBarrier();
            if (opcode == IR::Opcode::ImageRead) {
                auto value = [&] {
                    if (!image.info.props.is_block)
                        return LoadTexel(ir, image, IR::U32{address});
                    const auto words = ir.LoadBufferU32(image.info.num_bits == 64 ? 2u : 4u, data,
                                                        IR::U32{address}, {});
                    return DecodeCompressed(ir, image, words,
                                            IR::U32{ir.CompositeExtract(inst.Arg(1), 0)},
                                            IR::U32{ir.CompositeExtract(inst.Arg(1), 1)});
                }();
                const auto in_bounds =
                    ir.ILessThan(byte_address, ir.Imm32(image.info.guest_size), false);
                const auto component = [&](u32 index) {
                    return ir.Select(in_bounds, ir.CompositeExtract(value, index), ir.Imm32(0.f));
                };
                value =
                    ir.CompositeConstruct(component(0), component(1), component(2), component(3));
                if (!image.resource.is_written && !image.resource.is_feedback) {
                    if (image.sharp.GetNumberFmt() == AmdGpu::NumberFormat::Srgb)
                        value = IR::ApplyForceDegamma(ir, value);
                    value = IR::ApplySwizzle(ir, value, image.sharp.DstSelect(),
                                             AmdGpu::IsInteger(image.sharp.GetNumberFmt()));
                }
                inst.ReplaceUsesWithAndRemove(value);
            } else if (opcode == IR::Opcode::ImageWrite) {
                if (image.info.props.is_block)
                    throw std::runtime_error("Guest BC formats do not support typed image stores; "
                                             "write blocks through a buffer alias");
                StoreTexel(ir, image, IR::U32{address}, inst.Arg(4));
                ir.DeviceMemoryBarrier();
                inst.Invalidate();
            } else {
                if (image.info.num_bits != 32 ||
                    image.sharp.GetDataFmt() != AmdGpu::DataFormat::Format32)
                    throw std::runtime_error("Shared image atomics require one 32-bit component");
                inst.ReplaceUsesWithAndRemove(Atomic(ir, inst, data, IR::U32{address}));
                ir.DeviceMemoryBarrier();
            }
        }
    }
    for (u32 i = info.images.size(); i > 0; --i)
        if (selected[i - 1])
            info.images.erase(info.images.begin() + i - 1);
    return true;
}
bool LowerBufferAliases(CompiledShader& shader, IR::Program& program,
                        const ResourceCache* resources) {
    auto& info = shader.info;
    const u32 count = info.buffers.size();
    if (std::ranges::none_of(info.buffers, [](const auto& buffer) {
            return buffer.buffer_type == Shader::BufferType::Guest;
        }))
        return false;
    for (u32 i = 0; i < count; ++i) {
        auto& buffer = info.buffers[i];
        if (buffer.buffer_type != Shader::BufferType::Guest)
            continue;
        const auto sharp = buffer.GetSharp(info);
        const u64 size = u64(sharp.num_records) * std::max<u32>(1, sharp.stride);
        buffer.is_coherent |= resources && resources->HasRepeatedBacking(sharp.base_address, size);
        for (u32 j = 0; j < count; ++j) {
            const auto& other = info.buffers[j];
            if (i == j || other.buffer_type != Shader::BufferType::Guest ||
                !(buffer.is_written || other.is_written))
                continue;
            const auto peer = other.GetSharp(info);
            const u64 peer_size = u64(peer.num_records) * std::max<u32>(1, peer.stride);
            buffer.is_coherent |=
                resources
                    ? resources->Aliases(sharp.base_address, size, peer.base_address, peer_size)
                    : size && peer_size && sharp.base_address < peer.base_address + peer_size &&
                          peer.base_address < sharp.base_address + size;
        }
    }
    const auto table = IR::Value{
        AddBuffer(info, 1, 16 * 1024 * 1024, false, Shader::BufferType::BufferAddressTable)};
    for (auto* block : program.blocks) {
        for (auto& inst : block->Instructions()) {
            if (!Optimization::IsBufferInstruction(inst) || !inst.Arg(0).IsImmediate())
                continue;
            const u32 handle = inst.Arg(0).U32();
            if (handle >= count || info.buffers[handle].buffer_type != Shader::BufferType::Guest)
                continue;
            IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
            const auto metadata = [&](u32 field) {
                return IR::U32{ir.LoadBufferU32(1, table, ir.Imm32(handle * 4 + field), {})};
            };
            const auto map = metadata(0);
            const auto shift = metadata(1);
            const auto bias = metadata(2);
            const auto size = metadata(3);
            const auto translate = [&](IR::U32 address, u32 scale) {
                const auto bytes = ir.ShiftLeftLogical(address, ir.Imm32(scale));
                const auto valid = ir.ILessThan(bytes, size, false);
                const auto offset = ir.IAdd(bytes, bias);
                const auto page =
                    ir.Select(valid, ir.ShiftRightLogical(offset, shift), ir.Imm32(0));
                const auto base =
                    IR::U32{ir.LoadBufferU32(1, table, ir.IAdd(map, IR::U32{page}), {})};
                const auto mask = ir.ISub(ir.ShiftLeftLogical(ir.Imm32(1), shift), ir.Imm32(1));
                const auto physical = ir.IAdd(base, ir.BitwiseAnd(offset, mask));
                return ir.ShiftRightLogical(
                    IR::U32{ir.Select(valid, physical, ir.Imm32(UINT32_MAX))}, ir.Imm32(scale));
            };
            const auto opcode = inst.GetOpcode();
            if (info.buffers[handle].is_coherent) {
                ir.DeviceMemoryBarrier();
                if (Optimization::IsBufferStore(inst)) {
                    IR::IREmitter after{*block,
                                        std::next(IR::Block::InstructionList::s_iterator_to(inst))};
                    after.DeviceMemoryBarrier();
                }
            }
            if (opcode == IR::Opcode::LoadBufferU64 || opcode == IR::Opcode::StoreBufferU64) {
                info.buffers[handle].used_types |= IR::Type::U32;
                const auto address = ir.ShiftLeftLogical(IR::U32{inst.Arg(1)}, ir.Imm32(1));
                const auto first = translate(address, 2);
                const auto second = translate(ir.IAdd(address, ir.Imm32(1)), 2);
                const auto flags = inst.Flags<IR::BufferInstInfo>();
                if (opcode == IR::Opcode::StoreBufferU64) {
                    const auto words = ir.UnpackUint2x32(IR::U64{inst.Arg(2)});
                    ir.StoreBufferU32(1, inst.Arg(0), first, ir.CompositeExtract(words, 0), flags);
                    ir.StoreBufferU32(1, inst.Arg(0), second, ir.CompositeExtract(words, 1), flags);
                    inst.Invalidate();
                } else {
                    const auto low = ir.LoadBufferU32(1, inst.Arg(0), first, flags);
                    const auto high = ir.LoadBufferU32(1, inst.Arg(0), second, flags);
                    inst.ReplaceUsesWithAndRemove(
                        ir.PackUint2x32(ir.CompositeConstruct(low, high)));
                }
                continue;
            }
            u32 scale = 2;
            if (opcode == IR::Opcode::LoadBufferU8 || opcode == IR::Opcode::StoreBufferU8)
                scale = 0;
            else if (opcode == IR::Opcode::LoadBufferU16 || opcode == IR::Opcode::StoreBufferU16)
                scale = 1;
            else if (opcode == IR::Opcode::LoadBufferU64 || opcode == IR::Opcode::StoreBufferU64 ||
                     inst.Type() == IR::Type::U64)
                scale = 3;
            u32 components = 1;
            bool floating{};
            bool store{};
            switch (opcode) {
            case IR::Opcode::LoadBufferU32x2:
                components = 2;
                break;
            case IR::Opcode::LoadBufferU32x3:
                components = 3;
                break;
            case IR::Opcode::LoadBufferU32x4:
                components = 4;
                break;
            case IR::Opcode::LoadBufferF32x2:
                components = 2;
                floating = true;
                break;
            case IR::Opcode::LoadBufferF32x3:
                components = 3;
                floating = true;
                break;
            case IR::Opcode::LoadBufferF32x4:
                components = 4;
                floating = true;
                break;
            case IR::Opcode::StoreBufferU32x2:
                components = 2;
                store = true;
                break;
            case IR::Opcode::StoreBufferU32x3:
                components = 3;
                store = true;
                break;
            case IR::Opcode::StoreBufferU32x4:
                components = 4;
                store = true;
                break;
            case IR::Opcode::StoreBufferF32x2:
                components = 2;
                store = floating = true;
                break;
            case IR::Opcode::StoreBufferF32x3:
                components = 3;
                store = floating = true;
                break;
            case IR::Opcode::StoreBufferF32x4:
                components = 4;
                store = floating = true;
                break;
            default:
                break;
            }
            if (components == 1) {
                inst.SetArg(1, translate(IR::U32{inst.Arg(1)}, scale));
                continue;
            }
            std::array<IR::Value, 4> values;
            for (u32 i = 0; i < components; ++i) {
                const auto address = translate(ir.IAdd(IR::U32{inst.Arg(1)}, ir.Imm32(i)), 2);
                const auto flags = inst.Flags<IR::BufferInstInfo>();
                if (store) {
                    const auto value = ir.CompositeExtract(inst.Arg(2), i);
                    if (floating)
                        ir.StoreBufferF32(1, inst.Arg(0), address, value, flags);
                    else
                        ir.StoreBufferU32(1, inst.Arg(0), address, value, flags);
                } else {
                    values[i] = floating ? ir.LoadBufferF32(1, inst.Arg(0), address, flags)
                                         : ir.LoadBufferU32(1, inst.Arg(0), address, flags);
                }
            }
            if (store)
                inst.Invalidate();
            else
                inst.ReplaceUsesWithAndRemove(
                    ir.CompositeConstruct(std::span<const IR::Value>{values.data(), components}));
        }
    }
    return true;
}
} // namespace D3D12
