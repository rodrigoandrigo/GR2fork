// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {

namespace {
using PointerType = EmitContext::PointerType;
using PointerSize = EmitContext::PointerSize;

std::pair<Id, Id> AtomicArgs(EmitContext& ctx) {
    const Id scope{ctx.ConstU32(static_cast<u32>(spv::Scope::Device))};
    const Id semantics{ctx.u32_zero_value};
    return {scope, semantics};
}

Id SharedAtomicU32(EmitContext& ctx, Id offset, Id value,
                   Id (Sirit::Module::*atomic_func)(Id, Id, Id, Id, Id)) {
    const Id shift_id{ctx.ConstU32(2U)};
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], offset, shift_id)};
    const Id pointer{ctx.EmitSharedMemoryAccess(ctx.shared_u32, ctx.shared_memory_u32, index)};
    const auto [scope, semantics]{AtomicArgs(ctx)};
    return (ctx.*atomic_func)(ctx.U32[1], pointer, scope, semantics, value);
}

Id SharedAtomicU32IncDec(EmitContext& ctx, Id offset,
                         Id (Sirit::Module::*atomic_func)(Id, Id, Id, Id)) {
    const Id shift_id{ctx.ConstU32(2U)};
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], offset, shift_id)};
    const Id pointer{ctx.EmitSharedMemoryAccess(ctx.shared_u32, ctx.shared_memory_u32, index)};
    const auto [scope, semantics]{AtomicArgs(ctx)};
    return (ctx.*atomic_func)(ctx.U32[1], pointer, scope, semantics);
}

Id SharedAtomicU64(EmitContext& ctx, Id offset, Id value,
                   Id (Sirit::Module::*atomic_func)(Id, Id, Id, Id, Id)) {
    const Id shift_id{ctx.ConstU32(3U)};
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], offset, shift_id)};
    const Id pointer{ctx.EmitSharedMemoryAccess(ctx.shared_u64, ctx.shared_memory_u64, index)};
    const auto [scope, semantics]{AtomicArgs(ctx)};
    return (ctx.*atomic_func)(ctx.U64, pointer, scope, semantics, value);
}

Id SharedAtomicU64IncDec(EmitContext& ctx, Id offset,
                         Id (Sirit::Module::*atomic_func)(Id, Id, Id, Id)) {
    const Id shift_id{ctx.ConstU32(3U)};
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], offset, shift_id)};
    const Id pointer{ctx.EmitSharedMemoryAccess(ctx.shared_u64, ctx.shared_memory_u64, index)};
    const auto [scope, semantics]{AtomicArgs(ctx)};
    return (ctx.*atomic_func)(ctx.U64, pointer, scope, semantics);
}

Id SharedAtomicU32CmpSwap(EmitContext& ctx, Id offset, Id value, Id cmp_value) {
    const Id shift_id{ctx.ConstU32(2U)};
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], offset, shift_id)};
    const Id pointer{ctx.EmitSharedMemoryAccess(ctx.shared_u32, ctx.shared_memory_u32, index)};
    const auto [scope, semantics]{AtomicArgs(ctx)};
    return ctx.OpAtomicCompareExchange(ctx.U32[1], pointer, scope, semantics, semantics, value,
                                       cmp_value);
}

Id SharedAtomicU64CmpSwap(EmitContext& ctx, Id offset, Id value, Id cmp_value) {
    const Id shift_id{ctx.ConstU32(3U)};
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], offset, shift_id)};
    const Id pointer{ctx.EmitSharedMemoryAccess(ctx.shared_u64, ctx.shared_memory_u64, index)};
    const auto [scope, semantics]{AtomicArgs(ctx)};
    return ctx.OpAtomicCompareExchange(ctx.U64, pointer, scope, semantics, semantics, value,
                                       cmp_value);
}

Id BufferAtomicPointer(EmitContext& ctx, u32 handle, Id address) {
    const auto& buffer = ctx.buffers[handle];
    if (const Id offset = buffer.Offset(PointerSize::B32); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto [id, pointer_type] = buffer.Alias(PointerType::U32);
    return ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, address);
}

template <typename Update>
Id AtomicUpdate(EmitContext& ctx, Id pointer, Update update) {
    const auto [scope, semantics] = AtomicArgs(ctx);
    const Id header = ctx.OpLabel();
    const Id body = ctx.OpLabel();
    const Id retry = ctx.OpLabel();
    const Id merge = ctx.OpLabel();
    ctx.OpBranch(header);
    ctx.AddLabel(header);
    ctx.OpLoopMerge(merge, retry, spv::LoopControlMask::MaskNone);
    ctx.OpBranch(body);
    ctx.AddLabel(body);
    const Id expected = ctx.OpAtomicOr(ctx.U32[1], pointer, scope, semantics, ctx.u32_zero_value);
    const Id replacement = update(expected);
    const Id observed = ctx.OpAtomicCompareExchange(ctx.U32[1], pointer, scope, semantics,
                                                    semantics, replacement, expected);
    ctx.OpBranchConditional(ctx.OpIEqual(ctx.U1[1], observed, expected), merge, retry);
    ctx.AddLabel(retry);
    ctx.OpBranch(header);
    ctx.AddLabel(merge);
    return ctx.OpPhi(ctx.U32[1], expected, body);
}

Id FloatCompare(EmitContext& ctx, Id lhs, Id rhs, bool equal, bool maximum = false) {
    const Id lhs_mag = ctx.OpBitwiseAnd(ctx.U32[1], lhs, ctx.ConstU32(0x7fffffffu));
    const Id rhs_mag = ctx.OpBitwiseAnd(ctx.U32[1], rhs, ctx.ConstU32(0x7fffffffu));
    const Id ordered = ctx.OpLogicalAnd(
        ctx.U1[1], ctx.OpULessThanEqual(ctx.U1[1], lhs_mag, ctx.ConstU32(0x7f800000u)),
        ctx.OpULessThanEqual(ctx.U1[1], rhs_mag, ctx.ConstU32(0x7f800000u)));
    const Id both_zero =
        ctx.OpIEqual(ctx.U1[1], ctx.OpBitwiseOr(ctx.U32[1], lhs_mag, rhs_mag), ctx.u32_zero_value);
    if (equal) {
        return ctx.OpLogicalAnd(
            ctx.U1[1], ordered,
            ctx.OpLogicalOr(ctx.U1[1], both_zero, ctx.OpIEqual(ctx.U1[1], lhs, rhs)));
    }
    const auto key = [&](Id bits) {
        const Id negative = ctx.OpINotEqual(
            ctx.U1[1], ctx.OpBitwiseAnd(ctx.U32[1], bits, ctx.ConstU32(0x80000000u)),
            ctx.u32_zero_value);
        return ctx.OpSelect(ctx.U32[1], negative, ctx.OpNot(ctx.U32[1], bits),
                            ctx.OpBitwiseXor(ctx.U32[1], bits, ctx.ConstU32(0x80000000u)));
    };
    const Id left_key = key(lhs);
    const Id right_key = key(rhs);
    const Id comparison = maximum ? ctx.OpUGreaterThan(ctx.U1[1], left_key, right_key)
                                  : ctx.OpULessThan(ctx.U1[1], left_key, right_key);
    return ctx.OpLogicalAnd(
        ctx.U1[1], ordered,
        ctx.OpLogicalAnd(ctx.U1[1], ctx.OpLogicalNot(ctx.U1[1], both_zero), comparison));
}

Id AtomicSignedMinMax(EmitContext& ctx, Id pointer, Id value, bool maximum) {
    const Id signed_value = ctx.OpBitcast(ctx.S32[1], value);
    return AtomicUpdate(ctx, pointer, [&](Id old) {
        const Id signed_old = ctx.OpBitcast(ctx.S32[1], old);
        const Id compare = maximum ? ctx.OpSGreaterThan(ctx.U1[1], signed_value, signed_old)
                                   : ctx.OpSLessThan(ctx.U1[1], signed_value, signed_old);
        return ctx.OpSelect(ctx.U32[1], compare, value, old);
    });
}

Id AtomicFloatMinMax(EmitContext& ctx, Id pointer, Id value, bool maximum) {
    const Id bits = ctx.OpBitcast(ctx.U32[1], value);
    const Id result = AtomicUpdate(ctx, pointer, [&](Id old) {
        return ctx.OpSelect(ctx.U32[1], FloatCompare(ctx, bits, old, false, maximum), bits, old);
    });
    return ctx.OpBitcast(ctx.F32[1], result);
}

Id AtomicFloatCmpSwap(EmitContext& ctx, Id pointer, Id value, Id compare) {
    const Id bits = ctx.OpBitcast(ctx.U32[1], value);
    const Id cmp_bits = ctx.OpBitcast(ctx.U32[1], compare);
    const Id result = AtomicUpdate(ctx, pointer, [&](Id old) {
        return ctx.OpSelect(ctx.U32[1], FloatCompare(ctx, old, cmp_bits, true), bits, old);
    });
    return ctx.OpBitcast(ctx.F32[1], result);
}

Id AtomicIncDec(EmitContext& ctx, Id pointer, Id limit, bool decrement) {
    return AtomicUpdate(ctx, pointer, [&](Id old) {
        if (decrement) {
            const Id wrap =
                ctx.OpLogicalOr(ctx.U1[1], ctx.OpIEqual(ctx.U1[1], old, ctx.u32_zero_value),
                                ctx.OpUGreaterThan(ctx.U1[1], old, limit));
            return ctx.OpSelect(ctx.U32[1], wrap, limit,
                                ctx.OpISub(ctx.U32[1], old, ctx.ConstU32(1u)));
        }
        return ctx.OpSelect(ctx.U32[1], ctx.OpUGreaterThanEqual(ctx.U1[1], old, limit),
                            ctx.u32_zero_value, ctx.OpIAdd(ctx.U32[1], old, ctx.ConstU32(1u)));
    });
}

template <bool is_float = false>
Id BufferAtomicU32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value,
                   Id (Sirit::Module::*atomic_func)(Id, Id, Id, Id, Id)) {
    const auto& buffer = ctx.buffers[handle];
    const Id type = is_float ? ctx.F32[1] : ctx.U32[1];
    if (const Id offset = buffer.Offset(PointerSize::B32); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto [id, pointer_type] = buffer.Alias(is_float ? PointerType::F32 : PointerType::U32);
    const Id ptr = ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, address);
    const auto [scope, semantics]{AtomicArgs(ctx)};
    return (ctx.*atomic_func)(type, ptr, scope, semantics, value);
}

Id BufferAtomicU32CmpSwap(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value,
                          Id cmp_value,
                          Id (Sirit::Module::*atomic_func)(Id, Id, Id, Id, Id, Id, Id)) {
    const auto& buffer = ctx.buffers[handle];
    if (const Id offset = buffer.Offset(PointerSize::B32); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto [id, pointer_type] = buffer.Alias(PointerType::U32);
    const Id ptr = ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, address);
    const auto [scope, semantics]{AtomicArgs(ctx)};
    return (ctx.*atomic_func)(ctx.U32[1], ptr, scope, semantics, semantics, value, cmp_value);
}

Id BufferAtomicU64(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value,
                   Id (Sirit::Module::*atomic_func)(Id, Id, Id, Id, Id)) {
    const auto& buffer = ctx.buffers[handle];
    if (const Id offset = buffer.Offset(PointerSize::B64); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto [id, pointer_type] = buffer.Alias(PointerType::U64);
    const Id ptr = ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, address);
    const auto [scope, semantics]{AtomicArgs(ctx)};
    return (ctx.*atomic_func)(ctx.U64, ptr, scope, semantics, value);
}

Id FixImageAtomicCoords(EmitContext& ctx, Id coords, AmdGpu::ImageType image_type) {
    switch (image_type) {
    case AmdGpu::ImageType::Color1D: {
        // Lowered to 2D with height 1
        const auto x = coords;
        return ctx.OpCompositeConstruct(ctx.U32[2], x, ctx.u32_zero_value);
    }
    case AmdGpu::ImageType::Color1DArray: {
        // Lowered to 2D array with height 1
        const auto x = ctx.OpCompositeExtract(ctx.U32[1], coords, 0U);
        const auto slice = ctx.OpCompositeExtract(ctx.U32[1], coords, 1U);
        return ctx.OpCompositeConstruct(ctx.U32[3], x, ctx.u32_zero_value, slice);
    }
    default:
        return coords;
    }
}

Id ImageAtomicPointer(EmitContext& ctx, u32 handle, Id coords, bool is_float = false) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    Id sample = ctx.u32_zero_value;
    if (texture.view_type == AmdGpu::ImageType::Color2DMsaa) {
        sample = ctx.OpCompositeExtract(ctx.U32[1], coords, 2U);
        coords = ctx.OpVectorShuffle(ctx.U32[2], coords, coords, 0U, 1U);
    } else if (texture.view_type == AmdGpu::ImageType::Color2DMsaaArray) {
        sample = ctx.OpCompositeExtract(ctx.U32[1], coords, 3U);
        coords = ctx.OpVectorShuffle(ctx.U32[3], coords, coords, 0U, 1U, 2U);
    }
    const Id fixed_coords = FixImageAtomicCoords(ctx, coords, texture.view_type);
    return ctx.OpImageTexelPointer(is_float ? ctx.image_f32 : ctx.image_u32, texture.id,
                                   fixed_coords, sample);
}

Id ImageAtomicU32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value,
                  Id (Sirit::Module::*atomic_func)(Id, Id, Id, Id, Id)) {
    const Id pointer = ImageAtomicPointer(ctx, handle, coords);
    const auto [scope, semantics]{AtomicArgs(ctx)};
    return (ctx.*atomic_func)(ctx.U32[1], pointer, scope, semantics, value);
}

Id ImageAtomicF32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value,
                  Id (Sirit::Module::*atomic_func)(Id, Id, Id, Id, Id)) {
    const Id pointer = ImageAtomicPointer(ctx, handle, coords, true);
    const auto [scope, semantics]{AtomicArgs(ctx)};
    return (ctx.*atomic_func)(ctx.F32[1], pointer, scope, semantics, value);
}

Id ImageAtomicU32CmpSwap(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value,
                         Id cmp_value,
                         Id (Sirit::Module::*atomic_func)(Id, Id, Id, Id, Id, Id, Id)) {
    const Id pointer = ImageAtomicPointer(ctx, handle, coords);
    const auto [scope, semantics]{AtomicArgs(ctx)};
    return (ctx.*atomic_func)(ctx.U32[1], pointer, scope, semantics, semantics, value, cmp_value);
}
} // Anonymous namespace

Id EmitSharedAtomicIAdd32(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU32(ctx, offset, value, &Sirit::Module::OpAtomicIAdd);
}

Id EmitSharedAtomicIAdd64(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU64(ctx, offset, value, &Sirit::Module::OpAtomicIAdd);
}

Id EmitSharedAtomicUMax32(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU32(ctx, offset, value, &Sirit::Module::OpAtomicUMax);
}

Id EmitSharedAtomicUMax64(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU64(ctx, offset, value, &Sirit::Module::OpAtomicUMax);
}

Id EmitSharedAtomicSMax32(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU32(ctx, offset, value, &Sirit::Module::OpAtomicSMax);
}

Id EmitSharedAtomicSMax64(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU64(ctx, offset, value, &Sirit::Module::OpAtomicSMax);
}

Id EmitSharedAtomicUMin32(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU32(ctx, offset, value, &Sirit::Module::OpAtomicUMin);
}

Id EmitSharedAtomicUMin64(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU64(ctx, offset, value, &Sirit::Module::OpAtomicUMin);
}

Id EmitSharedAtomicSMin32(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU32(ctx, offset, value, &Sirit::Module::OpAtomicSMin);
}

Id EmitSharedAtomicSMin64(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU64(ctx, offset, value, &Sirit::Module::OpAtomicSMin);
}

Id EmitSharedAtomicAnd32(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU32(ctx, offset, value, &Sirit::Module::OpAtomicAnd);
}

Id EmitSharedAtomicAnd64(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU64(ctx, offset, value, &Sirit::Module::OpAtomicAnd);
}

Id EmitSharedAtomicOr32(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU32(ctx, offset, value, &Sirit::Module::OpAtomicOr);
}

Id EmitSharedAtomicOr64(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU64(ctx, offset, value, &Sirit::Module::OpAtomicOr);
}

Id EmitSharedAtomicXor32(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU32(ctx, offset, value, &Sirit::Module::OpAtomicXor);
}

Id EmitSharedAtomicXor64(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU64(ctx, offset, value, &Sirit::Module::OpAtomicXor);
}

Id EmitSharedAtomicISub32(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU32(ctx, offset, value, &Sirit::Module::OpAtomicISub);
}

Id EmitSharedAtomicISub64(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU64(ctx, offset, value, &Sirit::Module::OpAtomicISub);
}

Id EmitSharedAtomicCmpSwap32(EmitContext& ctx, Id offset, Id value, Id cmp_value) {
    return SharedAtomicU32CmpSwap(ctx, offset, value, cmp_value);
}

Id EmitSharedAtomicCmpSwap64(EmitContext& ctx, Id offset, Id value, Id cmp_value) {
    return SharedAtomicU64CmpSwap(ctx, offset, value, cmp_value);
}

Id EmitSharedAtomicInc32(EmitContext& ctx, Id offset) {
    return SharedAtomicU32IncDec(ctx, offset, &Sirit::Module::OpAtomicIIncrement);
}

Id EmitSharedAtomicInc64(EmitContext& ctx, Id offset) {
    return SharedAtomicU64IncDec(ctx, offset, &Sirit::Module::OpAtomicIIncrement);
}

Id EmitSharedAtomicDec32(EmitContext& ctx, Id offset) {
    return SharedAtomicU32IncDec(ctx, offset, &Sirit::Module::OpAtomicIDecrement);
}

Id EmitSharedAtomicDec64(EmitContext& ctx, Id offset) {
    return SharedAtomicU64IncDec(ctx, offset, &Sirit::Module::OpAtomicIDecrement);
}

Id EmitBufferAtomicIAdd32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU32(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicIAdd);
}

Id EmitBufferAtomicIAdd64(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU64(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicIAdd);
}

Id EmitBufferAtomicISub32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU32(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicISub);
}

Id EmitBufferAtomicSMin32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    if (ctx.profile.needs_signed_atomic32_emulation) {
        return AtomicSignedMinMax(ctx, BufferAtomicPointer(ctx, handle, address), value, false);
    }
    return BufferAtomicU32(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicSMin);
}

Id EmitBufferAtomicSMin64(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU64(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicSMin);
}

Id EmitBufferAtomicUMin32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU32(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicUMin);
}

Id EmitBufferAtomicUMin64(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU64(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicUMin);
}

Id EmitBufferAtomicFMin32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    if (ctx.profile.supports_buffer_fp32_atomic_min_max) {
        return BufferAtomicU32<true>(ctx, inst, handle, address, value,
                                     &Sirit::Module::OpAtomicFMin);
    }
    return AtomicFloatMinMax(ctx, BufferAtomicPointer(ctx, handle, address), value, false);
}

Id EmitBufferAtomicSMax32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    if (ctx.profile.needs_signed_atomic32_emulation) {
        return AtomicSignedMinMax(ctx, BufferAtomicPointer(ctx, handle, address), value, true);
    }
    return BufferAtomicU32(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicSMax);
}

Id EmitBufferAtomicSMax64(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU64(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicSMax);
}

Id EmitBufferAtomicUMax32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU32(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicUMax);
}

Id EmitBufferAtomicUMax64(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU64(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicUMax);
}

Id EmitBufferAtomicFMax32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    if (ctx.profile.supports_buffer_fp32_atomic_min_max) {
        return BufferAtomicU32<true>(ctx, inst, handle, address, value,
                                     &Sirit::Module::OpAtomicFMax);
    }
    return AtomicFloatMinMax(ctx, BufferAtomicPointer(ctx, handle, address), value, true);
}

Id EmitBufferAtomicInc32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id limit) {
    return AtomicIncDec(ctx, BufferAtomicPointer(ctx, handle, address), limit, false);
}

Id EmitBufferAtomicDec32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id limit) {
    return AtomicIncDec(ctx, BufferAtomicPointer(ctx, handle, address), limit, true);
}

Id EmitBufferAtomicAnd32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU32(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicAnd);
}

Id EmitBufferAtomicOr32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU32(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicOr);
}

Id EmitBufferAtomicXor32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU32(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicXor);
}

Id EmitBufferAtomicSwap32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU32(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicExchange);
}

Id EmitBufferAtomicCmpSwap32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value,
                             Id cmp_value) {
    return BufferAtomicU32CmpSwap(ctx, inst, handle, address, value, cmp_value,
                                  &Sirit::Module::OpAtomicCompareExchange);
}

Id EmitBufferAtomicFCmpSwap32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value,
                              Id cmp_value) {
    return AtomicFloatCmpSwap(ctx, BufferAtomicPointer(ctx, handle, address), value, cmp_value);
}

Id EmitImageAtomicIAdd32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value) {
    return ImageAtomicU32(ctx, inst, handle, coords, value, &Sirit::Module::OpAtomicIAdd);
}

Id EmitImageAtomicSMin32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value) {
    if (ctx.profile.needs_signed_atomic32_emulation) {
        return AtomicSignedMinMax(ctx, ImageAtomicPointer(ctx, handle, coords), value, false);
    }
    return ImageAtomicU32(ctx, inst, handle, coords, value, &Sirit::Module::OpAtomicSMin);
}

Id EmitImageAtomicUMin32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value) {
    return ImageAtomicU32(ctx, inst, handle, coords, value, &Sirit::Module::OpAtomicUMin);
}

Id EmitImageAtomicSMax32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value) {
    if (ctx.profile.needs_signed_atomic32_emulation) {
        return AtomicSignedMinMax(ctx, ImageAtomicPointer(ctx, handle, coords), value, true);
    }
    return ImageAtomicU32(ctx, inst, handle, coords, value, &Sirit::Module::OpAtomicSMax);
}

Id EmitImageAtomicUMax32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value) {
    return ImageAtomicU32(ctx, inst, handle, coords, value, &Sirit::Module::OpAtomicUMax);
}

Id EmitImageAtomicFMax32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value) {
    if (ctx.profile.supports_image_fp32_atomic_min_max) {
        return ImageAtomicF32(ctx, inst, handle, coords, value, &Sirit::Module::OpAtomicFMax);
    }
    return AtomicFloatMinMax(ctx, ImageAtomicPointer(ctx, handle, coords), value, true);
}

Id EmitImageAtomicFMin32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value) {
    if (ctx.profile.supports_image_fp32_atomic_min_max) {
        return ImageAtomicF32(ctx, inst, handle, coords, value, &Sirit::Module::OpAtomicFMin);
    }
    return AtomicFloatMinMax(ctx, ImageAtomicPointer(ctx, handle, coords), value, false);
}

Id EmitImageAtomicInc32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id limit) {
    return AtomicIncDec(ctx, ImageAtomicPointer(ctx, handle, coords), limit, false);
}

Id EmitImageAtomicDec32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id limit) {
    return AtomicIncDec(ctx, ImageAtomicPointer(ctx, handle, coords), limit, true);
}

Id EmitImageAtomicAnd32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value) {
    return ImageAtomicU32(ctx, inst, handle, coords, value, &Sirit::Module::OpAtomicAnd);
}

Id EmitImageAtomicOr32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value) {
    return ImageAtomicU32(ctx, inst, handle, coords, value, &Sirit::Module::OpAtomicOr);
}

Id EmitImageAtomicXor32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value) {
    return ImageAtomicU32(ctx, inst, handle, coords, value, &Sirit::Module::OpAtomicXor);
}

Id EmitImageAtomicExchange32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value) {
    return ImageAtomicU32(ctx, inst, handle, coords, value, &Sirit::Module::OpAtomicExchange);
}

Id EmitImageAtomicCmpSwap32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value,
                            Id cmp_value) {
    return ImageAtomicU32CmpSwap(ctx, inst, handle, coords, value, cmp_value,
                                 &Sirit::Module::OpAtomicCompareExchange);
}

Id EmitImageAtomicFCmpSwap32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value,
                             Id cmp_value) {
    return AtomicFloatCmpSwap(ctx, ImageAtomicPointer(ctx, handle, coords), value, cmp_value);
}

Id EmitDataAppend(EmitContext& ctx, Id gds_dw_offset, Id exec) {
    UNREACHABLE_MSG("SPIR-V Instruction");
}

Id EmitDataConsume(EmitContext& ctx, Id gds_dw_offset, Id exec) {
    UNREACHABLE_MSG("SPIR-V Instruction");
}

} // namespace Shader::Backend::SPIRV
