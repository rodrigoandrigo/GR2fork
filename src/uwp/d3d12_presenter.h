#pragma once

#include <array>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include "d3d12_command_encoder.h"
#include "d3d12_device_context.h"
#include "d3d12_pipeline_cache.h"
#include "d3d12_resource_allocator.h"
#include "uwp/shadps4_api.h"

namespace Core::Uwp {
class D3D12Presenter {
public:
    static void CheckEnvironment();
    void Initialize(const ShadPS4Config& config);
    void Render();
    void Resize(uint32_t width, uint32_t height);

private:
    void GetBuffers();
    D3D12DeviceContext context;
    D3D12ResourceAllocator resources;
    D3D12DescriptorArena descriptors;
    D3D12TransferStats transfers;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commands_allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> buffers;
    uint32_t width{};
    uint32_t height{};
};
} // namespace Core::Uwp
