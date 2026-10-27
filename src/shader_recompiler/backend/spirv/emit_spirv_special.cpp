// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/debug_print.h"
#include "shader_recompiler/ir/microinstruction.h"

namespace Shader::Backend::SPIRV {

void EmitPrologue(EmitContext& ctx) {
    if (ctx.hw_stage == HwStage::Fragment) {
        ctx.DefineAmdPerVertexAttribs();
    }
    if (ctx.info.loads.Get(IR::Attribute::WorkgroupIndex)) {
        ctx.DefineWorkgroupIndex();
    }
    ctx.DefineBufferProperties();
}

void ConvertDepthMode(EmitContext& ctx) {
    const Id type{ctx.F32[1]};
    const Id position{ctx.OpLoad(ctx.F32[4], ctx.output_position)};
    const Id z{ctx.OpCompositeExtract(type, position, 2u)};
    const Id w{ctx.OpCompositeExtract(type, position, 3u)};
    const Id screen_depth{ctx.OpFMul(type, ctx.OpFAdd(type, z, w), ctx.Constant(type, 0.5f))};
    const Id vector{ctx.OpCompositeInsert(ctx.F32[4], screen_depth, position, 2u)};
    ctx.OpStore(ctx.output_position, vector);
}

void ConvertPositionToClipSpace(EmitContext& ctx) {
    ASSERT_MSG(!ctx.info.stores.GetAny(IR::Attribute::ViewportIndex),
               "Multi-viewport with shader clip space conversion not yet implemented.");

    const Id type{ctx.F32[1]};
    Id position{ctx.OpLoad(ctx.F32[4], ctx.output_position)};
    const Id x{ctx.OpCompositeExtract(type, position, 0u)};
    const Id y{ctx.OpCompositeExtract(type, position, 1u)};
    const Id z{ctx.OpCompositeExtract(type, position, 2u)};
    const Id w{ctx.OpCompositeExtract(type, position, 3u)};
    const Id xoffset_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                           ctx.push_data_block,
                                           ctx.ConstU32(PushData::XOffsetIndex))};
    const Id xoffset{ctx.OpLoad(type, xoffset_ptr)};
    const Id yoffset_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                           ctx.push_data_block,
                                           ctx.ConstU32(PushData::YOffsetIndex))};
    const Id yoffset{ctx.OpLoad(type, yoffset_ptr)};
    const Id xscale_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                          ctx.push_data_block,
                                          ctx.ConstU32(PushData::XScaleIndex))};
    const Id xscale{ctx.OpLoad(type, xscale_ptr)};
    const Id yscale_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                          ctx.push_data_block,
                                          ctx.ConstU32(PushData::YScaleIndex))};
    const Id yscale{ctx.OpLoad(type, yscale_ptr)};
    const Id vport_w =
        ctx.Constant(type, float(std::min<u32>(ctx.profile.max_viewport_width / 2, 8_KB)));
    const Id wnd_x = ctx.OpFAdd(type, ctx.OpFMul(type, x, xscale), xoffset);
    const Id ndc_x = ctx.OpFSub(type, ctx.OpFDiv(type, wnd_x, vport_w), ctx.Constant(type, 1.f));
    const Id vport_h =
        ctx.Constant(type, float(std::min<u32>(ctx.profile.max_viewport_height / 2, 8_KB)));
    const Id wnd_y = ctx.OpFAdd(type, ctx.OpFMul(type, y, yscale), yoffset);
    const Id ndc_y = ctx.OpFSub(type, ctx.OpFDiv(type, wnd_y, vport_h), ctx.Constant(type, 1.f));
    const Id vector{ctx.OpCompositeConstruct(ctx.F32[4], std::array<Id, 4>({ndc_x, ndc_y, z, w}))};
    ctx.OpStore(ctx.output_position, vector);
}

void EmitEpilogue(EmitContext& ctx) {
    if (ctx.hw_stage == HwStage::Vertex &&
        ctx.runtime_info.hw.vs.emulate_depth_negative_one_to_one) {
        ConvertDepthMode(ctx);
    }
    if (ctx.hw_stage == HwStage::Vertex && ctx.runtime_info.hw.vs.clip_disable) {
        ConvertPositionToClipSpace(ctx);
    }
}

void EmitDiscard(EmitContext& ctx) {
    ctx.OpDemoteToHelperInvocationEXT();
}

void EmitDiscardCond(EmitContext& ctx, Id condition) {
    const Id kill_label{ctx.OpLabel()};
    const Id merge_label{ctx.OpLabel()};
    ctx.OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(condition, kill_label, merge_label);
    ctx.AddLabel(kill_label);
    ctx.OpDemoteToHelperInvocationEXT();
    ctx.OpBranch(merge_label);
    ctx.AddLabel(merge_label);
}

static void EmitStreamOutCapture(EmitContext& ctx) {
    const auto& count = ctx.buffers[ctx.strmout_counter].Alias(EmitContext::PointerType::U32);
    const Id count_ptr =
        ctx.OpAccessChain(count.pointer_type, count.id, ctx.u32_zero_value, ctx.u32_zero_value);
    ctx.OpAtomicIAdd(ctx.U32[1], count_ptr, ctx.ConstU32(std::to_underlying(spv::Scope::Device)),
                     ctx.ConstU32(std::to_underlying(spv::MemorySemanticsMask::MaskNone)),
                     ctx.u32_one_value);
    const auto& gs = ctx.runtime_info.hw.gs;
    const Id position{ctx.OpLoad(ctx.F32[4], ctx.output_position)};
    std::array<Id, 4> positions;
    for (u32 comp = 0; comp < 4; ++comp) {
        positions[comp] =
            ctx.OpBitcast(ctx.U32[1], ctx.OpCompositeExtract(ctx.F32[1], position, comp));
    }
    const auto component_ptr = ctx.TypePointer(spv::StorageClass::Output, ctx.F32[1]);
    const u32 window = std::min(gs.out_vertex_data_size, 4u + ctx.info.gs_copy_data.num_attrs * 4u);
    u32 mask = gs.strmout_buffer_mask;
    for (u32 slot = 0; slot < ctx.strmout_buffers.size(); ++slot) {
        const u32 buffer_index = std::countr_zero(mask);
        mask &= mask - 1;
        const u32 stride = gs.strmout_strides[buffer_index];
        const auto& buffer = ctx.buffers[ctx.strmout_buffers[slot]];
        const auto& buffer_alias = buffer.Alias(EmitContext::PointerType::U32);
        Id base_address = ctx.OpLoad(ctx.U32[1], ctx.strmout_vertex);
        base_address = ctx.OpIMul(ctx.U32[1], base_address, ctx.ConstU32(stride));
        if (const Id offset = buffer.Offset(EmitContext::PointerSize::B32);
            Sirit::ValidId(offset)) {
            base_address = ctx.OpIAdd(ctx.U32[1], base_address, offset);
        }
        for (u32 dword = 0; dword < std::min(window, stride); ++dword) {
            Id value = ctx.u32_zero_value;
            if (dword < 4) {
                value = positions[dword];
            } else if (const u32 attr = (dword - 4) / 4; attr < ctx.info.gs_copy_data.num_attrs) {
                const auto& output = ctx.output_params.at(attr);
                const Id ptr =
                    ctx.OpAccessChain(component_ptr, output.id, ctx.ConstU32((dword - 4) % 4));
                value = ctx.OpBitcast(ctx.U32[1], ctx.OpLoad(ctx.F32[1], ptr));
            }
            const Id address = ctx.OpIAdd(ctx.U32[1], base_address, ctx.ConstU32(dword));
            const Id ptr = ctx.OpAccessChain(buffer_alias.pointer_type, buffer_alias.id,
                                             ctx.u32_zero_value, address);
            ctx.OpStore(ptr, value);
        }
    }
    const Id counter = ctx.OpLoad(ctx.U32[1], ctx.strmout_vertex);
    ctx.OpStore(ctx.strmout_vertex, ctx.OpIAdd(ctx.U32[1], counter, ctx.u32_one_value));
}

void EmitEmitVertex(EmitContext& ctx) {
    if (ctx.hw_stage == HwStage::Geometry && !ctx.strmout_buffers.empty()) {
        EmitStreamOutCapture(ctx);
    }
    if (ctx.hw_stage == HwStage::Geometry &&
        ctx.runtime_info.hw.gs.emulate_depth_negative_one_to_one) {
        ConvertDepthMode(ctx);
    }
    ctx.OpEmitVertex();
}

void EmitEmitPrimitive(EmitContext& ctx) {
    ctx.OpEndPrimitive();
}

void EmitEmitVertex(EmitContext& ctx, const IR::Value& stream) {
    UNREACHABLE_MSG("Geometry streams");
}

void EmitEndPrimitive(EmitContext& ctx, const IR::Value& stream) {
    UNREACHABLE_MSG("Geometry streams");
}

void EmitDebugPrint(EmitContext& ctx, IR::Inst* inst, Id fmt, Id arg0, Id arg1, Id arg2, Id arg3) {
    IR::DebugPrintFlags flags = inst->Flags<IR::DebugPrintFlags>();
    std::array<Id, IR::DEBUGPRINT_NUM_FORMAT_ARGS> fmt_args = {arg0, arg1, arg2, arg3};
    auto fmt_args_span = std::span<Id>(fmt_args.begin(), fmt_args.begin() + flags.num_args);
    ctx.OpDebugPrintf(fmt, fmt_args_span);
}

Id EmitMemtime(EmitContext& ctx) {
    if (ctx.profile.supports_shader_subgroup_clock) {
        return ctx.OpReadClockKHR(ctx.U64, ctx.ConstU32(std::to_underlying(spv::Scope::Subgroup)));
    } else {
        return ctx.Constant(ctx.U64, 1U);
    }
}

} // namespace Shader::Backend::SPIRV
