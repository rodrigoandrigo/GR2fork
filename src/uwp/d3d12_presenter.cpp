#include "d3d12_videocore_bridge.h"
#include "uwp/d3d12_presenter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <winrt/base.h>
#include "core/memory.h"
#include "video_core/texture_cache/cpu_tiling.h"

using Microsoft::WRL::ComPtr;

namespace Core::Uwp {
struct D3D12Presenter::Frame final : VideoCore::PresentationFrame {
    std::vector<u8> pixels;
    D3D12Resource upload;
    u32 width{};
    u32 height{};
    u64 charge{};
    std::shared_ptr<std::atomic<u64>> budget;
    ~Frame() override {
        if (budget)
            budget->fetch_sub(charge);
    }
};
void D3D12Presenter::RegisterVideoOutSurface(const Libraries::VideoOut::BufferAttributeGroup& group,
                                             VAddr address) {
    const auto& attr = group.attrib;
    if (!address || !attr.width || !attr.height || attr.width > 8192 || attr.height > 8192 ||
        attr.pitch_in_pixel < attr.width || attr.pitch_in_pixel > 16384)
        throw std::runtime_error("Invalid D3D12 VideoOut surface");
    if (attr.pixel_format != Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb &&
        attr.pixel_format != Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb &&
        attr.pixel_format != Libraries::VideoOut::PixelFormat::A2R10G10B10 &&
        attr.pixel_format != Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb)
        throw std::runtime_error(
            "D3D12 VideoOut HDR/float format requires native color conversion");
}
VideoCore::FrameHandle D3D12Presenter::PrepareFrame(
    const Libraries::VideoOut::BufferAttributeGroup& group, VAddr address) {
    RegisterVideoOutSurface(group, address);
    VideoCore::ImageInfo info{group, address};
    if (group.attrib.tiling_mode == Libraries::VideoOut::TilingMode::Linear) {
        info.pitch = group.attrib.pitch_in_pixel;
        info.UpdateSize();
    }
    if (info.guest_size > 64ULL * 1024 * 1024)
        throw std::runtime_error("D3D12 VideoOut surface exceeds staging budget");
    auto frame = std::make_shared<Frame>();
    const u64 charge = u64(group.attrib.width) * group.attrib.height * 8;
    constexpr u64 limit = 256ULL * 1024 * 1024;
    if (charge > limit)
        throw std::runtime_error("D3D12 VideoOut frame exceeds budget");
    if (frame_bytes->fetch_add(charge) > limit - charge) {
        frame_bytes->fetch_sub(charge);
        throw std::runtime_error("D3D12 VideoOut frame budget exhausted");
    }
    frame->charge = charge;
    frame->budget = frame_bytes;
    frame->width = group.attrib.width;
    frame->height = group.attrib.height;
    std::vector<u8> guest(info.guest_size), linear(info.guest_size);
    {
        auto* memory = Core::Memory::Instance();
        Core::MemoryManager::GuestCopyScope scope{memory};
        memory->CopySparseMemory(address, guest.data(), guest.size());
    }
    VideoCore::ConvertTiling(guest, linear, info, false);
    frame->pixels.resize(u64(frame->width) * frame->height * 4);
    const float adjustment = gamma.load();
    const auto encode = [&](float value, bool srgb) -> u8 {
        if (srgb)
            value = value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
        value = value > 0.0031308f ? 1.055f * std::pow(value, 1.0f / (3.4f - adjustment)) - 0.055f
                                   : 12.92f * value / adjustment;
        return static_cast<u8>(std::clamp(value * 255.0f + 0.5f, 0.0f, 255.0f));
    };
    std::array<u8, 256> srgb8;
    std::array<u8, 1024> encoded10;
    for (u32 i = 0; i < srgb8.size(); ++i)
        srgb8[i] = encode(float(i) / 255, true);
    for (u32 i = 0; i < encoded10.size(); ++i)
        encoded10[i] =
            encode(float(i) / 1023,
                   group.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb);
    using Format = Libraries::VideoOut::PixelFormat;
    for (u32 y = 0; y < frame->height; ++y)
        for (u32 x = 0; x < frame->width; ++x) {
            u32 pixel;
            const u64 source = (u64(y) * info.mips_layout[0].pitch + x) * 4;
            if (source + 4 > linear.size())
                throw std::runtime_error("D3D12 VideoOut row exceeds guest backing");
            std::memcpy(&pixel, linear.data() + source, 4);
            auto* output = frame->pixels.data() + (u64(y) * frame->width + x) * 4;
            const auto format = group.attrib.pixel_format;
            if (format == Format::A8R8G8B8Srgb || format == Format::A8B8G8R8Srgb) {
                output[0] = srgb8[(pixel >> (format == Format::A8R8G8B8Srgb ? 0 : 16)) & 255];
                output[1] = srgb8[(pixel >> 8) & 255];
                output[2] = srgb8[(pixel >> (format == Format::A8R8G8B8Srgb ? 16 : 0)) & 255];
                output[3] = pixel >> 24;
            } else {
                for (u32 channel = 0; channel < 3; ++channel)
                    output[channel] = encoded10[(pixel >> (channel * 10)) & 1023];
                output[3] = (pixel >> 30) * 85;
            }
        }
    return frame;
}
VideoCore::FrameHandle D3D12Presenter::PrepareBlankFrame(bool) {
    return std::make_shared<Frame>();
}
VideoCore::FrameHandle D3D12Presenter::PrepareLastFrame() {
    std::scoped_lock lock{mutex};
    return last_frame;
}
void D3D12Presenter::Present(const VideoCore::FrameHandle& frame, bool, bool) {
    const auto native = std::dynamic_pointer_cast<Frame>(frame);
    if (!native)
        throw std::runtime_error("VideoOut frame belongs to another presentation backend");
    std::scoped_lock lock{mutex};
    last_frame = native;
    RenderFrame();
}
void D3D12Presenter::SetHDR(bool enabled) {
    if (enabled)
        throw std::runtime_error("D3D12 VideoOut HDR is not implemented");
}
void D3D12Presenter::SetGamma(float value) {
    if (!std::isfinite(value) || value < 0.1f || value > 2.0f)
        throw std::runtime_error("Invalid VideoOut gamma");
    gamma.store(value);
}
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
    D3D12Resource sink_buffer;
    winrt::check_hresult(allocator.CreateBuffer(sizeof(expected), D3D12_HEAP_TYPE_DEFAULT,
                                                D3D12_RESOURCE_STATE_COMMON, sink_buffer));
    D3D12VideoCoreBridge bridge{device, allocator, stats};
    bridge.RegisterBuffer(0x1000, sink_buffer.Get());
    VideoCore::GpuCommandSink& sink = bridge;
    sink.FillBuffer(0x1000, sizeof(expected), expected.front(), false);
    std::array<uint32_t, 4> output{};
    bridge.DownloadBuffer(0x1000,
                          std::span{reinterpret_cast<uint8_t*>(output.data()), sizeof(output)});
    if (!std::ranges::all_of(output, [&](uint32_t value) { return value == expected.front(); }))
        winrt::throw_hresult(E_FAIL);
}

void D3D12Presenter::Initialize(const ShadPS4Config& config) {
    context.Initialize();
    resources.Initialize(context.Device(), 512ULL * 1024 * 1024);
    descriptors.Initialize(context.Device(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, false);
    static_cast<void>(descriptors.Allocate(2));
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
    std::scoped_lock lock{mutex};
    RenderFrame();
}
void D3D12Presenter::RenderFrame() {
    if (last_frame && last_frame->width && last_frame->height)
        ResizeBuffers(last_frame->width, last_frame->height);
    winrt::check_hresult(commands_allocator->Reset());
    winrt::check_hresult(commands->Reset(commands_allocator.Get(), nullptr));
    const auto index = swapchain->GetCurrentBackBufferIndex();
    D3D12CommandEncoder encoder{commands.Get(), transfers};
    encoder.Track(buffers[index].Get(), D3D12_RESOURCE_STATE_PRESENT);
    if (last_frame && !last_frame->pixels.empty()) {
        const auto description = buffers[index]->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        u64 bytes;
        context.Device()->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr,
                                                &bytes);
        if (!last_frame->upload.Get()) {
            winrt::check_hresult(resources.CreateBuffer(bytes, D3D12_HEAP_TYPE_UPLOAD,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ,
                                                        last_frame->upload));
            void* mapped;
            const D3D12_RANGE empty{0, 0};
            winrt::check_hresult(last_frame->upload->Map(0, &empty, &mapped));
            for (u32 row = 0; row < height; ++row)
                std::memcpy(static_cast<u8*>(mapped) + row * footprint.Footprint.RowPitch,
                            last_frame->pixels.data() + u64(row) * width * 4, u64(width) * 4);
            const D3D12_RANGE contents{0, static_cast<SIZE_T>(bytes)};
            last_frame->upload->Unmap(0, &contents);
        }
        encoder.Transition(buffers[index].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
        source.pResource = last_frame->upload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;
        destination.pResource = buffers[index].Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    } else {
        encoder.Transition(buffers[index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        encoder.ClearColor(buffers[index].Get(), descriptors.Cpu(index),
                           {0.025f, 0.035f, 0.05f, 1.0f});
    }
    encoder.Transition(buffers[index].Get(), D3D12_RESOURCE_STATE_PRESENT);
    winrt::check_hresult(commands->Close());
    const auto ticket = context.Submit(commands.Get());
    winrt::check_hresult(swapchain->Present(1, 0));
    context.Wait(ticket);
}

void D3D12Presenter::Resize(uint32_t new_width, uint32_t new_height) {
    std::scoped_lock lock{mutex};
    if (last_frame && last_frame->width && last_frame->height)
        return;
    ResizeBuffers(new_width, new_height);
}
void D3D12Presenter::ResizeBuffers(uint32_t new_width, uint32_t new_height) {
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
