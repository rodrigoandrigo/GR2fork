// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>

#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_command_recorder.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {

/// Debug label names are stored up to this size, terminator included.
constexpr size_t MaxLabelBytes = 64;

/// The record's payload viewed as an array of T.
template <typename T>
const T* PayloadAs(const std::byte* payload) noexcept {
    return reinterpret_cast<const T*>(payload);
}

/// Appends count elements to the payload at cursor and returns their copy, null for none. Arrays
/// of different types follow each other unpadded; the assert checks the alignment each needs.
template <typename T>
T* Append(std::byte*& cursor, const T* data, size_t count) {
    if (count == 0) {
        return nullptr;
    }
    DEBUG_ASSERT(reinterpret_cast<std::uintptr_t>(cursor) % alignof(T) == 0);
    T* const copy = reinterpret_cast<T*>(cursor);
    std::memcpy(copy, data, sizeof(T) * count);
    cursor += sizeof(T) * count;
    return copy;
}

/// Append for structs with an sType header. The copy omits pNext chains, so a struct carrying one
/// fails the assert instead of dangling at replay.
template <typename T>
T* AppendStructs(std::byte*& cursor, const T* data, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        ASSERT_MSG(data[i].pNext == nullptr, "Recorded {} carries a pNext chain",
                   vk::to_string(data[i].sType));
    }
    return Append(cursor, data, count);
}

/// Appends descriptor writes and, behind them, the infos each write points at, chosen by its
/// descriptor type as the driver chooses them; the copies point into the payload.
vk::WriteDescriptorSet* AppendDescriptorWrites(std::byte*& cursor,
                                               const vk::WriteDescriptorSet* writes, u32 count) {
    vk::WriteDescriptorSet* const copies = AppendStructs(cursor, writes, count);
    for (u32 i = 0; i < count; ++i) {
        const vk::WriteDescriptorSet& write = writes[i];
        vk::WriteDescriptorSet& copy = copies[i];
        copy.pImageInfo = nullptr;
        copy.pBufferInfo = nullptr;
        copy.pTexelBufferView = nullptr;
        switch (write.descriptorType) {
        case vk::DescriptorType::eUniformBuffer:
        case vk::DescriptorType::eStorageBuffer:
            copy.pBufferInfo = Append(cursor, write.pBufferInfo, write.descriptorCount);
            break;
        case vk::DescriptorType::eUniformTexelBuffer:
        case vk::DescriptorType::eStorageTexelBuffer:
            copy.pTexelBufferView = Append(cursor, write.pTexelBufferView, write.descriptorCount);
            break;
        case vk::DescriptorType::eSampler:
        case vk::DescriptorType::eCombinedImageSampler:
        case vk::DescriptorType::eSampledImage:
        case vk::DescriptorType::eStorageImage:
        case vk::DescriptorType::eInputAttachment:
            copy.pImageInfo = Append(cursor, write.pImageInfo, write.descriptorCount);
            break;
        default:
            UNREACHABLE_MSG("Unexpected descriptor type {}", vk::to_string(write.descriptorType));
        }
    }
    return copies;
}

/// Bytes of a copy of count descriptor writes with their infos. Image and buffer infos are 24
/// bytes and bound the 8 byte texel buffer views.
size_t DescriptorWritesSize(const vk::WriteDescriptorSet* writes, u32 count) {
    static_assert(sizeof(vk::DescriptorBufferInfo) <= sizeof(vk::DescriptorImageInfo) &&
                  sizeof(vk::BufferView) <= sizeof(vk::DescriptorImageInfo));
    size_t size = sizeof(vk::WriteDescriptorSet) * count;
    for (u32 i = 0; i < count; ++i) {
        size += sizeof(vk::DescriptorImageInfo) * writes[i].descriptorCount;
    }
    return size;
}

/// Copies a debug label with at most 63 bytes of its name; guest marker strings may lack a NUL
/// terminator.
void AppendLabel(std::byte* cursor, const vk::DebugUtilsLabelEXT& label_info) {
    vk::DebugUtilsLabelEXT& copy = *AppendStructs(cursor, &label_info, 1);
    char* const name = reinterpret_cast<char*>(cursor);
    size_t length = 0;
    while (length < MaxLabelBytes - 1 && label_info.pLabelName[length] != '\0') {
        ++length;
    }
    std::memcpy(name, label_info.pLabelName, length);
    name[length] = '\0';
    copy.pLabelName = name;
}

} // namespace

vk::CommandBuffer CommandRecorder::Raw() const {
    return scheduler->sessions.back().primary;
}

void CommandRecorder::pipelineBarrier2(const vk::DependencyInfo& dependency_info) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().pipelineBarrier2(dependency_info);
        return;
    }
    const size_t size =
        sizeof(vk::DependencyInfo) +
        sizeof(vk::MemoryBarrier2) * dependency_info.memoryBarrierCount +
        sizeof(vk::BufferMemoryBarrier2) * dependency_info.bufferMemoryBarrierCount +
        sizeof(vk::ImageMemoryBarrier2) * dependency_info.imageMemoryBarrierCount;
    std::byte* cursor =
        scheduler->RecordWithPayload(size, [](vk::CommandBuffer cmdbuf, const std::byte* payload) {
            cmdbuf.pipelineBarrier2(*PayloadAs<vk::DependencyInfo>(payload));
        });
    vk::DependencyInfo& copy = *AppendStructs(cursor, &dependency_info, 1);
    copy.pMemoryBarriers =
        AppendStructs(cursor, dependency_info.pMemoryBarriers, dependency_info.memoryBarrierCount);
    copy.pBufferMemoryBarriers = AppendStructs(cursor, dependency_info.pBufferMemoryBarriers,
                                               dependency_info.bufferMemoryBarrierCount);
    copy.pImageMemoryBarriers = AppendStructs(cursor, dependency_info.pImageMemoryBarriers,
                                              dependency_info.imageMemoryBarrierCount);
}

void CommandRecorder::bindPipeline(vk::PipelineBindPoint pipeline_bind_point,
                                   vk::Pipeline pipeline) const {
    scheduler->Record([pipeline_bind_point, pipeline](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindPipeline(pipeline_bind_point, pipeline);
    });
}

void CommandRecorder::bindDescriptorSets(
    vk::PipelineBindPoint pipeline_bind_point, vk::PipelineLayout layout, u32 first_set,
    vk::ArrayProxy<const vk::DescriptorSet> const& descriptor_sets,
    vk::ArrayProxy<const u32> const& dynamic_offsets) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().bindDescriptorSets(pipeline_bind_point, layout, first_set, descriptor_sets,
                                 dynamic_offsets);
        return;
    }
    const u32 num_sets = descriptor_sets.size();
    const u32 num_offsets = dynamic_offsets.size();
    std::byte* cursor = scheduler->RecordWithPayload(
        sizeof(vk::DescriptorSet) * num_sets + sizeof(u32) * num_offsets,
        [pipeline_bind_point, layout, first_set, num_sets, num_offsets](vk::CommandBuffer cmdbuf,
                                                                        const std::byte* payload) {
            cmdbuf.bindDescriptorSets(
                pipeline_bind_point, layout, first_set, num_sets,
                PayloadAs<vk::DescriptorSet>(payload), num_offsets,
                PayloadAs<u32>(payload + sizeof(vk::DescriptorSet) * num_sets));
        });
    Append(cursor, descriptor_sets.data(), num_sets);
    Append(cursor, dynamic_offsets.data(), num_offsets);
}

void CommandRecorder::pushDescriptorSetKHR(
    vk::PipelineBindPoint pipeline_bind_point, vk::PipelineLayout layout, u32 set,
    vk::ArrayProxy<const vk::WriteDescriptorSet> const& descriptor_writes) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().pushDescriptorSetKHR(pipeline_bind_point, layout, set, descriptor_writes);
        return;
    }
    const u32 count = descriptor_writes.size();
    std::byte* cursor = scheduler->RecordWithPayload(
        DescriptorWritesSize(descriptor_writes.data(), count),
        [pipeline_bind_point, layout, set, count](vk::CommandBuffer cmdbuf,
                                                  const std::byte* payload) {
            cmdbuf.pushDescriptorSetKHR(pipeline_bind_point, layout, set, count,
                                        PayloadAs<vk::WriteDescriptorSet>(payload));
        });
    AppendDescriptorWrites(cursor, descriptor_writes.data(), count);
}

void CommandRecorder::pushDescriptorSet2(
    const vk::PushDescriptorSetInfo& push_descriptor_set_info) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().pushDescriptorSet2(push_descriptor_set_info);
        return;
    }
    std::byte* cursor = scheduler->RecordWithPayload(
        sizeof(vk::PushDescriptorSetInfo) +
            DescriptorWritesSize(push_descriptor_set_info.pDescriptorWrites,
                                 push_descriptor_set_info.descriptorWriteCount),
        [](vk::CommandBuffer cmdbuf, const std::byte* payload) {
            cmdbuf.pushDescriptorSet2(*PayloadAs<vk::PushDescriptorSetInfo>(payload));
        });
    vk::PushDescriptorSetInfo& copy = *AppendStructs(cursor, &push_descriptor_set_info, 1);
    copy.pDescriptorWrites =
        AppendDescriptorWrites(cursor, push_descriptor_set_info.pDescriptorWrites,
                               push_descriptor_set_info.descriptorWriteCount);
}

void CommandRecorder::pushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stage_flags,
                                    u32 offset, u32 size, const void* values) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().pushConstants(layout, stage_flags, offset, size, values);
        return;
    }
    std::byte* cursor = scheduler->RecordWithPayload(
        size,
        [layout, stage_flags, offset, size](vk::CommandBuffer cmdbuf, const std::byte* payload) {
            cmdbuf.pushConstants(layout, stage_flags, offset, size, payload);
        });
    Append(cursor, static_cast<const std::byte*>(values), size);
}

void CommandRecorder::pushConstants2(const vk::PushConstantsInfo& push_constants_info) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().pushConstants2(push_constants_info);
        return;
    }
    std::byte* cursor = scheduler->RecordWithPayload(
        sizeof(vk::PushConstantsInfo) + push_constants_info.size,
        [](vk::CommandBuffer cmdbuf, const std::byte* payload) {
            cmdbuf.pushConstants2(*PayloadAs<vk::PushConstantsInfo>(payload));
        });
    vk::PushConstantsInfo& copy = *AppendStructs(cursor, &push_constants_info, 1);
    copy.pValues = Append(cursor, static_cast<const std::byte*>(push_constants_info.pValues),
                          push_constants_info.size);
}

void CommandRecorder::bindVertexBuffers(u32 first_binding, u32 binding_count,
                                        const vk::Buffer* buffers,
                                        const vk::DeviceSize* offsets) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().bindVertexBuffers(first_binding, binding_count, buffers, offsets);
        return;
    }
    std::byte* cursor = scheduler->RecordWithPayload(
        (sizeof(vk::Buffer) + sizeof(vk::DeviceSize)) * binding_count,
        [first_binding, binding_count](vk::CommandBuffer cmdbuf, const std::byte* payload) {
            cmdbuf.bindVertexBuffers(
                first_binding, binding_count, PayloadAs<vk::Buffer>(payload),
                PayloadAs<vk::DeviceSize>(payload + sizeof(vk::Buffer) * binding_count));
        });
    Append(cursor, buffers, binding_count);
    Append(cursor, offsets, binding_count);
}

void CommandRecorder::bindVertexBuffers2(u32 first_binding, u32 binding_count,
                                         const vk::Buffer* buffers, const vk::DeviceSize* offsets,
                                         const vk::DeviceSize* sizes,
                                         const vk::DeviceSize* strides) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().bindVertexBuffers2(first_binding, binding_count, buffers, offsets, sizes, strides);
        return;
    }
    // pSizes and pStrides are optional in the Vulkan call.
    const u32 num_sizes = sizes ? binding_count : 0;
    const u32 num_strides = strides ? binding_count : 0;
    std::byte* cursor = scheduler->RecordWithPayload(
        sizeof(vk::Buffer) * binding_count +
            sizeof(vk::DeviceSize) * (binding_count + num_sizes + num_strides),
        [first_binding, binding_count, num_sizes, num_strides](vk::CommandBuffer cmdbuf,
                                                               const std::byte* payload) {
            // The offsets, sizes and strides follow the buffers back to back.
            const auto* device_sizes =
                PayloadAs<vk::DeviceSize>(payload + sizeof(vk::Buffer) * binding_count);
            cmdbuf.bindVertexBuffers2(
                first_binding, binding_count, PayloadAs<vk::Buffer>(payload), device_sizes,
                num_sizes ? device_sizes + binding_count : nullptr,
                num_strides ? device_sizes + binding_count + num_sizes : nullptr);
        });
    Append(cursor, buffers, binding_count);
    Append(cursor, offsets, binding_count);
    Append(cursor, sizes, num_sizes);
    Append(cursor, strides, num_strides);
}

void CommandRecorder::bindIndexBuffer(vk::Buffer buffer, vk::DeviceSize offset,
                                      vk::IndexType index_type) const {
    scheduler->Record([buffer, offset, index_type](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindIndexBuffer(buffer, offset, index_type);
    });
}

void CommandRecorder::bindIndexBuffer2KHR(vk::Buffer buffer, vk::DeviceSize offset,
                                          vk::DeviceSize size, vk::IndexType index_type) const {
    scheduler->Record([buffer, offset, size, index_type](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindIndexBuffer2KHR(buffer, offset, size, index_type);
    });
}

void CommandRecorder::setVertexInputEXT(
    vk::ArrayProxy<const vk::VertexInputBindingDescription2EXT> const& binding_descriptions,
    vk::ArrayProxy<const vk::VertexInputAttributeDescription2EXT> const& attribute_descriptions)
    const {
    if (!scheduler->HasRecordingThread()) {
        Raw().setVertexInputEXT(binding_descriptions, attribute_descriptions);
        return;
    }
    const u32 num_bindings = binding_descriptions.size();
    const u32 num_attributes = attribute_descriptions.size();
    std::byte* cursor = scheduler->RecordWithPayload(
        sizeof(vk::VertexInputBindingDescription2EXT) * num_bindings +
            sizeof(vk::VertexInputAttributeDescription2EXT) * num_attributes,
        [num_bindings, num_attributes](vk::CommandBuffer cmdbuf, const std::byte* payload) {
            cmdbuf.setVertexInputEXT(
                num_bindings, PayloadAs<vk::VertexInputBindingDescription2EXT>(payload),
                num_attributes,
                PayloadAs<vk::VertexInputAttributeDescription2EXT>(
                    payload + sizeof(vk::VertexInputBindingDescription2EXT) * num_bindings));
        });
    AppendStructs(cursor, binding_descriptions.data(), num_bindings);
    AppendStructs(cursor, attribute_descriptions.data(), num_attributes);
}

void CommandRecorder::setViewport(u32 first_viewport,
                                  vk::ArrayProxy<const vk::Viewport> const& viewports) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().setViewport(first_viewport, viewports);
        return;
    }
    const u32 count = viewports.size();
    std::byte* cursor = scheduler->RecordWithPayload(
        sizeof(vk::Viewport) * count,
        [first_viewport, count](vk::CommandBuffer cmdbuf, const std::byte* payload) {
            cmdbuf.setViewport(first_viewport, count, PayloadAs<vk::Viewport>(payload));
        });
    Append(cursor, viewports.data(), count);
}

void CommandRecorder::setScissor(u32 first_scissor,
                                 vk::ArrayProxy<const vk::Rect2D> const& scissors) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().setScissor(first_scissor, scissors);
        return;
    }
    const u32 count = scissors.size();
    std::byte* cursor = scheduler->RecordWithPayload(
        sizeof(vk::Rect2D) * count,
        [first_scissor, count](vk::CommandBuffer cmdbuf, const std::byte* payload) {
            cmdbuf.setScissor(first_scissor, count, PayloadAs<vk::Rect2D>(payload));
        });
    Append(cursor, scissors.data(), count);
}

void CommandRecorder::setViewportWithCount(
    vk::ArrayProxy<const vk::Viewport> const& viewports) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().setViewportWithCount(viewports);
        return;
    }
    const u32 count = viewports.size();
    std::byte* cursor = scheduler->RecordWithPayload(
        sizeof(vk::Viewport) * count, [count](vk::CommandBuffer cmdbuf, const std::byte* payload) {
            cmdbuf.setViewportWithCount(count, PayloadAs<vk::Viewport>(payload));
        });
    Append(cursor, viewports.data(), count);
}

void CommandRecorder::setScissorWithCount(vk::ArrayProxy<const vk::Rect2D> const& scissors) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().setScissorWithCount(scissors);
        return;
    }
    const u32 count = scissors.size();
    std::byte* cursor = scheduler->RecordWithPayload(
        sizeof(vk::Rect2D) * count, [count](vk::CommandBuffer cmdbuf, const std::byte* payload) {
            cmdbuf.setScissorWithCount(count, PayloadAs<vk::Rect2D>(payload));
        });
    Append(cursor, scissors.data(), count);
}

void CommandRecorder::setDepthTestEnable(vk::Bool32 depth_test_enable) const {
    scheduler->Record([depth_test_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.setDepthTestEnable(depth_test_enable);
    });
}

void CommandRecorder::setDepthWriteEnable(vk::Bool32 depth_write_enable) const {
    scheduler->Record([depth_write_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.setDepthWriteEnable(depth_write_enable);
    });
}

void CommandRecorder::setDepthCompareOp(vk::CompareOp depth_compare_op) const {
    scheduler->Record([depth_compare_op](vk::CommandBuffer cmdbuf) {
        cmdbuf.setDepthCompareOp(depth_compare_op);
    });
}

void CommandRecorder::setDepthBoundsTestEnable(vk::Bool32 depth_bounds_test_enable) const {
    scheduler->Record([depth_bounds_test_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.setDepthBoundsTestEnable(depth_bounds_test_enable);
    });
}

void CommandRecorder::setDepthBounds(float min_depth_bounds, float max_depth_bounds) const {
    scheduler->Record([min_depth_bounds, max_depth_bounds](vk::CommandBuffer cmdbuf) {
        cmdbuf.setDepthBounds(min_depth_bounds, max_depth_bounds);
    });
}

void CommandRecorder::setDepthBiasEnable(vk::Bool32 depth_bias_enable) const {
    scheduler->Record([depth_bias_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.setDepthBiasEnable(depth_bias_enable);
    });
}

void CommandRecorder::setDepthBias(float depth_bias_constant_factor, float depth_bias_clamp,
                                   float depth_bias_slope_factor) const {
    scheduler->Record([depth_bias_constant_factor, depth_bias_clamp,
                       depth_bias_slope_factor](vk::CommandBuffer cmdbuf) {
        cmdbuf.setDepthBias(depth_bias_constant_factor, depth_bias_clamp, depth_bias_slope_factor);
    });
}

void CommandRecorder::setStencilTestEnable(vk::Bool32 stencil_test_enable) const {
    scheduler->Record([stencil_test_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.setStencilTestEnable(stencil_test_enable);
    });
}

void CommandRecorder::setStencilOp(vk::StencilFaceFlags face_mask, vk::StencilOp fail_op,
                                   vk::StencilOp pass_op, vk::StencilOp depth_fail_op,
                                   vk::CompareOp compare_op) const {
    scheduler->Record(
        [face_mask, fail_op, pass_op, depth_fail_op, compare_op](vk::CommandBuffer cmdbuf) {
            cmdbuf.setStencilOp(face_mask, fail_op, pass_op, depth_fail_op, compare_op);
        });
}

void CommandRecorder::setStencilReference(vk::StencilFaceFlags face_mask, u32 reference) const {
    scheduler->Record([face_mask, reference](vk::CommandBuffer cmdbuf) {
        cmdbuf.setStencilReference(face_mask, reference);
    });
}

void CommandRecorder::setStencilWriteMask(vk::StencilFaceFlags face_mask, u32 write_mask) const {
    scheduler->Record([face_mask, write_mask](vk::CommandBuffer cmdbuf) {
        cmdbuf.setStencilWriteMask(face_mask, write_mask);
    });
}

void CommandRecorder::setStencilCompareMask(vk::StencilFaceFlags face_mask,
                                            u32 compare_mask) const {
    scheduler->Record([face_mask, compare_mask](vk::CommandBuffer cmdbuf) {
        cmdbuf.setStencilCompareMask(face_mask, compare_mask);
    });
}

void CommandRecorder::setPrimitiveRestartEnable(vk::Bool32 primitive_restart_enable) const {
    scheduler->Record([primitive_restart_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.setPrimitiveRestartEnable(primitive_restart_enable);
    });
}

void CommandRecorder::setRasterizerDiscardEnable(vk::Bool32 rasterizer_discard_enable) const {
    scheduler->Record([rasterizer_discard_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.setRasterizerDiscardEnable(rasterizer_discard_enable);
    });
}

void CommandRecorder::setCullMode(vk::CullModeFlags cull_mode) const {
    scheduler->Record([cull_mode](vk::CommandBuffer cmdbuf) { cmdbuf.setCullMode(cull_mode); });
}

void CommandRecorder::setFrontFace(vk::FrontFace front_face) const {
    scheduler->Record([front_face](vk::CommandBuffer cmdbuf) { cmdbuf.setFrontFace(front_face); });
}

void CommandRecorder::setBlendConstants(const float blend_constants[4]) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().setBlendConstants(blend_constants);
        return;
    }
    std::array<float, 4> constants;
    std::copy_n(blend_constants, constants.size(), constants.begin());
    scheduler->Record(
        [constants](vk::CommandBuffer cmdbuf) { cmdbuf.setBlendConstants(constants.data()); });
}

void CommandRecorder::setColorWriteMaskEXT(
    u32 first_attachment,
    vk::ArrayProxy<const vk::ColorComponentFlags> const& color_write_masks) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().setColorWriteMaskEXT(first_attachment, color_write_masks);
        return;
    }
    const u32 count = color_write_masks.size();
    std::byte* cursor = scheduler->RecordWithPayload(
        sizeof(vk::ColorComponentFlags) * count,
        [first_attachment, count](vk::CommandBuffer cmdbuf, const std::byte* payload) {
            cmdbuf.setColorWriteMaskEXT(first_attachment, count,
                                        PayloadAs<vk::ColorComponentFlags>(payload));
        });
    Append(cursor, color_write_masks.data(), count);
}

void CommandRecorder::setLineWidth(float line_width) const {
    scheduler->Record([line_width](vk::CommandBuffer cmdbuf) { cmdbuf.setLineWidth(line_width); });
}

void CommandRecorder::setAttachmentFeedbackLoopEnableEXT(vk::ImageAspectFlags aspect_mask) const {
    scheduler->Record([aspect_mask](vk::CommandBuffer cmdbuf) {
        cmdbuf.setAttachmentFeedbackLoopEnableEXT(aspect_mask);
    });
}

void CommandRecorder::beginRendering(const vk::RenderingInfo& rendering_info) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().beginRendering(rendering_info);
        return;
    }
    const u32 num_depth = rendering_info.pDepthAttachment ? 1 : 0;
    const u32 num_stencil = rendering_info.pStencilAttachment ? 1 : 0;
    std::byte* cursor = scheduler->RecordWithPayload(
        sizeof(vk::RenderingInfo) +
            sizeof(vk::RenderingAttachmentInfo) *
                (rendering_info.colorAttachmentCount + num_depth + num_stencil),
        [](vk::CommandBuffer cmdbuf, const std::byte* payload) {
            cmdbuf.beginRendering(*PayloadAs<vk::RenderingInfo>(payload));
        });
    vk::RenderingInfo& copy = *AppendStructs(cursor, &rendering_info, 1);
    copy.pColorAttachments = AppendStructs(cursor, rendering_info.pColorAttachments,
                                           rendering_info.colorAttachmentCount);
    copy.pDepthAttachment = AppendStructs(cursor, rendering_info.pDepthAttachment, num_depth);
    copy.pStencilAttachment = AppendStructs(cursor, rendering_info.pStencilAttachment, num_stencil);
}

void CommandRecorder::endRendering() const {
    scheduler->Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endRendering(); });
}

void CommandRecorder::draw(u32 vertex_count, u32 instance_count, u32 first_vertex,
                           u32 first_instance) const {
    scheduler->Record(
        [vertex_count, instance_count, first_vertex, first_instance](vk::CommandBuffer cmdbuf) {
            cmdbuf.draw(vertex_count, instance_count, first_vertex, first_instance);
        });
}

void CommandRecorder::drawIndexed(u32 index_count, u32 instance_count, u32 first_index,
                                  s32 vertex_offset, u32 first_instance) const {
    scheduler->Record([index_count, instance_count, first_index, vertex_offset,
                       first_instance](vk::CommandBuffer cmdbuf) {
        cmdbuf.drawIndexed(index_count, instance_count, first_index, vertex_offset, first_instance);
    });
}

void CommandRecorder::drawIndirect(vk::Buffer buffer, vk::DeviceSize offset, u32 draw_count,
                                   u32 stride) const {
    scheduler->Record([buffer, offset, draw_count, stride](vk::CommandBuffer cmdbuf) {
        cmdbuf.drawIndirect(buffer, offset, draw_count, stride);
    });
}

void CommandRecorder::drawIndexedIndirect(vk::Buffer buffer, vk::DeviceSize offset, u32 draw_count,
                                          u32 stride) const {
    scheduler->Record([buffer, offset, draw_count, stride](vk::CommandBuffer cmdbuf) {
        cmdbuf.drawIndexedIndirect(buffer, offset, draw_count, stride);
    });
}

void CommandRecorder::drawIndirectCount(vk::Buffer buffer, vk::DeviceSize offset,
                                        vk::Buffer count_buffer, vk::DeviceSize count_buffer_offset,
                                        u32 max_draw_count, u32 stride) const {
    scheduler->Record([buffer, offset, count_buffer, count_buffer_offset, max_draw_count,
                       stride](vk::CommandBuffer cmdbuf) {
        cmdbuf.drawIndirectCount(buffer, offset, count_buffer, count_buffer_offset, max_draw_count,
                                 stride);
    });
}

void CommandRecorder::drawIndexedIndirectCount(vk::Buffer buffer, vk::DeviceSize offset,
                                               vk::Buffer count_buffer,
                                               vk::DeviceSize count_buffer_offset,
                                               u32 max_draw_count, u32 stride) const {
    scheduler->Record([buffer, offset, count_buffer, count_buffer_offset, max_draw_count,
                       stride](vk::CommandBuffer cmdbuf) {
        cmdbuf.drawIndexedIndirectCount(buffer, offset, count_buffer, count_buffer_offset,
                                        max_draw_count, stride);
    });
}

void CommandRecorder::dispatch(u32 group_count_x, u32 group_count_y, u32 group_count_z) const {
    scheduler->Record([group_count_x, group_count_y, group_count_z](vk::CommandBuffer cmdbuf) {
        cmdbuf.dispatch(group_count_x, group_count_y, group_count_z);
    });
}

void CommandRecorder::dispatchIndirect(vk::Buffer buffer, vk::DeviceSize offset) const {
    scheduler->Record(
        [buffer, offset](vk::CommandBuffer cmdbuf) { cmdbuf.dispatchIndirect(buffer, offset); });
}

void CommandRecorder::copyBuffer(vk::Buffer src_buffer, vk::Buffer dst_buffer,
                                 vk::ArrayProxy<const vk::BufferCopy> const& regions) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().copyBuffer(src_buffer, dst_buffer, regions);
        return;
    }
    const u32 count = regions.size();
    std::byte* cursor = scheduler->RecordWithPayload(
        sizeof(vk::BufferCopy) * count,
        [src_buffer, dst_buffer, count](vk::CommandBuffer cmdbuf, const std::byte* payload) {
            cmdbuf.copyBuffer(src_buffer, dst_buffer, count, PayloadAs<vk::BufferCopy>(payload));
        });
    Append(cursor, regions.data(), count);
}

void CommandRecorder::copyImage(vk::Image src_image, vk::ImageLayout src_image_layout,
                                vk::Image dst_image, vk::ImageLayout dst_image_layout,
                                vk::ArrayProxy<const vk::ImageCopy> const& regions) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().copyImage(src_image, src_image_layout, dst_image, dst_image_layout, regions);
        return;
    }
    const u32 count = regions.size();
    std::byte* cursor = scheduler->RecordWithPayload(
        sizeof(vk::ImageCopy) * count, [src_image, src_image_layout, dst_image, dst_image_layout,
                                        count](vk::CommandBuffer cmdbuf, const std::byte* payload) {
            cmdbuf.copyImage(src_image, src_image_layout, dst_image, dst_image_layout, count,
                             PayloadAs<vk::ImageCopy>(payload));
        });
    Append(cursor, regions.data(), count);
}

void CommandRecorder::copyBufferToImage(
    vk::Buffer src_buffer, vk::Image dst_image, vk::ImageLayout dst_image_layout,
    vk::ArrayProxy<const vk::BufferImageCopy> const& regions) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().copyBufferToImage(src_buffer, dst_image, dst_image_layout, regions);
        return;
    }
    const u32 count = regions.size();
    std::byte* cursor = scheduler->RecordWithPayload(
        sizeof(vk::BufferImageCopy) * count,
        [src_buffer, dst_image, dst_image_layout, count](vk::CommandBuffer cmdbuf,
                                                         const std::byte* payload) {
            cmdbuf.copyBufferToImage(src_buffer, dst_image, dst_image_layout, count,
                                     PayloadAs<vk::BufferImageCopy>(payload));
        });
    Append(cursor, regions.data(), count);
}

void CommandRecorder::copyImageToBuffer(
    vk::Image src_image, vk::ImageLayout src_image_layout, vk::Buffer dst_buffer,
    vk::ArrayProxy<const vk::BufferImageCopy> const& regions) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().copyImageToBuffer(src_image, src_image_layout, dst_buffer, regions);
        return;
    }
    const u32 count = regions.size();
    std::byte* cursor = scheduler->RecordWithPayload(
        sizeof(vk::BufferImageCopy) * count,
        [src_image, src_image_layout, dst_buffer, count](vk::CommandBuffer cmdbuf,
                                                         const std::byte* payload) {
            cmdbuf.copyImageToBuffer(src_image, src_image_layout, dst_buffer, count,
                                     PayloadAs<vk::BufferImageCopy>(payload));
        });
    Append(cursor, regions.data(), count);
}

void CommandRecorder::resolveImage(vk::Image src_image, vk::ImageLayout src_image_layout,
                                   vk::Image dst_image, vk::ImageLayout dst_image_layout,
                                   vk::ArrayProxy<const vk::ImageResolve> const& regions) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().resolveImage(src_image, src_image_layout, dst_image, dst_image_layout, regions);
        return;
    }
    const u32 count = regions.size();
    std::byte* cursor = scheduler->RecordWithPayload(
        sizeof(vk::ImageResolve) * count,
        [src_image, src_image_layout, dst_image, dst_image_layout,
         count](vk::CommandBuffer cmdbuf, const std::byte* payload) {
            cmdbuf.resolveImage(src_image, src_image_layout, dst_image, dst_image_layout, count,
                                PayloadAs<vk::ImageResolve>(payload));
        });
    Append(cursor, regions.data(), count);
}

void CommandRecorder::fillBuffer(vk::Buffer dst_buffer, vk::DeviceSize dst_offset,
                                 vk::DeviceSize size, u32 data) const {
    scheduler->Record([dst_buffer, dst_offset, size, data](vk::CommandBuffer cmdbuf) {
        cmdbuf.fillBuffer(dst_buffer, dst_offset, size, data);
    });
}

void CommandRecorder::updateBuffer(vk::Buffer dst_buffer, vk::DeviceSize dst_offset,
                                   vk::DeviceSize data_size, const void* data) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().updateBuffer(dst_buffer, dst_offset, data_size, data);
        return;
    }
    std::byte* cursor = scheduler->RecordWithPayload(
        data_size,
        [dst_buffer, dst_offset, data_size](vk::CommandBuffer cmdbuf, const std::byte* payload) {
            cmdbuf.updateBuffer(dst_buffer, dst_offset, data_size, payload);
        });
    Append(cursor, static_cast<const std::byte*>(data), data_size);
}

void CommandRecorder::clearColorImage(
    vk::Image image, vk::ImageLayout image_layout, const vk::ClearColorValue& color,
    vk::ArrayProxy<const vk::ImageSubresourceRange> const& ranges) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().clearColorImage(image, image_layout, color, ranges);
        return;
    }
    const u32 count = ranges.size();
    std::byte* cursor = scheduler->RecordWithPayload(
        sizeof(vk::ImageSubresourceRange) * count,
        [image, image_layout, color, count](vk::CommandBuffer cmdbuf, const std::byte* payload) {
            cmdbuf.clearColorImage(image, image_layout, &color, count,
                                   PayloadAs<vk::ImageSubresourceRange>(payload));
        });
    Append(cursor, ranges.data(), count);
}

void CommandRecorder::beginDebugUtilsLabelEXT(const vk::DebugUtilsLabelEXT& label_info) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().beginDebugUtilsLabelEXT(label_info);
        return;
    }
    std::byte* cursor = scheduler->RecordWithPayload(
        sizeof(vk::DebugUtilsLabelEXT) + MaxLabelBytes,
        [](vk::CommandBuffer cmdbuf, const std::byte* payload) {
            cmdbuf.beginDebugUtilsLabelEXT(*PayloadAs<vk::DebugUtilsLabelEXT>(payload));
        });
    AppendLabel(cursor, label_info);
}

void CommandRecorder::insertDebugUtilsLabelEXT(const vk::DebugUtilsLabelEXT& label_info) const {
    if (!scheduler->HasRecordingThread()) {
        Raw().insertDebugUtilsLabelEXT(label_info);
        return;
    }
    std::byte* cursor = scheduler->RecordWithPayload(
        sizeof(vk::DebugUtilsLabelEXT) + MaxLabelBytes,
        [](vk::CommandBuffer cmdbuf, const std::byte* payload) {
            cmdbuf.insertDebugUtilsLabelEXT(*PayloadAs<vk::DebugUtilsLabelEXT>(payload));
        });
    AppendLabel(cursor, label_info);
}

void CommandRecorder::endDebugUtilsLabelEXT() const {
    scheduler->Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endDebugUtilsLabelEXT(); });
}

} // namespace Vulkan
