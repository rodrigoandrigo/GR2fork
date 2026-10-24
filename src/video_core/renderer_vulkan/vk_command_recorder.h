// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Scheduler;

/// Mirrors the vk::CommandBuffer methods the renderer uses; every call lands on the
/// scheduler's current primary command buffer.
class CommandRecorder {
public:
    explicit CommandRecorder(Scheduler& scheduler_) noexcept : scheduler{&scheduler_} {}

    void pipelineBarrier2(const vk::DependencyInfo& dependency_info) const;

    void bindPipeline(vk::PipelineBindPoint pipeline_bind_point, vk::Pipeline pipeline) const;
    void bindDescriptorSets(vk::PipelineBindPoint pipeline_bind_point, vk::PipelineLayout layout,
                            u32 first_set,
                            vk::ArrayProxy<const vk::DescriptorSet> const& descriptor_sets,
                            vk::ArrayProxy<const u32> const& dynamic_offsets) const;
    void pushDescriptorSetKHR(
        vk::PipelineBindPoint pipeline_bind_point, vk::PipelineLayout layout, u32 set,
        vk::ArrayProxy<const vk::WriteDescriptorSet> const& descriptor_writes) const;
    void pushDescriptorSet2(const vk::PushDescriptorSetInfo& push_descriptor_set_info) const;
    void pushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stage_flags, u32 offset,
                       u32 size, const void* values) const;
    void pushConstants2(const vk::PushConstantsInfo& push_constants_info) const;
    void bindVertexBuffers(u32 first_binding, u32 binding_count, const vk::Buffer* buffers,
                           const vk::DeviceSize* offsets) const;
    void bindVertexBuffers2(u32 first_binding, u32 binding_count, const vk::Buffer* buffers,
                            const vk::DeviceSize* offsets, const vk::DeviceSize* sizes,
                            const vk::DeviceSize* strides) const;
    void bindIndexBuffer(vk::Buffer buffer, vk::DeviceSize offset, vk::IndexType index_type) const;
    void bindIndexBuffer2KHR(vk::Buffer buffer, vk::DeviceSize offset, vk::DeviceSize size,
                             vk::IndexType index_type) const;
    void setVertexInputEXT(
        vk::ArrayProxy<const vk::VertexInputBindingDescription2EXT> const& binding_descriptions,
        vk::ArrayProxy<const vk::VertexInputAttributeDescription2EXT> const& attribute_descriptions)
        const;

    void setViewport(u32 first_viewport, vk::ArrayProxy<const vk::Viewport> const& viewports) const;
    void setScissor(u32 first_scissor, vk::ArrayProxy<const vk::Rect2D> const& scissors) const;
    void setViewportWithCount(vk::ArrayProxy<const vk::Viewport> const& viewports) const;
    void setScissorWithCount(vk::ArrayProxy<const vk::Rect2D> const& scissors) const;
    void setDepthTestEnable(vk::Bool32 depth_test_enable) const;
    void setDepthWriteEnable(vk::Bool32 depth_write_enable) const;
    void setDepthCompareOp(vk::CompareOp depth_compare_op) const;
    void setDepthBoundsTestEnable(vk::Bool32 depth_bounds_test_enable) const;
    void setDepthBounds(float min_depth_bounds, float max_depth_bounds) const;
    void setDepthBiasEnable(vk::Bool32 depth_bias_enable) const;
    void setDepthBias(float depth_bias_constant_factor, float depth_bias_clamp,
                      float depth_bias_slope_factor) const;
    void setStencilTestEnable(vk::Bool32 stencil_test_enable) const;
    void setStencilOp(vk::StencilFaceFlags face_mask, vk::StencilOp fail_op, vk::StencilOp pass_op,
                      vk::StencilOp depth_fail_op, vk::CompareOp compare_op) const;
    void setStencilReference(vk::StencilFaceFlags face_mask, u32 reference) const;
    void setStencilWriteMask(vk::StencilFaceFlags face_mask, u32 write_mask) const;
    void setStencilCompareMask(vk::StencilFaceFlags face_mask, u32 compare_mask) const;
    void setPrimitiveRestartEnable(vk::Bool32 primitive_restart_enable) const;
    void setRasterizerDiscardEnable(vk::Bool32 rasterizer_discard_enable) const;
    void setCullMode(vk::CullModeFlags cull_mode) const;
    void setFrontFace(vk::FrontFace front_face) const;
    void setBlendConstants(const float blend_constants[4]) const;
    void setColorWriteMaskEXT(
        u32 first_attachment,
        vk::ArrayProxy<const vk::ColorComponentFlags> const& color_write_masks) const;
    void setLineWidth(float line_width) const;
    void setAttachmentFeedbackLoopEnableEXT(vk::ImageAspectFlags aspect_mask) const;

    void beginRendering(const vk::RenderingInfo& rendering_info) const;
    void endRendering() const;
    void draw(u32 vertex_count, u32 instance_count, u32 first_vertex, u32 first_instance) const;
    void drawIndexed(u32 index_count, u32 instance_count, u32 first_index, s32 vertex_offset,
                     u32 first_instance) const;
    void drawIndirect(vk::Buffer buffer, vk::DeviceSize offset, u32 draw_count, u32 stride) const;
    void drawIndexedIndirect(vk::Buffer buffer, vk::DeviceSize offset, u32 draw_count,
                             u32 stride) const;
    void drawIndirectCount(vk::Buffer buffer, vk::DeviceSize offset, vk::Buffer count_buffer,
                           vk::DeviceSize count_buffer_offset, u32 max_draw_count,
                           u32 stride) const;
    void drawIndexedIndirectCount(vk::Buffer buffer, vk::DeviceSize offset, vk::Buffer count_buffer,
                                  vk::DeviceSize count_buffer_offset, u32 max_draw_count,
                                  u32 stride) const;
    void dispatch(u32 group_count_x, u32 group_count_y, u32 group_count_z) const;
    void dispatchIndirect(vk::Buffer buffer, vk::DeviceSize offset) const;

    void copyBuffer(vk::Buffer src_buffer, vk::Buffer dst_buffer,
                    vk::ArrayProxy<const vk::BufferCopy> const& regions) const;
    void copyImage(vk::Image src_image, vk::ImageLayout src_image_layout, vk::Image dst_image,
                   vk::ImageLayout dst_image_layout,
                   vk::ArrayProxy<const vk::ImageCopy> const& regions) const;
    void copyBufferToImage(vk::Buffer src_buffer, vk::Image dst_image,
                           vk::ImageLayout dst_image_layout,
                           vk::ArrayProxy<const vk::BufferImageCopy> const& regions) const;
    void copyImageToBuffer(vk::Image src_image, vk::ImageLayout src_image_layout,
                           vk::Buffer dst_buffer,
                           vk::ArrayProxy<const vk::BufferImageCopy> const& regions) const;
    void resolveImage(vk::Image src_image, vk::ImageLayout src_image_layout, vk::Image dst_image,
                      vk::ImageLayout dst_image_layout,
                      vk::ArrayProxy<const vk::ImageResolve> const& regions) const;
    void fillBuffer(vk::Buffer dst_buffer, vk::DeviceSize dst_offset, vk::DeviceSize size,
                    u32 data) const;
    void updateBuffer(vk::Buffer dst_buffer, vk::DeviceSize dst_offset, vk::DeviceSize data_size,
                      const void* data) const;
    void clearColorImage(vk::Image image, vk::ImageLayout image_layout,
                         const vk::ClearColorValue& color,
                         vk::ArrayProxy<const vk::ImageSubresourceRange> const& ranges) const;

    void beginDebugUtilsLabelEXT(const vk::DebugUtilsLabelEXT& label_info) const;
    void insertDebugUtilsLabelEXT(const vk::DebugUtilsLabelEXT& label_info) const;
    void endDebugUtilsLabelEXT() const;

private:
    /// The current command buffer, read directly from the scheduler for the inline branches.
    vk::CommandBuffer Raw() const;

    Scheduler* scheduler;
};

} // namespace Vulkan
