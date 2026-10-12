// SPDX-License-Identifier: GPL-2.0-or-later
#include "d3d12_pipeline_cache.h"

#include <winrt/base.h>

#include <limits>

using Microsoft::WRL::ComPtr;

namespace {
void AppendInteger(std::vector<std::uint8_t> &key, UINT64 value) {
  for (UINT i = 0; i < 8; ++i) {
    key.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
  }
}

void AppendBytes(std::vector<std::uint8_t> &key, const void *data, SIZE_T size) {
  if (!data || size == 0) {
    winrt::throw_hresult(E_INVALIDARG);
  }
  AppendInteger(key, size);
  const auto *bytes = static_cast<const std::uint8_t *>(data);
  key.insert(key.end(), bytes, bytes + size);
}
} // namespace

void D3D12DescriptorArena::Initialize(ID3D12Device *device,
                                      D3D12_DESCRIPTOR_HEAP_TYPE type,
                                      UINT capacity, bool shader_visible) {
  if (heap_ || !device || capacity == 0 ||
      type < D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
      type >= D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES ||
      (shader_visible && type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
       type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)) {
    winrt::throw_hresult(E_INVALIDARG);
  }
  D3D12_DESCRIPTOR_HEAP_DESC desc{};
  desc.Type = type;
  desc.NumDescriptors = capacity;
  desc.Flags = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                             : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  winrt::check_hresult(device->CreateDescriptorHeap(
      &desc, IID_PPV_ARGS(heap_.ReleaseAndGetAddressOf())));
  increment_ = device->GetDescriptorHandleIncrementSize(type);
  capacity_ = capacity;
  shader_visible_ = shader_visible;
}

UINT D3D12DescriptorArena::Allocate(UINT count) {
  if (!heap_ || count == 0) {
    winrt::throw_hresult(E_INVALIDARG);
  }
  if (count > capacity_ - used_) {
    winrt::throw_hresult(E_OUTOFMEMORY);
  }
  const UINT first = used_;
  used_ += count;
  return first;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorArena::Cpu(UINT index) const {
  if (!heap_ || index >= used_) {
    winrt::throw_hresult(E_INVALIDARG);
  }
  auto handle = heap_->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += static_cast<SIZE_T>(index) * increment_;
  return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12DescriptorArena::Gpu(UINT index) const {
  if (!heap_ || !shader_visible_ || index >= used_) {
    winrt::throw_hresult(E_INVALIDARG);
  }
  auto handle = heap_->GetGPUDescriptorHandleForHeapStart();
  handle.ptr += static_cast<UINT64>(index) * increment_;
  return handle;
}

void D3D12PipelineCache::Initialize(ID3D12Device *device) {
  if (device_ || !device) {
    winrt::throw_hresult(E_INVALIDARG);
  }
  device_ = device;
}

D3D12PipelineCache::Root D3D12PipelineCache::RootSignature(
    const D3D12_ROOT_SIGNATURE_DESC &description) {
  if (!device_) {
    winrt::throw_hresult(E_UNEXPECTED);
  }
  ComPtr<ID3DBlob> blob;
  ComPtr<ID3DBlob> errors;
  winrt::check_hresult(D3D12SerializeRootSignature(
      &description, D3D_ROOT_SIGNATURE_VERSION_1,
      blob.ReleaseAndGetAddressOf(), errors.ReleaseAndGetAddressOf()));
  const auto *bytes = static_cast<const std::uint8_t *>(blob->GetBufferPointer());
  std::vector<std::uint8_t> key(bytes, bytes + blob->GetBufferSize());
  if (const auto found = roots_.find(key); found != roots_.end()) {
    ++stats_.cache_hits;
    return found->second;
  }
  if (roots_.size() >= EntryLimit) {
    winrt::throw_hresult(E_OUTOFMEMORY);
  }
  auto root = std::make_shared<D3D12RootLayout>();
  winrt::check_hresult(device_->CreateRootSignature(
      0, blob->GetBufferPointer(), blob->GetBufferSize(),
      IID_PPV_ARGS(root->root_.ReleaseAndGetAddressOf())));
  root->serialized_ = key;
  roots_.emplace(std::move(key), root);
  ++stats_.root_creations;
  return root;
}

void D3D12PipelineCache::ValidateRoot(const Root &root) const {
  if (!device_ || !root) {
    winrt::throw_hresult(E_INVALIDARG);
  }
  const auto found = roots_.find(root->serialized_);
  if (found == roots_.end() || found->second != root) {
    // Reject even an equivalent layout belonging to a different cache/device.
    winrt::throw_hresult(E_INVALIDARG);
  }
}

ComPtr<ID3D12PipelineState> D3D12PipelineCache::Graphics(
    const Root &root, D3D12_SHADER_BYTECODE vertex, D3D12_SHADER_BYTECODE pixel,
    DXGI_FORMAT color_format, bool premultiplied_blend) {
  ValidateRoot(root);
  std::vector<std::uint8_t> key;
  AppendBytes(key, root->serialized_.data(), root->serialized_.size());
  AppendBytes(key, vertex.pShaderBytecode, vertex.BytecodeLength);
  AppendBytes(key, pixel.pShaderBytecode, pixel.BytecodeLength);
  AppendInteger(key, static_cast<UINT64>(color_format));
  AppendInteger(key, premultiplied_blend ? 1 : 0);
  if (const auto found = graphics_.find(key); found != graphics_.end()) {
    ++stats_.cache_hits;
    return found->second;
  }
  if (graphics_.size() >= EntryLimit) {
    winrt::throw_hresult(E_OUTOFMEMORY);
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root->Get();
  desc.VS = vertex;
  desc.PS = pixel;
  auto &blend = desc.BlendState.RenderTarget[0];
  blend.BlendEnable = premultiplied_blend;
  blend.SrcBlend = D3D12_BLEND_ONE;
  blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
  blend.BlendOp = D3D12_BLEND_OP_ADD;
  blend.SrcBlendAlpha = D3D12_BLEND_ONE;
  blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
  blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  blend.LogicOp = D3D12_LOGIC_OP_NOOP;
  blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = std::numeric_limits<UINT>::max();
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  auto &face = desc.DepthStencilState.FrontFace;
  face.StencilFailOp = D3D12_STENCIL_OP_KEEP;
  face.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
  face.StencilPassOp = D3D12_STENCIL_OP_KEEP;
  face.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  desc.DepthStencilState.BackFace = face;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = color_format;
  desc.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pipeline;
  winrt::check_hresult(device_->CreateGraphicsPipelineState(
      &desc, IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf())));
  graphics_.emplace(std::move(key), pipeline);
  ++stats_.graphics_creations;
  return pipeline;
}

ComPtr<ID3D12PipelineState> D3D12PipelineCache::Compute(
    const Root &root, D3D12_SHADER_BYTECODE shader) {
  ValidateRoot(root);
  std::vector<std::uint8_t> key;
  AppendBytes(key, root->serialized_.data(), root->serialized_.size());
  AppendBytes(key, shader.pShaderBytecode, shader.BytecodeLength);
  if (const auto found = compute_.find(key); found != compute_.end()) {
    ++stats_.cache_hits;
    return found->second;
  }
  if (compute_.size() >= EntryLimit) {
    winrt::throw_hresult(E_OUTOFMEMORY);
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root->Get();
  desc.CS = shader;
  ComPtr<ID3D12PipelineState> pipeline;
  winrt::check_hresult(device_->CreateComputePipelineState(
      &desc, IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf())));
  compute_.emplace(std::move(key), pipeline);
  ++stats_.compute_creations;
  return pipeline;
}
