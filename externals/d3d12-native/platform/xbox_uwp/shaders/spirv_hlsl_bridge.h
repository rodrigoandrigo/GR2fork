// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "shader_recompiler/push_data.h"
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Xbox::Shaders {
struct ComputeTranslation {
  std::string hlsl;
  std::array<std::uint32_t, 3> local_size{};
  std::uint32_t descriptor_set{};
  std::uint32_t binding{};
  std::uint32_t push_constant_words{};
};
enum class GraphicsStage { Vertex, Fragment };
struct GraphicsTranslation {
  std::string hlsl;
  GraphicsStage stage{};
  std::uint32_t constant_register{};
  std::uint32_t constant_space{};
  std::uint32_t push_constant_words{};
};
// Closed initial layout: location 0 float4, no textures/SSBO/vertex fetch.
// Stage-local PushData maps to b0 space1 (VS) and b0 space2 (PS).
[[nodiscard]] GraphicsTranslation
TranslateGraphics(std::span<const std::uint32_t> words, GraphicsStage stage);

// Initial contract: compute, literal workgroup size, one R32_UINT storage
// image at set=0/binding=0, optionally the exact Shader::PushData layout.
// Not a general guest shader translator.
[[nodiscard]] ComputeTranslation
TranslateCompute(std::span<const std::uint32_t> words);
[[nodiscard]] std::vector<std::uint32_t> ComputeFixture();
[[nodiscard]] std::vector<std::uint32_t> PushDataFixture();
[[nodiscard]] Shader::PushData ProbePushData();
[[nodiscard]] std::array<std::uint32_t, sizeof(Shader::PushData) / 4>
EncodePushData(const Shader::PushData &data);
} // namespace Xbox::Shaders
