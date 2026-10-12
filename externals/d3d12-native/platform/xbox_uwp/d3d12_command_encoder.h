// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <map>

struct D3D12TransferStats {
  UINT64 transitions{};
  UINT64 redundant_transitions{};
  UINT64 uav_barriers{};
  UINT64 buffer_copies{};
  UINT64 texture_copies{};
  UINT64 color_clears{};
  UINT64 depth_clears{};
  UINT64 resolves{};
  UINT64 rejected_requests{};
};

// Recording-local explicit states on a DIRECT list. Initial states are supplied
// by the caller. State() describes this recording; StateAfterExecution() also
// handles buffer/simultaneous-access decay at an ExecuteCommandLists boundary.
// Do not mix raw barriers with this encoder or reuse it after list Reset.
// Keep all referenced resources/descriptors alive through the submission fence.
class D3D12CommandEncoder final {
public:
  D3D12CommandEncoder(ID3D12GraphicsCommandList *commands, D3D12TransferStats &stats);
  D3D12CommandEncoder(const D3D12CommandEncoder &) = delete;
  D3D12CommandEncoder &operator=(const D3D12CommandEncoder &) = delete;
  void Track(ID3D12Resource *resource, D3D12_RESOURCE_STATES initial_state);
  [[nodiscard]] D3D12_RESOURCE_STATES State(ID3D12Resource *resource);
  [[nodiscard]] D3D12_RESOURCE_STATES StateAfterExecution(ID3D12Resource *resource);
  void Transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES state);
  void UavBarrier(ID3D12Resource *resource);
  void CopyBuffer(ID3D12Resource *destination, UINT64 destination_offset,
                  ID3D12Resource *source, UINT64 source_offset, UINT64 bytes);
  // Full subresource zero only: 2D, one mip/layer, single sample/plane.
  // Footprint must match GetCopyableFootprints and fit inside the buffer.
  void CopyBufferToTexture(ID3D12Resource *texture, ID3D12Resource *buffer,
                           const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint);
  void CopyTextureToBuffer(ID3D12Resource *buffer, ID3D12Resource *texture,
                           const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint);
  void ClearColor(ID3D12Resource *resource, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                   const std::array<float, 4> &color);
  void ClearDepth(ID3D12Resource *resource, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                   float depth);
  // Typed identical formats/dimensions, one mip/layer; MSAA -> single sample.
  void Resolve(ID3D12Resource *destination, ID3D12Resource *source);

private:
  struct Resource {
    Microsoft::WRL::ComPtr<ID3D12Resource> owner;
    D3D12_RESOURCE_DESC description{};
    D3D12_HEAP_TYPE heap{};
    D3D12_RESOURCE_STATES state{};
  };
  [[noreturn]] void Reject();
  Resource &Find(ID3D12Resource *resource);
  void ValidateState(const Resource &resource, D3D12_RESOURCE_STATES state);
  void ValidateFootprint(const Resource &texture, const Resource &buffer,
                         const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint);
  void CopyTexture(ID3D12Resource *texture, ID3D12Resource *buffer,
                    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint,
                    bool to_texture);
  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  ID3D12GraphicsCommandList *commands_{};
  D3D12TransferStats &stats_;
  std::map<ID3D12Resource *, Resource> resources_;
};
