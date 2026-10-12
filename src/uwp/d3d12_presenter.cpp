#include "uwp/d3d12_presenter.h"

#include <winrt/base.h>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace Core::Uwp {
void D3D12Presenter::CheckEnvironment() {
    D3D12DeviceContext device;
    device.Initialize();
    D3D12ResourceAllocator allocator;
    allocator.Initialize(device.Device(), 1024 * 1024);
    D3D12Resource upload;
    D3D12Resource buffer;
    D3D12Resource readback;
    constexpr std::array<uint32_t, 4> expected{0x10203040, 0x50607080, 0x90abcdef, 0x12345678};
    winrt::check_hresult(allocator.CreateBuffer(sizeof(expected), D3D12_HEAP_TYPE_UPLOAD,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, upload));
    winrt::check_hresult(allocator.CreateBuffer(sizeof(expected), D3D12_HEAP_TYPE_DEFAULT,
                                                D3D12_RESOURCE_STATE_COMMON, buffer));
    winrt::check_hresult(allocator.CreateBuffer(sizeof(expected), D3D12_HEAP_TYPE_READBACK,
                                                D3D12_RESOURCE_STATE_COPY_DEST, readback));
    void* mapped{};
    const D3D12_RANGE empty{0, 0};
    const D3D12_RANGE contents{0, sizeof(expected)};
    winrt::check_hresult(upload->Map(0, &empty, &mapped));
    std::memcpy(mapped, expected.data(), sizeof(expected));
    upload->Unmap(0, &contents);
    ComPtr<ID3D12CommandAllocator> command_allocator;
    ComPtr<ID3D12GraphicsCommandList> command_list;
    device.CreateDirectCommands(command_allocator, command_list);
    D3D12TransferStats stats;
    D3D12CommandEncoder encoder{command_list.Get(), stats};
    encoder.Track(upload.Get(), D3D12_RESOURCE_STATE_GENERIC_READ);
    encoder.Track(buffer.Get(), D3D12_RESOURCE_STATE_COMMON);
    encoder.Track(readback.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    encoder.CopyBuffer(buffer.Get(), 0, upload.Get(), 0, sizeof(expected));
    encoder.CopyBuffer(readback.Get(), 0, buffer.Get(), 0, sizeof(expected));
    winrt::check_hresult(command_list->Close());
    device.Wait(device.Submit(command_list.Get()));
    winrt::check_hresult(readback->Map(0, &contents, &mapped));
    const bool matches = std::memcmp(mapped, expected.data(), sizeof(expected)) == 0;
    readback->Unmap(0, &empty);
    if (!matches) {
        winrt::throw_hresult(E_FAIL);
    }
}

void D3D12Presenter::Initialize(const ShadPS4Config& config) {
    context.Initialize();
    resources.Initialize(context.Device(), 512ULL * 1024 * 1024);
    descriptors.Initialize(context.Device(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, false);
    descriptors.Allocate(2);
    width = config.width;
    height = config.height;
    ComPtr<IDXGIFactory2> factory;
    winrt::check_hresult(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width;
    description.Height = height;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain1> chain;
    winrt::check_hresult(factory->CreateSwapChainForComposition(context.DirectQueue(), &description,
                                                                nullptr, &chain));
    winrt::check_hresult(chain.As(&swapchain));
    GetBuffers();
    context.CreateDirectCommands(commands_allocator, commands);
    winrt::check_hresult(commands->Close());
    winrt::check_hresult(config.attach_swapchain(config.attach_context, chain.Get()));
}

void D3D12Presenter::GetBuffers() {
    for (uint32_t index = 0; index < buffers.size(); ++index) {
        winrt::check_hresult(swapchain->GetBuffer(index, IID_PPV_ARGS(&buffers[index])));
        context.Device()->CreateRenderTargetView(buffers[index].Get(), nullptr,
                                                 descriptors.Cpu(index));
    }
}

void D3D12Presenter::Render() {
    winrt::check_hresult(commands_allocator->Reset());
    winrt::check_hresult(commands->Reset(commands_allocator.Get(), nullptr));
    const auto index = swapchain->GetCurrentBackBufferIndex();
    D3D12CommandEncoder encoder{commands.Get(), transfers};
    encoder.Track(buffers[index].Get(), D3D12_RESOURCE_STATE_PRESENT);
    encoder.Transition(buffers[index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    encoder.ClearColor(buffers[index].Get(), descriptors.Cpu(index), {0.025f, 0.035f, 0.05f, 1.0f});
    encoder.Transition(buffers[index].Get(), D3D12_RESOURCE_STATE_PRESENT);
    winrt::check_hresult(commands->Close());
    const auto ticket = context.Submit(commands.Get());
    winrt::check_hresult(swapchain->Present(1, 0));
    context.Wait(ticket);
}

void D3D12Presenter::Resize(uint32_t new_width, uint32_t new_height) {
    if (!swapchain || !new_width || !new_height || (new_width == width && new_height == height)) {
        return;
    }
    context.WaitForGpu();
    for (auto& buffer : buffers) {
        buffer.Reset();
    }
    winrt::check_hresult(
        swapchain->ResizeBuffers(2, new_width, new_height, DXGI_FORMAT_B8G8R8A8_UNORM, 0));
    width = new_width;
    height = new_height;
    GetBuffers();
}
} // namespace Core::Uwp
