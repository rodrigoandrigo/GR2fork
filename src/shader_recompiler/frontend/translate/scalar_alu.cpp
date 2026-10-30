// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <magic_enum/magic_enum.hpp>
#include "common/assert.h"
#include "shader_recompiler/frontend/translate/translate.h"

namespace Shader::Gcn {

void Translator::EmitScalarAlu(const GcnInst& inst) {
    switch (inst.encoding) {
    case InstEncoding::SOPC: {
        EmitSOPC(inst);
        break;
    }
    case InstEncoding::SOPK: {
        EmitSOPK(inst);
        break;
    }
    default:
        switch (inst.opcode) {
            // SOP2
        case Opcode::S_ADD_U32:
            return S_ADD_U32(inst);
        case Opcode::S_SUB_U32:
            return S_SUB_U32(inst);
        case Opcode::S_ADD_I32:
            return S_ADD_I32(inst);
        case Opcode::S_SUB_I32:
            return S_SUB_I32(inst);
        case Opcode::S_ADDC_U32:
            return S_ADDC_U32(inst);
        case Opcode::S_SUBB_U32:
            return S_SUBB_U32(inst);
        case Opcode::S_MIN_I32:
            return S_MIN_U32(true, inst);
        case Opcode::S_MIN_U32:
            return S_MIN_U32(false, inst);
        case Opcode::S_MAX_I32:
            return S_MAX_U32(true, inst);
        case Opcode::S_MAX_U32:
            return S_MAX_U32(false, inst);
        case Opcode::S_CSELECT_B32:
            return S_CSELECT_B32(inst);
        case Opcode::S_CSELECT_B64:
            return S_CSELECT_B64(inst);
        case Opcode::S_AND_B32:
            return S_AND_B32(NegateMode::None, inst);
        case Opcode::S_AND_B64:
            return S_AND_B64(NegateMode::None, inst);
        case Opcode::S_OR_B32:
            return S_OR_B32(NegateMode::None, false, inst);
        case Opcode::S_OR_B64:
            return S_OR_B64(NegateMode::None, false, inst);
        case Opcode::S_XOR_B32:
            return S_XOR_B32(inst);
        case Opcode::S_NOT_B32:
            return S_NOT_B32(inst);
        case Opcode::S_XOR_B64:
            return S_OR_B64(NegateMode::None, true, inst);
        case Opcode::S_ANDN2_B32:
            return S_AND_B32(NegateMode::Src1, inst);
        case Opcode::S_ANDN2_B64:
            return S_AND_B64(NegateMode::Src1, inst);
        case Opcode::S_ORN2_B32:
            return S_OR_B32(NegateMode::Src1, false, inst);
        case Opcode::S_NOR_B32:
            return S_OR_B32(NegateMode::Result, false, inst);
        case Opcode::S_XNOR_B32:
            return S_OR_B32(NegateMode::Result, true, inst);
        case Opcode::S_ORN2_B64:
            return S_OR_B64(NegateMode::Src1, false, inst);
        case Opcode::S_NAND_B32:
            return S_AND_B32(NegateMode::Result, inst);
        case Opcode::S_NAND_B64:
            return S_AND_B64(NegateMode::Result, inst);
        case Opcode::S_NOR_B64:
            return S_OR_B64(NegateMode::Result, false, inst);
        case Opcode::S_XNOR_B64:
            return S_OR_B64(NegateMode::Result, true, inst);
        case Opcode::S_LSHL_B32:
            return S_LSHL_B32(inst);
        case Opcode::S_LSHL_B64:
            return S_LSHL_B64(inst);
        case Opcode::S_LSHR_B32:
            return S_LSHR_B32(inst);
        case Opcode::S_LSHR_B64:
            return S_LSHR_B64(inst);
        case Opcode::S_ASHR_I32:
            return S_ASHR_I32(inst);
        case Opcode::S_ASHR_I64:
            return S_ASHR_I64(inst);
        case Opcode::S_BFM_B32:
            return S_BFM_B32(inst);
        case Opcode::S_BFM_B64:
            return S_BFM_B64(inst);
        case Opcode::S_MUL_I32:
            return S_MUL_I32(inst);
        case Opcode::S_BFE_I32:
            return S_BFE(inst, true);
        case Opcode::S_BFE_U32:
            return S_BFE(inst, false);
        case Opcode::S_BFE_U64:
            return S_BFE64(inst, false);
        case Opcode::S_BFE_I64:
            return S_BFE64(inst, true);
        case Opcode::S_ABSDIFF_I32:
            return S_ABSDIFF_I32(inst);

        // SOP1
        case Opcode::S_MOV_B32:
            return S_MOV(inst);
        case Opcode::S_MOVRELS_B32:
            return S_MOVRELS_B32(inst);
        case Opcode::S_MOVRELS_B64:
            return S_MOVRELS_B64(inst);
        case Opcode::S_MOVRELD_B32:
            return S_MOVRELD_B32(inst);
        case Opcode::S_MOVRELD_B64:
            return S_MOVRELD_B64(inst);
        case Opcode::S_CMOV_B32:
            return S_MOV(inst, true);
        case Opcode::S_SEXT_I32_I8:
            return S_SEXT_I32(inst, 8);
        case Opcode::S_SEXT_I32_I16:
            return S_SEXT_I32(inst, 16);
        case Opcode::S_MOV_B64:
            return S_MOV_B64(inst);
        case Opcode::S_CMOV_B64:
            return S_MOV_B64(inst, true);
        case Opcode::S_NOT_B64:
            return S_NOT_B64(inst);
        case Opcode::S_WQM_B32:
            return S_WQM_B32(inst);
        case Opcode::S_WQM_B64:
            return S_WQM_B64(inst);
        case Opcode::S_QUADMASK_B32:
            return S_QUADMASK_B32(inst);
        case Opcode::S_QUADMASK_B64:
            return S_QUADMASK_B64(inst);
        case Opcode::S_BREV_B32:
            return S_BREV_B32(inst);
        case Opcode::S_BREV_B64:
            return S_BREV_B64(inst);
        case Opcode::S_BCNT0_I32_B32:
            return S_BCNT0_I32_B32(inst);
        case Opcode::S_BCNT0_I32_B64:
            return S_BCNT0_I32_B64(inst);
        case Opcode::S_BCNT1_I32_B32:
            return S_BCNT1_I32_B32(inst);
        case Opcode::S_BCNT1_I32_B64:
            return S_BCNT1_I32_B64(inst);
        case Opcode::S_FF1_I32_B32:
            return S_FF1_I32_B32(inst);
        case Opcode::S_FF1_I32_B64:
            return S_FF1_I32_B64(inst);
        case Opcode::S_FF0_I32_B32:
            return S_FF0_I32_B32(inst);
        case Opcode::S_FF0_I32_B64:
            return S_FF0_I32_B64(inst);
        case Opcode::S_FLBIT_I32_B32:
            return S_FLBIT_I32_B32(inst);
        case Opcode::S_FLBIT_I32_B64:
            return S_FLBIT_I32_B64(inst);
        case Opcode::S_FLBIT_I32:
            return S_FLBIT_I32(inst);
        case Opcode::S_FLBIT_I32_I64:
            return S_FLBIT_I32_I64(inst);
        case Opcode::S_BITSET0_B32:
            return S_BITSET_B32(inst, 0);
        case Opcode::S_BITSET1_B32:
            return S_BITSET_B32(inst, 1);
        case Opcode::S_BITSET0_B64:
            return S_BITSET_B64(inst, 0);
        case Opcode::S_BITSET1_B64:
            return S_BITSET_B64(inst, 1);
        case Opcode::S_AND_SAVEEXEC_B64:
        case Opcode::S_OR_SAVEEXEC_B64:
        case Opcode::S_XOR_SAVEEXEC_B64:
        case Opcode::S_ANDN2_SAVEEXEC_B64:
        case Opcode::S_ORN2_SAVEEXEC_B64:
        case Opcode::S_NAND_SAVEEXEC_B64:
        case Opcode::S_NOR_SAVEEXEC_B64:
        case Opcode::S_XNOR_SAVEEXEC_B64:
            return S_SAVEEXEC_B64(inst);
        case Opcode::S_ABS_I32:
            return S_ABS_I32(inst);
        default:
            LogMissingOpcode(inst);
        }
        break;
    }
}

void Translator::EmitSOPC(const GcnInst& inst) {
    switch (inst.opcode) {
    case Opcode::S_CMP_EQ_I32:
        return S_CMP(ConditionOp::EQ, true, inst);
    case Opcode::S_CMP_LG_I32:
        return S_CMP(ConditionOp::LG, true, inst);
    case Opcode::S_CMP_GT_I32:
        return S_CMP(ConditionOp::GT, true, inst);
    case Opcode::S_CMP_GE_I32:
        return S_CMP(ConditionOp::GE, true, inst);
    case Opcode::S_CMP_LT_I32:
        return S_CMP(ConditionOp::LT, true, inst);
    case Opcode::S_CMP_LE_I32:
        return S_CMP(ConditionOp::LE, true, inst);

    case Opcode::S_CMP_EQ_U32:
        return S_CMP(ConditionOp::EQ, false, inst);
    case Opcode::S_CMP_LG_U32:
        return S_CMP(ConditionOp::LG, false, inst);
    case Opcode::S_CMP_GT_U32:
        return S_CMP(ConditionOp::GT, false, inst);
    case Opcode::S_CMP_GE_U32:
        return S_CMP(ConditionOp::GE, false, inst);
    case Opcode::S_CMP_LT_U32:
        return S_CMP(ConditionOp::LT, false, inst);
    case Opcode::S_CMP_LE_U32:
        return S_CMP(ConditionOp::LE, false, inst);

    case Opcode::S_BITCMP0_B32:
        return S_BITCMP(false, 32, inst);
    case Opcode::S_BITCMP1_B32:
        return S_BITCMP(true, 32, inst);
    case Opcode::S_BITCMP0_B64:
        return S_BITCMP(false, 64, inst);
    case Opcode::S_BITCMP1_B64:
        return S_BITCMP(true, 64, inst);

    default:
        LogMissingOpcode(inst);
    }
}

void Translator::EmitSOPK(const GcnInst& inst) {
    switch (inst.opcode) {
        // SOPK
    case Opcode::S_MOVK_I32:
        return S_MOVK(inst, false);
    case Opcode::S_CMOVK_I32:
        return S_MOVK(inst, true);
    case Opcode::S_CMPK_EQ_I32:
        return S_CMPK(ConditionOp::EQ, true, inst);
    case Opcode::S_CMPK_LG_I32:
        return S_CMPK(ConditionOp::LG, true, inst);
    case Opcode::S_CMPK_GT_I32:
        return S_CMPK(ConditionOp::GT, true, inst);
    case Opcode::S_CMPK_GE_I32:
        return S_CMPK(ConditionOp::GE, true, inst);
    case Opcode::S_CMPK_LT_I32:
        return S_CMPK(ConditionOp::LT, true, inst);
    case Opcode::S_CMPK_LE_I32:
        return S_CMPK(ConditionOp::LE, true, inst);

    case Opcode::S_CMPK_EQ_U32:
        return S_CMPK(ConditionOp::EQ, false, inst);
    case Opcode::S_CMPK_LG_U32:
        return S_CMPK(ConditionOp::LG, false, inst);
    case Opcode::S_CMPK_GT_U32:
        return S_CMPK(ConditionOp::GT, false, inst);
    case Opcode::S_CMPK_GE_U32:
        return S_CMPK(ConditionOp::GE, false, inst);
    case Opcode::S_CMPK_LT_U32:
        return S_CMPK(ConditionOp::LT, false, inst);
    case Opcode::S_CMPK_LE_U32:
        return S_CMPK(ConditionOp::LE, false, inst);

    case Opcode::S_ADDK_I32:
        return S_ADDK_I32(inst);
    case Opcode::S_MULK_I32:
        return S_MULK_I32(inst);
    default:
        LogMissingOpcode(inst);
    }
}

// SOP2

void Translator::S_ADD_U32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 result{ir.IAdd(src0, src1)};
    SetDst(inst.dst[0], result);

    // SCC = tmp >= 0x100000000ULL ? 1'1U : 1'0U;
    // The above assumes tmp is a 64-bit value.
    // It should be enough however to test that the truncated result is less than at least one
    // of the operands. In unsigned addition the result is always bigger than both the operands,
    // except in the case of overflow where the truncated result is less than both.
    ir.SetScc(ir.ILessThan(result, src0, false));
}

void Translator::S_SUB_U32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    SetDst(inst.dst[0], ir.ISub(src0, src1));

    // SCC = S1.u > S0.u ? 1'1U : 1'0U;
    ir.SetScc(ir.IGreaterThan(src1, src0, false));
}

void Translator::S_SUBB_U32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 borrow{ir.Select(ir.GetScc(), ir.Imm32(1U), ir.Imm32(0U))};
    const IR::U32 difference{ir.ISub(src0, src1)};
    const IR::U32 result{ir.ISub(difference, borrow)};
    SetDst(inst.dst[0], result);

    // SCC = (S1.u + SCC > S0.u) as a 33-bit compare.
    const IR::U1 underflow{ir.IGreaterThan(src1, src0, false)};
    const IR::U1 borrow_underflow{ir.IGreaterThan(borrow, difference, false)};
    ir.SetScc(ir.LogicalOr(underflow, borrow_underflow));
}

void Translator::S_ADD_I32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 result{ir.IAdd(src0, src1)};
    SetDst(inst.dst[0], result);

    // SCC = ((S0.u[31] == S1.u[31]) && (S0.u[31] != Result.u[31]));
    const IR::U32 shift{ir.Imm32(31)};
    const IR::U32 sign0{ir.ShiftRightLogical(src0, shift)};
    const IR::U32 sign1{ir.ShiftRightLogical(src1, shift)};
    const IR::U32 signr{ir.ShiftRightLogical(result, shift)};
    ir.SetScc(ir.LogicalAnd(ir.IEqual(sign0, sign1), ir.INotEqual(sign0, signr)));
}

void Translator::S_SUB_I32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 result{ir.ISub(src0, src1)};
    SetDst(inst.dst[0], result);

    // SCC = ((S0.u[31] != S1.u[31]) && (S0.u[31] != tmp.u[31]));
    const IR::U32 shift{ir.Imm32(31)};
    const IR::U32 sign0{ir.ShiftRightLogical(src0, shift)};
    const IR::U32 sign1{ir.ShiftRightLogical(src1, shift)};
    const IR::U32 signr{ir.ShiftRightLogical(result, shift)};
    ir.SetScc(ir.LogicalAnd(ir.INotEqual(sign0, sign1), ir.INotEqual(sign0, signr)));
}

void Translator::S_ADDC_U32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 carry{ir.Select(ir.GetScc(), ir.Imm32(1U), ir.Imm32(0U))};
    const IR::U32 result1{ir.IAdd(src0, src1)};
    const IR::U32 result2{ir.IAdd(result1, carry)};
    const IR::U1 carry_out1{ir.ILessThan(result1, src0, false)};
    const IR::U1 carry_out2{ir.ILessThan(result2, result1, false)};
    SetDst(inst.dst[0], result2);
    ir.SetScc(ir.LogicalOr(carry_out1, carry_out2));
}

void Translator::S_MIN_U32(bool is_signed, const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 result = ir.IMin(src0, src1, is_signed);
    SetDst(inst.dst[0], result);
    ir.SetScc(ir.IEqual(result, src0));
}

void Translator::S_MAX_U32(bool is_signed, const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 result = ir.IMax(src0, src1, is_signed);
    SetDst(inst.dst[0], result);
    ir.SetScc(ir.IEqual(result, src0));
}

void Translator::S_CSELECT_B32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    SetDst(inst.dst[0], IR::U32{ir.Select(ir.GetScc(), src0, src1)});
}

void Translator::S_CSELECT_B64(const GcnInst& inst) {
    const IR::U64 src0{GetSrc64(inst.src[0])};
    const IR::U64 src1{GetSrc64(inst.src[1])};
    const IR::U64 result{ir.Select(ir.GetScc(), src0, src1)};
    SetDst64(inst.dst[0], result);
}

void Translator::S_AND_B32(NegateMode negate, const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    IR::U32 src1{GetSrc(inst.src[1])};
    if (negate == NegateMode::Src1) {
        src1 = ir.BitwiseNot(src1);
    }
    IR::U32 result{ir.BitwiseAnd(src0, src1)};
    if (negate == NegateMode::Result) {
        result = ir.BitwiseNot(result);
    }
    SetDst(inst.dst[0], result);
    ir.SetScc(ir.INotEqual(result, ir.Imm32(0)));
}

void Translator::S_AND_B64(NegateMode negate, const GcnInst& inst) {
    const IR::U64 src0{GetSrc64(inst.src[0])};
    IR::U64 src1{GetSrc64(inst.src[1])};
    if (negate == NegateMode::Src1) {
        src1 = ir.BitwiseNot(src1);
    }
    IR::U64 result = ir.BitwiseAnd(src0, src1);
    if (negate == NegateMode::Result) {
        result = ir.BitwiseNot(result);
    }
    ir.SetScc(ir.INotEqual(result, ir.Imm64(u64(0))));
    SetDst64(inst.dst[0], result);
}

void Translator::S_OR_B32(NegateMode negate, bool is_xor, const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    IR::U32 src1{GetSrc(inst.src[1])};
    if (negate == NegateMode::Src1)
        src1 = ir.BitwiseNot(src1);
    IR::U32 result = is_xor ? ir.BitwiseXor(src0, src1) : ir.BitwiseOr(src0, src1);
    if (negate == NegateMode::Result)
        result = ir.BitwiseNot(result);
    SetDst(inst.dst[0], result);
    ir.SetScc(ir.INotEqual(result, ir.Imm32(0)));
}

void Translator::S_OR_B64(NegateMode negate, bool is_xor, const GcnInst& inst) {
    const IR::U64 src0{GetSrc64(inst.src[0])};
    IR::U64 src1{GetSrc64(inst.src[1])};
    if (negate == NegateMode::Src1) {
        src1 = ir.BitwiseNot(src1);
    }
    IR::U64 result = is_xor ? ir.BitwiseXor(src0, src1) : ir.BitwiseOr(src0, src1);
    if (negate == NegateMode::Result) {
        result = ir.BitwiseNot(result);
    }
    ir.SetScc(ir.INotEqual(result, ir.Imm64(u64(0))));
    SetDst64(inst.dst[0], result);
}

void Translator::S_XOR_B32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 result{ir.BitwiseXor(src0, src1)};
    SetDst(inst.dst[0], result);
    ir.SetScc(ir.INotEqual(result, ir.Imm32(0)));
}

void Translator::S_NOT_B32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 result{ir.BitwiseNot(src0)};
    SetDst(inst.dst[0], result);
    ir.SetScc(ir.INotEqual(result, ir.Imm32(0)));
}

void Translator::S_LSHL_B32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 result = ir.ShiftLeftLogical(src0, ir.BitwiseAnd(src1, ir.Imm32(0x1F)));
    SetDst(inst.dst[0], result);
    ir.SetScc(ir.INotEqual(result, ir.Imm32(0)));
}

void Translator::S_LSHL_B64(const GcnInst& inst) {
    const IR::U64 src0{GetSrc64(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U64 result = ir.ShiftLeftLogical(src0, ir.BitwiseAnd(src1, ir.Imm32(0x3F)));
    SetDst64(inst.dst[0], result);
    ir.SetScc(ir.INotEqual(result, ir.Imm64(u64(0))));
}

void Translator::S_LSHR_B32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 shift_amt = ir.BitwiseAnd(src1, ir.Imm32(0x1F));
    const IR::U32 result = ir.ShiftRightLogical(src0, shift_amt);
    SetDst(inst.dst[0], result);
    ir.SetScc(ir.INotEqual(result, ir.Imm32(0)));
}

void Translator::S_LSHR_B64(const GcnInst& inst) {
    const IR::U64 src0{GetSrc64(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U64 result = ir.ShiftRightLogical(src0, ir.BitwiseAnd(src1, ir.Imm32(0x3F)));
    SetDst64(inst.dst[0], result);
    ir.SetScc(ir.INotEqual(result, ir.Imm64(u64(0))));
}

void Translator::S_ASHR_I32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 result{ir.ShiftRightArithmetic(src0, ir.BitwiseAnd(src1, ir.Imm32(0x1F)))};
    SetDst(inst.dst[0], result);
    ir.SetScc(ir.INotEqual(result, ir.Imm32(0)));
}

void Translator::S_ASHR_I64(const GcnInst& inst) {
    const IR::U64 src0{GetSrc64(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U64 result{ir.ShiftRightArithmetic(src0, ir.BitwiseAnd(src1, ir.Imm32(0x3F)))};
    SetDst64(inst.dst[0], result);
    ir.SetScc(ir.INotEqual(result, ir.Imm64(u64(0))));
}

void Translator::S_BFM_B32(const GcnInst& inst) {
    const IR::U32 src0{ir.BitwiseAnd(GetSrc(inst.src[0]), ir.Imm32(0x1F))};
    const IR::U32 src1{ir.BitwiseAnd(GetSrc(inst.src[1]), ir.Imm32(0x1F))};
    const IR::U32 mask{ir.ISub(ir.ShiftLeftLogical(ir.Imm32(1u), src0), ir.Imm32(1))};
    SetDst(inst.dst[0], ir.ShiftLeftLogical(mask, src1));
}

void Translator::S_BFM_B64(const GcnInst& inst) {
    const IR::U32 src0{ir.BitwiseAnd(GetSrc(inst.src[0]), ir.Imm32(0x3F))};
    const IR::U32 src1{ir.BitwiseAnd(GetSrc(inst.src[1]), ir.Imm32(0x3F))};
    const IR::U64 mask{ir.ISub(ir.ShiftLeftLogical(ir.Imm64(u64(1)), src0), ir.Imm64(u64(1)))};
    SetDst64(inst.dst[0], ir.ShiftLeftLogical(mask, src1));
}

void Translator::S_MUL_I32(const GcnInst& inst) {
    SetDst(inst.dst[0], ir.IMul(GetSrc(inst.src[0]), GetSrc(inst.src[1])));
}

// The ISA summary tables mark S_BFE_U32/I32/U64/I64 as not setting SCC. Detailed opcode
// descriptions contradict this; preserve SCC per the summary until hardware validation.
void Translator::S_BFE(const GcnInst& inst, bool is_signed) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 offset{ir.BitwiseAnd(src1, ir.Imm32(0x1F))};
    const IR::U32 width{ir.BitFieldExtract(src1, ir.Imm32(16), ir.Imm32(7))};
    const IR::U32 remaining{ir.ISub(ir.Imm32(32), offset)};
    const IR::U32 safe_width{ir.UMin(width, remaining)};
    SetDst(inst.dst[0], ir.BitFieldExtract(src0, offset, safe_width, is_signed));
}

void Translator::S_BFE64(const GcnInst& inst, bool is_signed) {
    const IR::U64 src0{GetSrc64(inst.src[0])};
    const IR::U32 control{GetSrc(inst.src[1])};
    const IR::U32 offset{ir.BitwiseAnd(control, ir.Imm32(0x3F))};
    const IR::U32 width{ir.BitFieldExtract(control, ir.Imm32(16), ir.Imm32(7))};
    const IR::U64 shifted{ir.ShiftRightLogical(src0, offset)};
    const IR::U32 mask_shift{ir.BitwiseAnd(width, ir.Imm32(0x3F))};
    const IR::U64 low_mask{
        ir.ISub(ir.ShiftLeftLogical(ir.Imm64(u64(1)), mask_shift), ir.Imm64(u64(1)))};
    const IR::U1 empty_width{ir.IEqual(width, ir.Imm32(0))};
    const IR::U1 full_width{ir.IGreaterThanEqual(width, ir.Imm32(64), false)};
    const IR::U64 mask{ir.Select(empty_width, ir.Imm64(u64(0)),
                                 IR::U64{ir.Select(full_width, ir.Imm64(~u64(0)), low_mask)})};
    const IR::U64 extracted{ir.BitwiseAnd(shifted, mask)};
    const IR::U32 effective_width{ir.UMax(ir.UMin(width, ir.Imm32(64)), ir.Imm32(1))};
    const IR::U32 sign_shift{ir.ISub(ir.Imm32(64), effective_width)};
    const IR::U64 sign_extended{
        ir.ShiftRightArithmetic(ir.ShiftLeftLogical(extracted, sign_shift), sign_shift)};
    IR::U64 result{extracted};
    if (is_signed) {
        const IR::U1 crosses_msb{ir.IGreaterThanEqual(ir.IAdd(offset, width), ir.Imm32(64), false)};
        const IR::U64 signed_result{
            ir.Select(crosses_msb, ir.ShiftRightArithmetic(src0, offset), sign_extended)};
        result = IR::U64{ir.Select(empty_width, ir.Imm64(u64(0)), signed_result)};
    }
    SetDst64(inst.dst[0], result);
}

void Translator::S_ABSDIFF_I32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 result{ir.IAbs(ir.ISub(src0, src1))};
    SetDst(inst.dst[0], result);
    ir.SetScc(ir.INotEqual(result, ir.Imm32(0)));
}

// SOPK

void Translator::S_MOVK(const GcnInst& inst, bool is_conditional) {
    const s16 simm16 = inst.control.sopk.simm;
    // do the sign extension
    const s32 simm32 = static_cast<s32>(simm16);
    IR::U32 val = ir.Imm32(simm32);
    if (is_conditional) {
        // if !SCC its a NOP
        val = IR::U32{ir.Select(ir.GetScc(), val, GetSrc(inst.dst[0]))};
    }
    SetDst(inst.dst[0], val);
}

void Translator::S_CMPK(ConditionOp cond, bool is_signed, const GcnInst& inst) {
    const s32 simm16 = inst.control.sopk.simm;
    const IR::U32 lhs = GetSrc(inst.dst[0]);
    const IR::U32 rhs = ir.Imm32(simm16);
    const IR::U1 result = [&] {
        switch (cond) {
        case ConditionOp::EQ:
            return ir.IEqual(lhs, rhs);
        case ConditionOp::LG:
            return ir.INotEqual(lhs, rhs);
        case ConditionOp::GT:
            return ir.IGreaterThan(lhs, rhs, is_signed);
        case ConditionOp::GE:
            return ir.IGreaterThanEqual(lhs, rhs, is_signed);
        case ConditionOp::LT:
            return ir.ILessThan(lhs, rhs, is_signed);
        case ConditionOp::LE:
            return ir.ILessThanEqual(lhs, rhs, is_signed);
        default:
            UNREACHABLE();
        }
    }();
    ir.SetScc(result);
}

void Translator::S_ADDK_I32(const GcnInst& inst) {
    const s32 simm16 = inst.control.sopk.simm;
    const IR::U32 src0{GetSrc(inst.dst[0])};
    const IR::U32 src1{ir.Imm32(simm16)};
    const IR::U32 result{ir.IAdd(src0, src1)};
    SetDst(inst.dst[0], result);

    const IR::U32 shift{ir.Imm32(31)};
    const IR::U32 sign0{ir.ShiftRightLogical(src0, shift)};
    const IR::U32 sign1{ir.ShiftRightLogical(src1, shift)};
    const IR::U32 signr{ir.ShiftRightLogical(result, shift)};
    ir.SetScc(ir.LogicalAnd(ir.IEqual(sign0, sign1), ir.INotEqual(sign0, signr)));
}

void Translator::S_MULK_I32(const GcnInst& inst) {
    const s32 simm16 = inst.control.sopk.simm;
    SetDst(inst.dst[0], ir.IMul(GetSrc(inst.dst[0]), ir.Imm32(simm16)));
}

// SOP1

void Translator::S_MOV(const GcnInst& inst, bool conditional) {
    const IR::U32 value = GetSrc(inst.src[0]);
    SetDst(inst.dst[0],
           conditional ? IR::U32{ir.Select(ir.GetScc(), value, GetSrc(inst.dst[0]))} : value);
}

void Translator::S_MOVRELS_B32(const GcnInst& inst) {
    const u32 max_sgprs = static_cast<u32>(IR::NumScalarRegs);
    const u32 allocated_sgprs = runtime_info.props.num_allocated_sgprs;
    const u32 num_sgprs = allocated_sgprs < max_sgprs ? allocated_sgprs : max_sgprs;
    if (inst.dst[0].code >= num_sgprs) {
        return;
    }
    InstOperand source = inst.src[0];
    if (source.field == OperandField::ScalarGPR && source.code >= num_sgprs) {
        source.code = static_cast<u32>(IR::ScalarReg::S0);
    }
    const IR::U32 base = GetSrc(source);
    const IR::U32 index = ir.IAdd(base, ir.GetM0());
    const IR::U1 no_overflow = ir.IGreaterThanEqual(index, base, false);
    // GCN3 returns SGPR0 when the relative source index is outside the wave's allocation.
    IR::U32 result = ir.GetScalarReg<IR::U32>(IR::ScalarReg::S0);
    for (u32 sgpr = 1; sgpr < num_sgprs; ++sgpr) {
        const IR::U1 is_index = ir.LogicalAnd(no_overflow, ir.IEqual(index, ir.Imm32(sgpr)));
        const auto reg = static_cast<IR::ScalarReg>(sgpr);
        result = IR::U32{ir.Select(is_index, ir.GetScalarReg<IR::U32>(reg), result)};
    }
    SetDst(inst.dst[0], result);
}

void Translator::S_MOVRELS_B64(const GcnInst& inst) {
    const u32 max_sgprs = static_cast<u32>(IR::NumScalarRegs);
    const u32 allocated_sgprs = runtime_info.props.num_allocated_sgprs;
    const u32 num_sgprs = allocated_sgprs < max_sgprs ? allocated_sgprs : max_sgprs;
    if ((inst.dst[0].code & 1) != 0 || inst.dst[0].code + 1 >= num_sgprs) {
        return;
    }
    InstOperand source = inst.src[0];
    if (source.field == OperandField::ScalarGPR && source.code >= num_sgprs) {
        source.code = static_cast<u32>(IR::ScalarReg::S0);
    }
    const IR::U32 base = GetSrc(source);
    const IR::U32 m0 = ir.GetM0();
    const IR::U32 index = ir.IAdd(base, m0);
    // GCN3 requires the source value and M0 to be even independently.
    const IR::U1 base_even = ir.IEqual(ir.BitwiseAnd(base, ir.Imm32(1)), ir.Imm32(0));
    const IR::U1 m0_even = ir.IEqual(ir.BitwiseAnd(m0, ir.Imm32(1)), ir.Imm32(0));
    const IR::U1 valid_index =
        ir.LogicalAnd(ir.IGreaterThanEqual(index, base, false), ir.LogicalAnd(base_even, m0_even));
    const IR::U32 sgpr0 = ir.GetScalarReg<IR::U32>(IR::ScalarReg::S0);
    IR::U32 lo = sgpr0;
    IR::U32 hi = sgpr0;
    // GCN3 requires an even source index. Each out-of-range source dword reads SGPR0.
    for (u32 sgpr = 0; sgpr < num_sgprs; sgpr += 2) {
        const IR::U1 is_index = ir.LogicalAnd(valid_index, ir.IEqual(index, ir.Imm32(sgpr)));
        const auto reg = static_cast<IR::ScalarReg>(sgpr);
        const IR::U32 pair_hi = sgpr + 1 < num_sgprs
                                    ? ir.GetScalarReg<IR::U32>(static_cast<IR::ScalarReg>(sgpr + 1))
                                    : sgpr0;
        lo = IR::U32{ir.Select(is_index, ir.GetScalarReg<IR::U32>(reg), lo)};
        hi = IR::U32{ir.Select(is_index, pair_hi, hi)};
    }
    SetDst64(inst.dst[0], ir.PackUint2x32(ir.CompositeConstruct(lo, hi)));
}

void Translator::S_MOVRELD_B32(const GcnInst& inst) {
    const u32 max_sgprs = static_cast<u32>(IR::NumScalarRegs);
    const u32 allocated_sgprs = runtime_info.props.num_allocated_sgprs;
    const u32 num_sgprs = allocated_sgprs < max_sgprs ? allocated_sgprs : max_sgprs;
    InstOperand source = inst.src[0];
    if (source.field == OperandField::ScalarGPR && source.code >= num_sgprs) {
        source.code = static_cast<u32>(IR::ScalarReg::S0);
    }
    const IR::U32 value = GetSrc(source);
    const IR::U32 base = ir.Imm32(inst.dst[0].code);
    const IR::U32 index = ir.IAdd(base, ir.GetM0());
    const IR::U1 no_overflow = ir.IGreaterThanEqual(index, base, false);
    // A destination outside the wave's SGPR allocation is ignored.
    for (u32 sgpr = 0; sgpr < num_sgprs; ++sgpr) {
        const IR::U1 is_index = ir.LogicalAnd(no_overflow, ir.IEqual(index, ir.Imm32(sgpr)));
        const auto reg = static_cast<IR::ScalarReg>(sgpr);
        ir.SetScalarReg(reg, IR::U32{ir.Select(is_index, value, ir.GetScalarReg<IR::U32>(reg))});
    }
}

void Translator::S_MOVRELD_B64(const GcnInst& inst) {
    const u32 max_sgprs = static_cast<u32>(IR::NumScalarRegs);
    const u32 allocated_sgprs = runtime_info.props.num_allocated_sgprs;
    const u32 num_sgprs = allocated_sgprs < max_sgprs ? allocated_sgprs : max_sgprs;
    const InstOperand& source = inst.src[0];
    IR::U64 value{};
    if (source.field == OperandField::ScalarGPR) {
        if ((source.code & 1) != 0) {
            return;
        }
        const IR::U32 sgpr0 = ir.GetScalarReg<IR::U32>(IR::ScalarReg::S0);
        const IR::U32 lo = source.code < num_sgprs
                               ? ir.GetScalarReg<IR::U32>(static_cast<IR::ScalarReg>(source.code))
                               : sgpr0;
        const IR::U32 hi =
            source.code + 1 < num_sgprs
                ? ir.GetScalarReg<IR::U32>(static_cast<IR::ScalarReg>(source.code + 1))
                : sgpr0;
        IR::U32 adjusted_hi = hi;
        if (source.input_modifier.abs) {
            adjusted_hi = ir.BitwiseAnd(adjusted_hi, ir.Imm32(0x7FFFFFFFu));
        }
        if (source.input_modifier.neg) {
            adjusted_hi = ir.BitwiseXor(adjusted_hi, ir.Imm32(0x80000000u));
        }
        value = ir.PackUint2x32(ir.CompositeConstruct(lo, adjusted_hi));
    } else {
        value = GetSrc64(source);
    }
    const IR::U32 base = ir.Imm32(inst.dst[0].code);
    const IR::U32 m0 = ir.GetM0();
    const IR::U32 index = ir.IAdd(base, m0);
    // GCN3 requires D and M0 to be even independently.
    const IR::U1 base_even = ir.IEqual(ir.BitwiseAnd(base, ir.Imm32(1)), ir.Imm32(0));
    const IR::U1 m0_even = ir.IEqual(ir.BitwiseAnd(m0, ir.Imm32(1)), ir.Imm32(0));
    const IR::U1 valid_index =
        ir.LogicalAnd(ir.IGreaterThanEqual(index, base, false), ir.LogicalAnd(base_even, m0_even));
    const auto words = ir.UnpackUint2x32(value);
    const IR::U32 lo{ir.CompositeExtract(words, 0)};
    const IR::U32 hi{ir.CompositeExtract(words, 1)};
    // An invalid or out-of-range destination pair is not written.
    for (u32 sgpr = 0; sgpr + 1 < num_sgprs; sgpr += 2) {
        const IR::U1 is_index = ir.LogicalAnd(valid_index, ir.IEqual(index, ir.Imm32(sgpr)));
        const auto lo_reg = static_cast<IR::ScalarReg>(sgpr);
        const auto hi_reg = static_cast<IR::ScalarReg>(sgpr + 1);
        ir.SetScalarReg(lo_reg, IR::U32{ir.Select(is_index, lo, ir.GetScalarReg<IR::U32>(lo_reg))});
        ir.SetScalarReg(hi_reg, IR::U32{ir.Select(is_index, hi, ir.GetScalarReg<IR::U32>(hi_reg))});
    }
}

void Translator::S_SEXT_I32(const GcnInst& inst, u32 width) {
    SetDst(inst.dst[0],
           ir.BitFieldExtract(GetSrc(inst.src[0]), ir.Imm32(0), ir.Imm32(width), true));
}

void Translator::S_MOV_B64(const GcnInst& inst, bool conditional) {
    if (inst.dst[0].field == OperandField::ScalarGPR) {
        // A 64-bit SGPR destination must fit entirely in the shader allocation.
        if (!IsScalarGprAllocated(inst.dst[0].code) ||
            !IsScalarGprAllocated(inst.dst[0].code + 1)) {
            return;
        }
    }
    const IR::U64 value{GetSrc64(inst.src[0])};
    if (conditional) {
        const IR::U64 old_value{GetSrc64(inst.dst[0])};
        SetDst64(inst.dst[0], IR::U64{ir.Select(ir.GetScc(), value, old_value)});
        return;
    }
    SetDst64(inst.dst[0], value);
}

void Translator::S_WQM_B32(const GcnInst& inst) {
    IR::U32 result{GetSrc(inst.src[0])};
    result = ir.BitwiseOr(
        result, ir.ShiftRightLogical(ir.BitwiseAnd(result, ir.Imm32(0xAAAAAAAA)), ir.Imm32(1)));
    result = ir.BitwiseOr(
        result, ir.ShiftLeftLogical(ir.BitwiseAnd(result, ir.Imm32(0x55555555)), ir.Imm32(1)));
    result = ir.BitwiseOr(
        result, ir.ShiftRightLogical(ir.BitwiseAnd(result, ir.Imm32(0xCCCCCCCC)), ir.Imm32(2)));
    result = ir.BitwiseOr(
        result, ir.ShiftLeftLogical(ir.BitwiseAnd(result, ir.Imm32(0x33333333)), ir.Imm32(2)));
    SetDst(inst.dst[0], result);
    ir.SetScc(ir.INotEqual(result, ir.Imm32(0)));
}

void Translator::S_WQM_B64(const GcnInst& inst) {
    IR::U64 result{GetSrc64(inst.src[0])};
    result = ir.BitwiseOr(
        result,
        ir.ShiftRightLogical(ir.BitwiseAnd(result, ir.Imm64(0xAAAAAAAAAAAAAAAAULL)), ir.Imm32(1)));
    result = ir.BitwiseOr(
        result,
        ir.ShiftLeftLogical(ir.BitwiseAnd(result, ir.Imm64(0x5555555555555555ULL)), ir.Imm32(1)));
    result = ir.BitwiseOr(
        result,
        ir.ShiftRightLogical(ir.BitwiseAnd(result, ir.Imm64(0xCCCCCCCCCCCCCCCCULL)), ir.Imm32(2)));
    result = ir.BitwiseOr(
        result,
        ir.ShiftLeftLogical(ir.BitwiseAnd(result, ir.Imm64(0x3333333333333333ULL)), ir.Imm32(2)));
    SetDst64(inst.dst[0], result);
    ir.SetScc(ir.INotEqual(result, ir.Imm64(u64(0))));
}

void Translator::S_QUADMASK_B32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    IR::U32 result{ir.Imm32(0)};
    for (u32 quad = 0; quad < 8; ++quad) {
        const IR::U32 group{
            ir.BitwiseAnd(ir.ShiftRightLogical(src0, ir.Imm32(quad * 4)), ir.Imm32(0xF))};
        const IR::U32 bit{
            ir.Select(ir.INotEqual(group, ir.Imm32(0)), ir.Imm32(1U << quad), ir.Imm32(0))};
        result = ir.BitwiseOr(result, bit);
    }
    SetDst(inst.dst[0], result);
    ir.SetScc(ir.INotEqual(result, ir.Imm32(0)));
}

void Translator::S_QUADMASK_B64(const GcnInst& inst) {
    const IR::U64 src0{GetSrc64(inst.src[0])};
    IR::U64 result{ir.Imm64(u64(0))};
    for (u32 quad = 0; quad < 16; ++quad) {
        const IR::U64 group{
            ir.BitwiseAnd(ir.ShiftRightLogical(src0, ir.Imm32(quad * 4)), ir.Imm64(u64(0xF)))};
        const IR::U64 bit{ir.Select(ir.INotEqual(group, ir.Imm64(u64(0))), ir.Imm64(u64(1) << quad),
                                    ir.Imm64(u64(0)))};
        result = ir.BitwiseOr(result, bit);
    }
    SetDst64(inst.dst[0], result);
    ir.SetScc(ir.INotEqual(result, ir.Imm64(u64(0))));
}

void Translator::S_NOT_B64(const GcnInst& inst) {
    const IR::U64 src0{GetSrc64(inst.src[0])};
    const IR::U64 result = ir.BitwiseNot(src0);
    ir.SetScc(ir.INotEqual(result, ir.Imm64(u64(0))));
    SetDst64(inst.dst[0], result);
}

void Translator::S_BREV_B32(const GcnInst& inst) {
    SetDst(inst.dst[0], ir.BitReverse(GetSrc(inst.src[0])));
}
void Translator::S_BREV_B64(const GcnInst& inst) {
    IR::U64 result{GetSrc64(inst.src[0])};
    const auto swap_bits = [&](u32 shift, u64 mask) {
        const IR::U64 right{
            ir.BitwiseAnd(ir.ShiftRightLogical(result, ir.Imm32(shift)), ir.Imm64(mask))};
        const IR::U64 left{
            ir.ShiftLeftLogical(ir.BitwiseAnd(result, ir.Imm64(mask)), ir.Imm32(shift))};
        result = ir.BitwiseOr(left, right);
    };
    swap_bits(1, 0x5555555555555555ULL);
    swap_bits(2, 0x3333333333333333ULL);
    swap_bits(4, 0x0F0F0F0F0F0F0F0FULL);
    swap_bits(8, 0x00FF00FF00FF00FFULL);
    swap_bits(16, 0x0000FFFF0000FFFFULL);
    swap_bits(32, 0x00000000FFFFFFFFULL);
    SetDst64(inst.dst[0], result);
}

void Translator::S_BCNT0_I32_B32(const GcnInst& inst) {
    const IR::U32 result = ir.BitCount(ir.BitwiseNot(GetSrc(inst.src[0])));
    SetDst(inst.dst[0], result);
    ir.SetScc(ir.INotEqual(result, ir.Imm32(0)));
}

void Translator::S_BCNT0_I32_B64(const GcnInst& inst) {
    const IR::U32 result = ir.BitCount(ir.BitwiseNot(GetSrc64(inst.src[0])));
    SetDst(inst.dst[0], result);
    ir.SetScc(ir.INotEqual(result, ir.Imm32(0)));
}

void Translator::S_BCNT1_I32_B32(const GcnInst& inst) {
    const IR::U32 result = ir.BitCount(GetSrc(inst.src[0]));
    SetDst(inst.dst[0], result);
    ir.SetScc(ir.INotEqual(result, ir.Imm32(0)));
}

void Translator::S_BCNT1_I32_B64(const GcnInst& inst) {
    const IR::U32 result = ir.BitCount(GetSrc64(inst.src[0]));
    SetDst(inst.dst[0], result);
    ir.SetScc(ir.INotEqual(result, ir.Imm32(0)));
}

void Translator::S_FF1_I32_B32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 result{ir.FindILsb(src0)};
    SetDst(inst.dst[0], result);
}

void Translator::S_FF1_I32_B64(const GcnInst& inst) {
    const IR::U64 src0{GetSrc64(inst.src[0])};
    // This is a scalar bit scan, not a lane-mask query; keep it independent of subgroup width.
    SetDst(inst.dst[0], ir.FindILsb(src0));
}

void Translator::S_FF0_I32_B32(const GcnInst& inst) {
    const IR::U32 src0{ir.BitwiseNot(GetSrc(inst.src[0]))};
    SetDst(inst.dst[0], ir.FindILsb(src0));
}

void Translator::S_FF0_I32_B64(const GcnInst& inst) {
    const IR::U64 src0{ir.BitwiseNot(GetSrc64(inst.src[0]))};
    SetDst(inst.dst[0], ir.FindILsb(src0));
}

void Translator::S_FLBIT_I32_B32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    // Gcn wants the MSB position counting from the left, but SPIR-V counts from the rightmost (LSB)
    // position
    const IR::U32 msb_pos = ir.FindUMsb(src0);
    const IR::U32 pos_from_left = ir.ISub(ir.Imm32(31), msb_pos);
    // Select 0xFFFFFFFF if src0 was 0
    const IR::U1 cond = ir.INotEqual(src0, ir.Imm32(0));
    SetDst(inst.dst[0], IR::U32{ir.Select(cond, pos_from_left, ir.Imm32(~0U))});
}

void Translator::S_FLBIT_I32_B64(const GcnInst& inst) {
    const IR::U64 src0{GetSrc64(inst.src[0])};
    // Gcn wants the MSB position counting from the left, but SPIR-V counts from the rightmost (LSB)
    // position
    const IR::U32 msb_pos = ir.FindUMsb(src0);
    const IR::U32 pos_from_left = ir.ISub(ir.Imm32(63), msb_pos);
    // Select 0xFFFFFFFF if src0 was 0
    const IR::U1 cond = ir.INotEqual(src0, ir.Imm64(u64(0u)));
    SetDst(inst.dst[0], IR::U32{ir.Select(cond, pos_from_left, ir.Imm32(~0U))});
}

void Translator::S_FLBIT_I32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U1 is_negative{ir.INotEqual(ir.ShiftRightLogical(src0, ir.Imm32(31)), ir.Imm32(0))};
    const IR::U32 normalized{ir.Select(is_negative, ir.BitwiseNot(src0), src0)};
    const IR::U32 leading_sign_bits{ir.ISub(ir.Imm32(31), ir.FindUMsb(normalized))};
    const IR::U1 has_transition{ir.INotEqual(normalized, ir.Imm32(0))};
    SetDst(inst.dst[0], IR::U32{ir.Select(has_transition, leading_sign_bits, ir.Imm32(~0U))});
}

void Translator::S_FLBIT_I32_I64(const GcnInst& inst) {
    const IR::U64 src0{GetSrc64(inst.src[0])};
    const IR::U64 sign{ir.ShiftRightLogical(src0, ir.Imm32(63))};
    const IR::U1 is_negative{ir.INotEqual(sign, ir.Imm64(u64(0)))};
    const IR::U64 normalized{ir.Select(is_negative, ir.BitwiseNot(src0), src0)};
    const IR::U32 leading_sign_bits{ir.ISub(ir.Imm32(63), ir.FindUMsb(normalized))};
    const IR::U1 has_transition{ir.INotEqual(normalized, ir.Imm64(u64(0)))};
    SetDst(inst.dst[0], IR::U32{ir.Select(has_transition, leading_sign_bits, ir.Imm32(~0U))});
}

void Translator::S_BITSET_B32(const GcnInst& inst, u32 bit_value) {
    const IR::U32 old_value{GetSrc(inst.dst[0])};
    const IR::U32 offset{ir.BitFieldExtract(GetSrc(inst.src[0]), ir.Imm32(0U), ir.Imm32(5U))};
    const IR::U32 result{ir.BitFieldInsert(old_value, ir.Imm32(bit_value), offset, ir.Imm32(1U))};
    SetDst(inst.dst[0], result);
}

void Translator::S_BITSET_B64(const GcnInst& inst, u32 bit_value) {
    const IR::U64 old_value{GetSrc64(inst.dst[0])};
    const IR::U32 offset{ir.BitwiseAnd(GetSrc(inst.src[0]), ir.Imm32(0x3F))};
    const IR::U64 result{
        ir.BitFieldInsert(old_value, ir.Imm64(u64(bit_value)), offset, ir.Imm32(1U))};
    SetDst64(inst.dst[0], result);
}

void Translator::S_SAVEEXEC_B64(const GcnInst& inst) {
    const IR::U64 exec{ir.Ballot(ir.GetExec())};
    const IR::U64 src{GetSrc64(inst.src[0])};
    SetDst64(inst.dst[0], exec);

    const IR::U64 result{[&] {
        switch (inst.opcode) {
        case Opcode::S_AND_SAVEEXEC_B64:
            return ir.BitwiseAnd(src, exec);
        case Opcode::S_OR_SAVEEXEC_B64:
            return ir.BitwiseOr(src, exec);
        case Opcode::S_XOR_SAVEEXEC_B64:
            return ir.BitwiseXor(src, exec);
        case Opcode::S_ANDN2_SAVEEXEC_B64:
            return ir.BitwiseAnd(src, ir.BitwiseNot(exec));
        case Opcode::S_ORN2_SAVEEXEC_B64:
            return ir.BitwiseOr(src, ir.BitwiseNot(exec));
        case Opcode::S_NAND_SAVEEXEC_B64:
            return ir.BitwiseNot(ir.BitwiseAnd(src, exec));
        case Opcode::S_NOR_SAVEEXEC_B64:
            return ir.BitwiseNot(ir.BitwiseOr(src, exec));
        case Opcode::S_XNOR_SAVEEXEC_B64:
            return ir.BitwiseNot(ir.BitwiseXor(src, exec));
        default:
            UNREACHABLE();
        }
    }()};
    const IR::U1 exec_lane = ir.InverseBallot(result);
    ir.SetExec(exec_lane);
    ir.SetScc(ir.INotEqual(result, ir.Imm64(u64(0))));
}

void Translator::S_ABS_I32(const GcnInst& inst) {
    const auto result = ir.IAbs(GetSrc(inst.src[0]));
    SetDst(inst.dst[0], result);
    ir.SetScc(ir.INotEqual(result, ir.Imm32(0)));
}

// SOPC

void Translator::S_CMP(ConditionOp cond, bool is_signed, const GcnInst& inst) {
    const IR::U32 lhs = GetSrc(inst.src[0]);
    const IR::U32 rhs = GetSrc(inst.src[1]);
    const IR::U1 result = [&] {
        switch (cond) {
        case ConditionOp::EQ:
            return ir.IEqual(lhs, rhs);
        case ConditionOp::LG:
            return ir.INotEqual(lhs, rhs);
        case ConditionOp::GT:
            return ir.IGreaterThan(lhs, rhs, is_signed);
        case ConditionOp::GE:
            return ir.IGreaterThanEqual(lhs, rhs, is_signed);
        case ConditionOp::LT:
            return ir.ILessThan(lhs, rhs, is_signed);
        case ConditionOp::LE:
            return ir.ILessThanEqual(lhs, rhs, is_signed);
        default:
            UNREACHABLE();
        }
    }();
    ir.SetScc(result);
}

void Translator::S_BITCMP(bool compare_mode, u32 bits, const GcnInst& inst) {
    const IR::U1 result = [&] {
        const IR::U32 src1 = GetSrc(inst.src[1]);
        const IR::U32 mask = ir.Imm32(bits == 64 ? 0x3f : 0x1f);
        const IR::U32 bitpos{ir.BitwiseAnd(src1, mask)};
        const IR::U32 bittest = [&]() -> IR::U32 {
            if (bits == 64) {
                const IR::U64 src0 = GetSrc64(inst.src[0]);
                const IR::U64 bit{
                    ir.BitwiseAnd(ir.ShiftRightLogical(src0, bitpos), ir.Imm64(u64(1)))};
                return ir.UConvert(32, bit);
            }
            if (bits != 32) {
                UNREACHABLE();
            }
            const IR::U32 src0 = GetSrc(inst.src[0]);
            return ir.BitwiseAnd(ir.ShiftRightLogical(src0, bitpos), ir.Imm32(1));
        }();

        if (!compare_mode) {
            return ir.IEqual(bittest, ir.Imm32(0));
        } else {
            return ir.IEqual(bittest, ir.Imm32(1));
        }
    }();
    ir.SetScc(result);
}

} // namespace Shader::Gcn
