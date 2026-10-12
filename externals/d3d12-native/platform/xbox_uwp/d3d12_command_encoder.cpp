// SPDX-License-Identifier: GPL-2.0-or-later
#include "d3d12_command_encoder.h"

#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
bool SimpleTexture(const D3D12_RESOURCE_DESC &desc) {
  return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
         desc.MipLevels == 1 && desc.DepthOrArraySize == 1;
}
} // namespace

D3D12CommandEncoder::D3D12CommandEncoder(ID3D12GraphicsCommandList *commands,
                                         D3D12TransferStats &stats)
    : commands_(commands), stats_(stats) {
  if (!commands || commands->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
    Reject();
  }
  winrt::check_hresult(commands->GetDevice(IID_PPV_ARGS(device_.ReleaseAndGetAddressOf())));
}

[[noreturn]] void D3D12CommandEncoder::Reject() {
  ++stats_.rejected_requests;
  winrt::throw_hresult(E_INVALIDARG);
}

D3D12CommandEncoder::Resource &D3D12CommandEncoder::Find(ID3D12Resource *resource) {
  const auto found = resources_.find(resource);
  if (found == resources_.end()) {
    Reject();
  }
  return found->second;
}

void D3D12CommandEncoder::ValidateState(const Resource &resource,
                                        D3D12_RESOURCE_STATES state) {
  constexpr UINT ReadStates = D3D12_RESOURCE_STATE_GENERIC_READ |
                              D3D12_RESOURCE_STATE_DEPTH_READ |
                              D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
  constexpr UINT WriteStates = D3D12_RESOURCE_STATE_RENDER_TARGET |
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
                               D3D12_RESOURCE_STATE_DEPTH_WRITE |
                               D3D12_RESOURCE_STATE_STREAM_OUT |
                               D3D12_RESOURCE_STATE_COPY_DEST |
                               D3D12_RESOURCE_STATE_RESOLVE_DEST;
  const UINT bits = static_cast<UINT>(state);
  const UINT writes = bits & WriteStates;
  if ((bits & ~(ReadStates | WriteStates)) != 0 ||
      (writes != 0 && ((writes & (writes - 1)) != 0 || (bits & ReadStates) != 0)) ||
      (resource.heap == D3D12_HEAP_TYPE_UPLOAD && state != D3D12_RESOURCE_STATE_GENERIC_READ) ||
      (resource.heap == D3D12_HEAP_TYPE_READBACK && state != D3D12_RESOURCE_STATE_COPY_DEST)) {
    Reject();
  }
  const auto &desc = resource.description;
  if (((bits & D3D12_RESOURCE_STATE_RENDER_TARGET) != 0 &&
       (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0) ||
      ((bits & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0 &&
       (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) ||
      ((bits & (D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_DEPTH_WRITE)) != 0 &&
       (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0) ||
      ((bits & (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) != 0 &&
       (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) ||
      (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
       (bits & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_DEPTH_READ |
                D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_RESOLVE_SOURCE |
                D3D12_RESOURCE_STATE_RESOLVE_DEST)) != 0) ||
      (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
       (bits & (D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
                D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_STREAM_OUT |
                D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)) != 0) ||
      ((bits & D3D12_RESOURCE_STATE_RESOLVE_SOURCE) != 0 && desc.SampleDesc.Count <= 1) ||
      ((bits & D3D12_RESOURCE_STATE_RESOLVE_DEST) != 0 && desc.SampleDesc.Count != 1)) {
    Reject();
  }
}

void D3D12CommandEncoder::Track(ID3D12Resource *resource,
                                D3D12_RESOURCE_STATES initial_state) {
  if (!resource) {
    Reject();
  }
  if (const auto found = resources_.find(resource); found != resources_.end()) {
    if (found->second.state != initial_state) {
      Reject();
    }
    return;
  }
  Resource tracked;
  tracked.owner = resource;
  tracked.description = resource->GetDesc();
  Microsoft::WRL::ComPtr<ID3D12Device> owner_device;
  winrt::check_hresult(resource->GetDevice(IID_PPV_ARGS(owner_device.ReleaseAndGetAddressOf())));
  if (owner_device.Get() != device_.Get()) {
    Reject();
  }
  D3D12_HEAP_PROPERTIES heap{};
  winrt::check_hresult(resource->GetHeapProperties(&heap, nullptr));
  tracked.heap = heap.Type;
  if (tracked.heap != D3D12_HEAP_TYPE_DEFAULT && tracked.heap != D3D12_HEAP_TYPE_UPLOAD &&
      tracked.heap != D3D12_HEAP_TYPE_READBACK) {
    Reject();
  }
  ValidateState(tracked, initial_state);
  tracked.state = initial_state;
  resources_.emplace(resource, std::move(tracked));
}

D3D12_RESOURCE_STATES D3D12CommandEncoder::State(ID3D12Resource *resource) {
  return Find(resource).state;
}

D3D12_RESOURCE_STATES D3D12CommandEncoder::StateAfterExecution(ID3D12Resource *resource) {
  const auto &tracked = Find(resource);
  if (tracked.heap == D3D12_HEAP_TYPE_DEFAULT &&
      (tracked.description.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ||
       (tracked.description.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0)) {
    return D3D12_RESOURCE_STATE_COMMON;
  }
  return tracked.state;
}

void D3D12CommandEncoder::Transition(ID3D12Resource *resource,
                                     D3D12_RESOURCE_STATES state) {
  auto &tracked = Find(resource);
  ValidateState(tracked, state);
  if (tracked.state == state) {
    ++stats_.redundant_transitions;
    return;
  }
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = tracked.state;
  barrier.Transition.StateAfter = state;
  commands_->ResourceBarrier(1, &barrier);
  tracked.state = state;
  ++stats_.transitions;
}

void D3D12CommandEncoder::UavBarrier(ID3D12Resource *resource) {
  if (Find(resource).state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    Reject();
  }
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.UAV.pResource = resource;
  commands_->ResourceBarrier(1, &barrier);
  ++stats_.uav_barriers;
}

void D3D12CommandEncoder::CopyBuffer(ID3D12Resource *destination,
                                     UINT64 destination_offset,
                                     ID3D12Resource *source, UINT64 source_offset,
                                     UINT64 bytes) {
  auto &dst = Find(destination);
  auto &src = Find(source);
  if (destination == source || bytes == 0 ||
      dst.description.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
      src.description.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
      destination_offset > dst.description.Width || bytes > dst.description.Width - destination_offset ||
      source_offset > src.description.Width || bytes > src.description.Width - source_offset ||
      dst.heap == D3D12_HEAP_TYPE_UPLOAD || src.heap == D3D12_HEAP_TYPE_READBACK) {
    Reject();
  }
  if (src.heap != D3D12_HEAP_TYPE_UPLOAD) {
    Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE);
  }
  Transition(destination, D3D12_RESOURCE_STATE_COPY_DEST);
  commands_->CopyBufferRegion(destination, destination_offset, source, source_offset, bytes);
  ++stats_.buffer_copies;
}

void D3D12CommandEncoder::ValidateFootprint(
    const Resource &texture, const Resource &buffer,
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint) {
  const auto &desc = texture.description;
  D3D12_FEATURE_DATA_FORMAT_INFO format_info{desc.Format, 0};
  if (!SimpleTexture(desc) || desc.SampleDesc.Count != 1 ||
      buffer.description.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
      footprint.Offset > buffer.description.Width ||
      footprint.Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT != 0 ||
      FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &format_info,
                                          sizeof(format_info))) || format_info.PlaneCount != 1) {
    Reject();
  }
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT expected{};
  UINT rows{};
  UINT64 row_bytes{};
  device_->GetCopyableFootprints(&desc, 0, 1, footprint.Offset, &expected,
                                 &rows, &row_bytes, nullptr);
  const auto &actual = footprint.Footprint;
  const auto &wanted = expected.Footprint;
  if (actual.Format != wanted.Format || actual.Width != wanted.Width ||
      actual.Height != wanted.Height || actual.Depth != wanted.Depth ||
      actual.RowPitch != wanted.RowPitch || footprint.Offset != expected.Offset ||
      rows == 0 || row_bytes > wanted.RowPitch || footprint.Offset > buffer.description.Width) {
    Reject();
  }
  const UINT64 row_offset = static_cast<UINT64>(rows - 1) * wanted.RowPitch;
  const UINT64 available = buffer.description.Width - footprint.Offset;
  if (row_offset > available || row_bytes > available - row_offset) {
    Reject();
  }
}

void D3D12CommandEncoder::CopyTexture(
    ID3D12Resource *texture, ID3D12Resource *buffer,
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint, bool to_texture) {
  auto &tex = Find(texture);
  auto &buf = Find(buffer);
  ValidateFootprint(tex, buf, footprint);
  if ((to_texture && buf.heap == D3D12_HEAP_TYPE_READBACK) ||
      (!to_texture && buf.heap == D3D12_HEAP_TYPE_UPLOAD)) {
    Reject();
  }
  Transition(texture, to_texture ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE);
  if (buf.heap != D3D12_HEAP_TYPE_UPLOAD) {
    Transition(buffer, to_texture ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST);
  }
  D3D12_TEXTURE_COPY_LOCATION image{};
  image.pResource = texture;
  image.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION placed{};
  placed.pResource = buffer;
  placed.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  placed.PlacedFootprint = footprint;
  commands_->CopyTextureRegion(to_texture ? &image : &placed, 0, 0, 0,
                                to_texture ? &placed : &image, nullptr);
  ++stats_.texture_copies;
}

void D3D12CommandEncoder::CopyBufferToTexture(
    ID3D12Resource *texture, ID3D12Resource *buffer,
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint) {
  CopyTexture(texture, buffer, footprint, true);
}

void D3D12CommandEncoder::CopyTextureToBuffer(
    ID3D12Resource *buffer, ID3D12Resource *texture,
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint) {
  CopyTexture(texture, buffer, footprint, false);
}

void D3D12CommandEncoder::ClearColor(ID3D12Resource *resource,
                                     D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                                     const std::array<float, 4> &color) {
  const auto &desc = Find(resource).description;
  if (rtv.ptr == 0 || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0 ||
      !std::all_of(color.begin(), color.end(), [](float value) { return std::isfinite(value); })) {
    Reject();
  }
  Transition(resource, D3D12_RESOURCE_STATE_RENDER_TARGET);
  commands_->ClearRenderTargetView(rtv, color.data(), 0, nullptr);
  ++stats_.color_clears;
}

void D3D12CommandEncoder::ClearDepth(ID3D12Resource *resource,
                                     D3D12_CPU_DESCRIPTOR_HANDLE dsv, float depth) {
  const auto &desc = Find(resource).description;
  if (dsv.ptr == 0 || !SimpleTexture(desc) || desc.Format != DXGI_FORMAT_D32_FLOAT ||
      (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0 ||
      !std::isfinite(depth) || depth < 0 || depth > 1) {
    Reject();
  }
  Transition(resource, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  commands_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, depth, 0, 0, nullptr);
  ++stats_.depth_clears;
}

void D3D12CommandEncoder::Resolve(ID3D12Resource *destination, ID3D12Resource *source) {
  const auto &dst = Find(destination).description;
  const auto &src = Find(source).description;
  D3D12_FEATURE_DATA_FORMAT_SUPPORT support{src.Format, {}, {}};
  if (destination == source || !SimpleTexture(src) || !SimpleTexture(dst) ||
      src.SampleDesc.Count <= 1 || dst.SampleDesc.Count != 1 ||
      src.Width != dst.Width || src.Height != dst.Height || src.Format != dst.Format ||
      (src.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 ||
      FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
      (support.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE) == 0) {
    Reject();
  }
  Transition(source, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
  Transition(destination, D3D12_RESOURCE_STATE_RESOLVE_DEST);
  commands_->ResolveSubresource(destination, 0, source, 0, src.Format);
  ++stats_.resolves;
}
