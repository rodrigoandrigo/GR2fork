// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"

namespace Shader::Optimization {

static constexpr u32 NumUserClipPlanes = 6;

// True when the guest shader exports its own clip distances, which share the eight ClipDistance
// slots with the lowered user planes.
static bool ExportsClipDistances(u32 num_outputs, const auto& outputs) {
    for (u32 i = 0; i < num_outputs; ++i) {
        for (const auto output : outputs[i]) {
            if (output >= Output::ClipDist0 && output <= Output::ClipDist7) {
                return true;
            }
        }
    }
    return false;
}

// Emits clip distances for the enabled user planes against a position, using the equations of the
// per-draw clip planes buffer where each plane occupies four consecutive dwords.
static void EmitPlaneDistances(IR::IREmitter& ir, const IR::U32& handle,
                               const std::array<IR::Value, 4>& position, u32 enabled_mask) {
    for (u32 plane = 0; plane < NumUserClipPlanes; ++plane) {
        if (((enabled_mask >> plane) & 1) == 0) {
            continue;
        }
        const IR::Value equation = ir.LoadBufferF32(4, handle, ir.Imm32(plane * 4), {});
        IR::F32 distance{ir.FPMul(IR::F32{position[0]}, IR::F32{ir.CompositeExtract(equation, 0)})};
        for (u32 comp = 1; comp < 4; ++comp) {
            distance = IR::F32{ir.FPFma(IR::F32{position[comp]},
                                        IR::F32{ir.CompositeExtract(equation, comp)}, distance)};
        }
        ir.SetAttribute(IR::Attribute::ClipDistance, distance, plane);
    }
}

// Lowers the fixed-function user clip planes in PA_CL_UCP_*, enabled by the UCP_ENA bits of
// PA_CL_CLIP_CNTL, to clip distances. Planes apply to the post-transform position and change per
// draw, so they are supplied through a per-draw uniform buffer. Geometry programs lower against
// every emitted vertex, since the final position is only known there.
void LowerUserClipPlanes(IR::Program& program, const RuntimeInfo& runtime_info) {
    Info& info = program.info;
    const bool is_geometry = info.hw_stage == HwStage::Geometry;
    if (info.hw_stage != HwStage::Vertex && !is_geometry) {
        return;
    }

    const u32 binding = static_cast<u32>(info.buffers.size());
    info.buffers.push_back(BufferResource{
        .used_types = IR::Type::F32,
        .buffer_type = BufferType::ClipPlanes,
    });

    const u32 enabled_mask = is_geometry ? runtime_info.hw.gs.user_clip_plane_mask
                                         : runtime_info.hw.vs.user_clip_plane_mask;
    if (enabled_mask == 0) {
        return;
    }

    // GCN clips against user planes and shader-exported distances independently, but both share
    // the eight ClipDistance slots here, and the guest's exports own their indices. Lowering is
    // skipped for such shaders, which restores the previous behavior of clipping by the exported
    // distances alone.
    if (ExportsClipDistances(
            is_geometry ? runtime_info.hw.gs.num_outputs : runtime_info.hw.vs.num_outputs,
            is_geometry ? runtime_info.hw.gs.outputs : runtime_info.hw.vs.outputs)) {
        LOG_WARNING(Render_Vulkan,
                    "Skipping user clip plane lowering: shader {:#x} exports its own clip "
                    "distances",
                    info.pgm_hash);
        return;
    }

    if (is_geometry) {
        // Clip distances are computed from the final exported position right before each vertex
        // is emitted. Position components are tracked from the guest's own exports, whose order
        // in the copy shader pattern runs ahead of the emit signal.
        std::array<IR::Value, 4> position{};
        for (IR::Block* const block : program.blocks) {
            for (auto inst = block->begin(); inst != block->end(); ++inst) {
                if (inst->GetOpcode() == IR::Opcode::SetAttribute &&
                    inst->Arg(0).Attribute() == IR::Attribute::Position0) {
                    position[inst->Arg(2).U32()] = inst->Arg(1);
                    continue;
                }
                if (inst->GetOpcode() != IR::Opcode::EmitVertex) {
                    continue;
                }
                if (std::ranges::any_of(position, [](const IR::Value& v) { return v.IsEmpty(); })) {
                    LOG_WARNING(Render_Vulkan,
                                "Vertex emitted without a full position export for user clip "
                                "plane lowering in shader {:#x}",
                                info.pgm_hash);
                    continue;
                }
                IR::IREmitter insertion{*block, inst};
                EmitPlaneDistances(insertion, insertion.Imm32(binding), position, enabled_mask);
            }
        }
        return;
    }

    // Clip distances are computed from the final exported position.
    std::array<IR::Value, 4> position{};
    IR::Block* target_block{};
    IR::Block::iterator target_it{};
    for (IR::Block* const block : program.blocks) {
        for (auto inst = block->begin(); inst != block->end(); ++inst) {
            if (inst->GetOpcode() != IR::Opcode::SetAttribute ||
                inst->Arg(0).Attribute() != IR::Attribute::Position0) {
                continue;
            }
            position[inst->Arg(2).U32()] = inst->Arg(1);
            target_block = block;
            target_it = std::next(inst);
        }
    }
    if (std::ranges::any_of(position, [](const IR::Value& v) { return v.IsEmpty(); })) {
        return;
    }

    IR::IREmitter ir{*target_block, target_it};
    EmitPlaneDistances(ir, ir.Imm32(binding), position, enabled_mask);
}

} // namespace Shader::Optimization
