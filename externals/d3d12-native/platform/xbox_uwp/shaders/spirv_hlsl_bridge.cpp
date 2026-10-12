// SPDX-License-Identifier: GPL-2.0-or-later
#include "spirv_hlsl_bridge.h"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <spirv_hlsl.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <stdexcept>

namespace Xbox::Shaders {
GraphicsTranslation TranslateGraphics(std::span<const std::uint32_t> words,
                                      GraphicsStage stage) {
  if (stage != GraphicsStage::Vertex && stage != GraphicsStage::Fragment)
    throw std::invalid_argument("unsupported graphics stage");
  if (words.size() < 5 || words.size() > 65536 || words[0] != 0x07230203 ||
      words[1] < 0x00010000 || words[1] > 0x00010600 || words[3] == 0 ||
      words[3] > 65536 || words[4] != 0)
    throw std::invalid_argument("invalid graphics SPIR-V header");
  for (std::size_t offset = 5; offset < words.size();) {
    const auto count = words[offset] >> 16;
    if (!count || count > words.size() - offset)
      throw std::invalid_argument("truncated graphics SPIR-V");
    offset += count;
  }
  spirv_cross::CompilerHLSL compiler(words.data(), words.size());
  const bool vertex = stage == GraphicsStage::Vertex;
  const auto model =
      vertex ? spv::ExecutionModelVertex : spv::ExecutionModelFragment;
  const auto entries = compiler.get_entry_points_and_stages();
  if (entries.size() != 1 || entries.front().execution_model != model)
    throw std::invalid_argument("graphics entry point stage mismatch");
  compiler.set_entry_point(entries.front().name, model);
  const auto resources = compiler.get_shader_resources();
  compiler.update_active_builtins();
  for (const auto &builtin : resources.builtin_inputs) {
    if (compiler.has_active_builtin(builtin.builtin, spv::StorageClassInput) &&
        (!vertex || builtin.builtin != spv::BuiltInVertexIndex))
      throw std::invalid_argument("unsupported graphics builtin input");
  }
  for (const auto &builtin : resources.builtin_outputs) {
    if (compiler.has_active_builtin(builtin.builtin, spv::StorageClassOutput) &&
        (!vertex || builtin.builtin != spv::BuiltInPosition))
      throw std::invalid_argument("unsupported graphics builtin output");
  }
  if (!resources.storage_images.empty() || !resources.uniform_buffers.empty() ||
      !resources.storage_buffers.empty() || !resources.sampled_images.empty() ||
      !resources.separate_images.empty() ||
      !resources.separate_samplers.empty() ||
      !resources.subpass_inputs.empty() || !resources.atomic_counters.empty() ||
      !resources.acceleration_structures.empty() ||
      !resources.gl_plain_uniforms.empty() || !resources.tensors.empty() ||
      !resources.shader_record_buffers.empty() ||
      resources.push_constant_buffers.size() != 1 ||
      resources.stage_outputs.size() != 1 ||
      resources.stage_inputs.size() != (vertex ? 0u : 1u))
    throw std::invalid_argument("unsupported graphics resources");
  const auto validate_varying = [&](const spirv_cross::Resource &resource) {
    const auto &type = compiler.get_type(resource.type_id);
    if (!compiler.has_decoration(resource.id, spv::DecorationLocation) ||
        compiler.get_decoration(resource.id, spv::DecorationLocation) != 0 ||
        type.basetype != spirv_cross::SPIRType::Float || type.width != 32 ||
        type.vecsize != 4 || type.columns != 1 || !type.array.empty() ||
        compiler.has_decoration(resource.id, spv::DecorationComponent) ||
        compiler.has_decoration(resource.id, spv::DecorationIndex))
      throw std::invalid_argument("expected location 0 float4");
  };
  for (const auto &input : resources.stage_inputs)
    validate_varying(input);
  for (const auto &output : resources.stage_outputs)
    validate_varying(output);
  const auto &push =
      compiler.get_type(resources.push_constant_buffers.front().base_type_id);
  constexpr std::array<std::uint32_t, 11> offsets{0,  4,  8,  12, 16, 32,
                                                  48, 64, 80, 96, 112};
  if (push.member_types.size() != offsets.size() ||
      compiler.get_declared_struct_size(push) != sizeof(Shader::PushData) ||
      !compiler.has_decoration(push.self, spv::DecorationBlock))
    throw std::invalid_argument("graphics PushData ABI mismatch");
  for (std::uint32_t i = 0; i < offsets.size(); ++i) {
    const auto &type = compiler.get_type(push.member_types[i]);
    if (!compiler.has_member_decoration(push.self, i, spv::DecorationOffset) ||
        compiler.type_struct_member_offset(push, i) != offsets[i] ||
        type.basetype != (i < 4 ? spirv_cross::SPIRType::Float
                                : spirv_cross::SPIRType::UInt) ||
        type.width != 32 ||
        type.vecsize != (i < 4 ? 1u : (i == 10 ? 2u : 4u)) ||
        type.columns != 1 || !type.array.empty())
      throw std::invalid_argument("graphics PushData member mismatch");
  }
  GraphicsTranslation result;
  result.stage = stage;
  result.constant_space = vertex ? 1 : 2;
  result.push_constant_words = sizeof(Shader::PushData) / 4;
  spirv_cross::CompilerHLSL::Options options;
  options.shader_model = 60;
  compiler.set_hlsl_options(options);
  spirv_cross::HLSLResourceBinding mapping{};
  mapping.stage = model;
  mapping.desc_set = spirv_cross::ResourceBindingPushConstantDescriptorSet;
  mapping.binding = spirv_cross::ResourceBindingPushConstantBinding;
  mapping.cbv.register_binding = 0;
  mapping.cbv.register_space = result.constant_space;
  compiler.add_hlsl_resource_binding(mapping);
  result.hlsl = compiler.compile();
  if (result.hlsl.empty())
    throw std::runtime_error("empty graphics HLSL");
  return result;
}
ComputeTranslation TranslateCompute(std::span<const std::uint32_t> words) {
  constexpr std::size_t MaxWords = 65536;
  if (words.size() < 5 || words.size() > MaxWords || words[0] != 0x07230203 ||
      words[1] < 0x00010000 || words[1] > 0x00010600 || words[3] == 0 ||
      words[3] > MaxWords || words[4] != 0) {
    throw std::invalid_argument("invalid or oversized SPIR-V header");
  }
  for (std::size_t offset = 5; offset < words.size();) {
    const auto count = words[offset] >> 16;
    if (count == 0 || count > words.size() - offset) {
      throw std::invalid_argument("truncated SPIR-V instruction");
    }
    offset += count;
  }
  spirv_cross::CompilerHLSL compiler(words.data(), words.size());
  const auto entries = compiler.get_entry_points_and_stages();
  if (entries.size() != 1 ||
      entries.front().execution_model != spv::ExecutionModelGLCompute) {
    throw std::invalid_argument("only one compute entry point is supported");
  }
  compiler.set_entry_point(entries.front().name, spv::ExecutionModelGLCompute);
  const auto resources = compiler.get_shader_resources();
  if (resources.storage_images.size() != 1 ||
      !resources.uniform_buffers.empty() ||
      !resources.storage_buffers.empty() || !resources.sampled_images.empty() ||
      !resources.separate_images.empty() ||
      !resources.separate_samplers.empty() ||
      resources.push_constant_buffers.size() > 1 ||
      !resources.subpass_inputs.empty() || !resources.atomic_counters.empty() ||
      !resources.acceleration_structures.empty() ||
      !resources.stage_inputs.empty() || !resources.stage_outputs.empty()) {
    throw std::invalid_argument("unsupported compute resource layout");
  }
  const auto &image = resources.storage_images.front();
  const auto &type = compiler.get_type(image.type_id);
  const auto &sampled = compiler.get_type(type.image.type);
  if (!type.array.empty() || type.image.dim != spv::Dim2D ||
      type.image.arrayed || type.image.ms ||
      type.image.format != spv::ImageFormatR32ui ||
      sampled.basetype != spirv_cross::SPIRType::UInt || sampled.width != 32 ||
      !compiler.has_decoration(image.id, spv::DecorationDescriptorSet) ||
      !compiler.has_decoration(image.id, spv::DecorationBinding) ||
      compiler.get_decoration(image.id, spv::DecorationDescriptorSet) != 0 ||
      compiler.get_decoration(image.id, spv::DecorationBinding) != 0) {
    throw std::invalid_argument(
        "expected scalar R32_UINT image at set 0 binding 0");
  }
  ComputeTranslation result;
  if (!resources.push_constant_buffers.empty()) {
    const auto &push_type =
        compiler.get_type(resources.push_constant_buffers.front().base_type_id);
    constexpr std::array<std::uint32_t, 11> offsets{0,  4,  8,  12, 16, 32,
                                                    48, 64, 80, 96, 112};
    if (push_type.member_types.size() != offsets.size() ||
        compiler.get_declared_struct_size(push_type) !=
            sizeof(Shader::PushData) ||
        !compiler.has_decoration(push_type.self, spv::DecorationBlock)) {
      throw std::invalid_argument(
          "push constants do not match Shader::PushData");
    }
    for (std::uint32_t member = 0; member < offsets.size(); ++member) {
      const auto &member_type =
          compiler.get_type(push_type.member_types[member]);
      const auto expected_type = member < 4 ? spirv_cross::SPIRType::Float
                                            : spirv_cross::SPIRType::UInt;
      const std::uint32_t expected_vector =
          member < 4 ? 1 : (member == 10 ? 2 : 4);
      if (!compiler.has_member_decoration(push_type.self, member,
                                          spv::DecorationOffset) ||
          compiler.type_struct_member_offset(push_type, member) !=
              offsets[member] ||
          member_type.basetype != expected_type || member_type.width != 32 ||
          member_type.vecsize != expected_vector || member_type.columns != 1 ||
          !member_type.array.empty()) {
        throw std::invalid_argument("incompatible PushData member offset/type");
      }
    }
    result.push_constant_words =
        static_cast<std::uint32_t>(sizeof(Shader::PushData) / 4);
  }
  if (!compiler.get_execution_mode_bitset().get(spv::ExecutionModeLocalSize) ||
      compiler.get_execution_mode_bitset().get(spv::ExecutionModeLocalSizeId)) {
    throw std::invalid_argument("literal local size required");
  }
  std::uint64_t threads = 1;
  for (std::uint32_t axis = 0; axis < 3; ++axis) {
    result.local_size[axis] =
        compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, axis);
    if (result.local_size[axis] == 0 || result.local_size[axis] > 1024) {
      throw std::invalid_argument("invalid workgroup size");
    }
    threads *= result.local_size[axis];
  }
  if (threads > 1024 || result.local_size[2] > 64) {
    throw std::invalid_argument("workgroup exceeds cs_6_0 limits");
  }
  spirv_cross::CompilerHLSL::Options options;
  options.shader_model = 60;
  compiler.set_hlsl_options(options);
  spirv_cross::HLSLResourceBinding mapping{};
  mapping.stage = spv::ExecutionModelGLCompute;
  mapping.desc_set = 0;
  mapping.binding = 0;
  mapping.uav.register_binding = 0;
  mapping.uav.register_space = 0;
  compiler.add_hlsl_resource_binding(mapping);
  if (result.push_constant_words != 0) {
    spirv_cross::HLSLResourceBinding push_mapping{};
    push_mapping.stage = spv::ExecutionModelGLCompute;
    push_mapping.desc_set =
        spirv_cross::ResourceBindingPushConstantDescriptorSet;
    push_mapping.binding = spirv_cross::ResourceBindingPushConstantBinding;
    push_mapping.cbv.register_binding = 0;
    push_mapping.cbv.register_space = 0;
    compiler.add_hlsl_resource_binding(push_mapping);
  }
  result.hlsl = compiler.compile();
  if (result.hlsl.empty()) {
    throw std::runtime_error("SPIRV-Cross emitted empty HLSL");
  }
  return result;
}
} // namespace Xbox::Shaders
