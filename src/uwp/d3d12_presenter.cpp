#include "d3d12_videocore_bridge.h"
#include "uwp/d3d12_presenter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <bit>
#include <half.hpp>
#include "core/emulator_settings.h"
#include <stdexcept>
#include <thread>
#include <chrono>
#include <winrt/base.h>
#include "core/memory.h"
#include "common/logging/log.h"
#include "uwp/resident_conversion.h"
#include "uwp/session.h"
#include "video_core/renderer_d3d12/profile.h"
#include "video_core/renderer_d3d12/temporal_jitter.h"
#include "video_core/texture_cache/cpu_tiling.h"

using Microsoft::WRL::ComPtr;

namespace Core::Uwp {
struct D3D12Presenter::Frame final : VideoCore::PresentationFrame {
    std::vector<u8> pixels;
    D3D12Resource upload, spatial_input;
    std::shared_ptr<VideoCore::ResidentFrame> resident;
    ComPtr<ID3D12Resource> source, converted;
    D3D12::TemporalJitter::Sample jitter;
    u32 width{};
    u32 height{};
    Libraries::VideoOut::PixelFormat format{};
    float gamma{1.0f};
    bool upload_hdr{};
    float upload_sdr_white{};
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
        attr.pixel_format != Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb &&
        attr.pixel_format != Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq &&
        attr.pixel_format != Libraries::VideoOut::PixelFormat::A16R16G16B16Float)
        throw std::runtime_error(
            "Unsupported D3D12 VideoOut pixel format");
}
VideoCore::FrameHandle D3D12Presenter::PrepareFrame(
    const Libraries::VideoOut::BufferAttributeGroup& group, VAddr address) {
    RegisterVideoOutSurface(group, address);
    const bool first = !prepared_frame.exchange(true);
    if (first)
        LOG_INFO(Core, "[UWP stage] first VideoOut frame capture");
    VideoCore::ImageInfo info{group, address};
    if (group.attrib.tiling_mode == Libraries::VideoOut::TilingMode::Linear) {
        info.pitch = group.attrib.pitch_in_pixel;
        info.UpdateSize();
    }
    if (info.guest_size > 64ULL * 1024 * 1024)
        throw std::runtime_error("D3D12 VideoOut surface exceeds staging budget");
    auto frame = std::make_shared<Frame>();
    const u32 pixel_bytes = info.num_bits / 8;
    const auto allocation = [](u64 bytes) { return (bytes + 65535) & ~u64{65535}; };
    const u64 source_bytes =
        ((u64(info.pitch) * pixel_bytes + 255) & ~u64{255}) * group.attrib.height;
    const u64 output_bytes =
        ((u64(group.attrib.width) * 8 + 255) & ~u64{255}) * group.attrib.height;
    const u64 charge = allocation(source_bytes) + allocation(output_bytes) + 65536;
    constexpr u64 limit = 256ULL * 1024 * 1024;
    if (charge > limit)
        throw std::runtime_error("D3D12 VideoOut frame exceeds budget");
    auto used = frame_bytes->load();
    for (;;) {
        if (used <= limit - charge && frame_bytes->compare_exchange_weak(used, used + charge))
            break;
        if (StopRequested())
            return PrepareBlankFrame(false);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        used = frame_bytes->load();
    }
    frame->charge = charge;
    frame->budget = frame_bytes;
    frame->width = group.attrib.width;
    frame->height = group.attrib.height;
    frame->format = group.attrib.pixel_format;
    frame->gamma = gamma.load();
    D3D12::ProfileScope prepare_timing{presenter_profile, "presenter_prepare"};
    frame->resident = Core::Memory::Instance()->CapturePresentation(info);
    if (frame->resident) {
        if (presenter_profile.enabled)
            presenter_profile.Record("presenter_resident_bytes", 128, 0, frame->resident->bytes);
        if (first)
            LOG_INFO(Core, "[UWP stage] first resident VideoOut snapshot ready");
        frame->jitter = D3D12::NativeTemporalJitter().CompleteFrame();
        return frame;
    }
    if (presenter_profile.enabled)
        presenter_profile.Record("presenter_cpu_bytes", 128, 0, info.guest_size);
    std::vector<u8> guest(info.guest_size), linear(info.guest_size);
    {
        auto* memory = Core::Memory::Instance();
        Core::MemoryManager::GuestCopyScope scope{memory};
        memory->CopySparseMemory(address, guest.data(), guest.size());
    }
    VideoCore::ConvertTiling(guest, linear, info, false);
    frame->pixels.resize(u64(frame->width) * frame->height * pixel_bytes);
    for (u32 row = 0; row < frame->height; ++row) {
        const u64 source = u64(row) * info.mips_layout[0].pitch * pixel_bytes;
        const u64 bytes = u64(frame->width) * pixel_bytes;
        if (source + bytes > linear.size())
            throw std::runtime_error("D3D12 VideoOut row exceeds guest backing");
        std::memcpy(frame->pixels.data() + u64(row) * bytes, linear.data() + source, bytes);
    }
    if (first)
        LOG_INFO(Core, "[UWP stage] first CPU VideoOut snapshot ready");
    frame->jitter = D3D12::NativeTemporalJitter().CompleteFrame();
    return frame;
}
std::array<float, 1024> D3D12Presenter::ColorComponents(const Frame& frame,
                                                        bool floating_point) const {
    using Format = Libraries::VideoOut::PixelFormat;
    const bool srgb = frame.format == Format::A8R8G8B8Srgb ||
                      frame.format == Format::A8B8G8R8Srgb ||
                      frame.format == Format::A2R10G10B10Srgb;
    const bool pq = frame.format == Format::A2R10G10B10Bt2020Pq ||
                    (hdr_enabled && (frame.format == Format::A2R10G10B10 ||
                                     frame.format == Format::A2R10G10B10Srgb));
    const auto degamma = [](float value) {
        return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
    };
    const auto encode = [&](float value) {
        return value > 0.0031308f ? 1.055f * std::pow(value, 1.0f / (3.4f - frame.gamma)) - 0.055f
                                  : 12.92f * value / frame.gamma;
    };
    const auto decode_pq = [](float value) {
        constexpr float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 32.0f;
        constexpr float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 128.0f;
        constexpr float c3 = 2392.0f / 128.0f;
        const float p = std::pow(std::clamp(value, 0.0f, 1.0f), 1.0f / m2);
        return 125.0f * std::pow(std::max(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
    };
    const bool eight_bit =
        frame.format == Format::A8R8G8B8Srgb || frame.format == Format::A8B8G8R8Srgb;
    const u32 maximum = eight_bit ? 255 : 1023;
    std::array<float, 1024> components{};
    if (frame.format == Format::A16R16G16B16Float)
        return components;
    for (u32 i = 0; i <= maximum; ++i) {
        float value = float(i) / maximum;
        if (pq) {
            value = decode_pq(value);
        } else {
            if (srgb)
                value = degamma(value);
            if (!floating_point || (!hdr_enabled && frame.gamma != 1.0f))
                value = floating_point ? degamma(encode(value)) : encode(value);
        }
        components[i] = value;
    }
    return components;
}
std::vector<u8> D3D12Presenter::ConvertPixels(const Frame& frame, bool floating_point) const {
    using Format = Libraries::VideoOut::PixelFormat;
    const bool fp16 = frame.format == Format::A16R16G16B16Float;
    const bool pq = frame.format == Format::A2R10G10B10Bt2020Pq ||
                    (hdr_enabled && (frame.format == Format::A2R10G10B10 ||
                                     frame.format == Format::A2R10G10B10Srgb));
    const auto degamma = [](float value) {
        return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
    };
    const auto encode = [&](float value) {
        return value > 0.0031308f ? 1.055f * std::pow(value, 1.0f / (3.4f - frame.gamma)) - 0.055f
                                  : 12.92f * value / frame.gamma;
    };
    const u32 maximum = frame.format == Format::A8R8G8B8Srgb ||
                                frame.format == Format::A8B8G8R8Srgb ? 255 : 1023;
    const auto components = ColorComponents(frame, floating_point);
    const u32 input_bytes = fp16 ? 8 : 4, output_bytes = floating_point ? 8 : 4;
    std::vector<u8> pixels(u64(frame.width) * frame.height * output_bytes);
    for (u64 i = 0; i < u64(frame.width) * frame.height; ++i) {
        std::array<float, 4> rgba{};
        const auto* input = frame.pixels.data() + i * input_bytes;
        if (fp16) {
            std::array<u16, 4> bgra;
            std::memcpy(bgra.data(), input, sizeof(bgra));
            for (u32 c = 0; c < 4; ++c)
                rgba[c] = static_cast<float>(std::bit_cast<half_float::half>(bgra[c == 0   ? 2
                                                                                  : c == 2 ? 0
                                                                                           : c]));
        } else {
            u32 value;
            std::memcpy(&value, input, sizeof(value));
            if (frame.format == Format::A8R8G8B8Srgb || frame.format == Format::A8B8G8R8Srgb) {
                rgba = {float((value >> 16) & 255) / 255, float((value >> 8) & 255) / 255,
                        float(value & 255) / 255, float(value >> 24) / 255};
                if (frame.format == Format::A8B8G8R8Srgb)
                    std::swap(rgba[0], rgba[2]);
            } else {
                rgba = {float((value >> 20) & 1023) / 1023, float((value >> 10) & 1023) / 1023,
                        float(value & 1023) / 1023, float(value >> 30) / 3};
            }
        }
        for (auto& value : rgba)
            if (!std::isfinite(value))
                value = 0;
        if (!fp16)
            for (u32 c = 0; c < 3; ++c)
                rgba[c] = components[static_cast<u32>(rgba[c] * maximum + 0.5f)];
        if (pq) {
            const float r = rgba[0], g = rgba[1], b = rgba[2];
            rgba[0] = 1.660491f * r - 0.587641f * g - 0.072850f * b;
            rgba[1] = -0.124550f * r + 1.132900f * g - 0.008349f * b;
            rgba[2] = -0.018151f * r - 0.100579f * g + 1.118730f * b;
        } else if (fp16) {
            for (u32 c = 0; c < 3; ++c) {
                if (!floating_point || (!hdr_enabled && frame.gamma != 1.0f))
                    rgba[c] = floating_point ? degamma(encode(rgba[c])) : encode(rgba[c]);
            }
        }
        if (fp16 && !hdr_enabled)
            for (u32 c = 0; c < 3; ++c)
                rgba[c] *= sdr_white_nits / 80.0f;
        auto* output = pixels.data() + i * output_bytes;
        if (floating_point) {
            std::array<u16, 4> packed;
            for (u32 c = 0; c < 4; ++c)
                packed[c] = std::bit_cast<u16>(
                    half_float::half_cast<half_float::half, std::round_to_nearest>(
                        std::clamp(rgba[c], -65504.0f, 65504.0f)));
            std::memcpy(output, packed.data(), sizeof(packed));
        } else {
            for (u32 c = 0; c < 4; ++c)
                output[c] = static_cast<u8>(std::clamp(rgba[c] *
                                                               255 +
                                                           0.5f,
                                                       0.0f, 255.0f));
        }
    }
    return pixels;
}
VideoCore::FrameHandle D3D12Presenter::PrepareBlankFrame(bool) {
    return std::make_shared<Frame>();
}
VideoCore::FrameHandle D3D12Presenter::PrepareLastFrame() {
    auto lock = ProfileSample(presenter_profile, "presenter_lock", 128,
                              [&] { return std::unique_lock{mutex}; });
    return last_frame;
}
void D3D12Presenter::Present(const VideoCore::FrameHandle& frame, bool, bool) {
    const auto native = std::dynamic_pointer_cast<Frame>(frame);
    if (!native)
        throw std::runtime_error("VideoOut frame belongs to another presentation backend");
    auto lock = ProfileSample(presenter_profile, "presenter_lock", 128,
                              [&] { return std::unique_lock{mutex}; });
    last_frame = native;
    const bool first = !presented_frame && native->width && native->height;
    if (first)
        LOG_INFO(Core, "[UWP stage] first native game presentation begin");
    RenderFrame();
    if (first) {
        presented_frame = true;
        LOG_INFO(Core, "[UWP stage] first native game presentation completed");
    }
}
bool D3D12Presenter::IsHDRSupported() const {
    return float_supported && hdr_display.load() && EmulatorSettings.IsHdrAllowed();
}
void D3D12Presenter::SetHDROutput(bool enabled, float white_nits) {
    auto lock = ProfileSample(presenter_profile, "presenter_lock", 128,
                              [&] { return std::unique_lock{mutex}; });
    hdr_display.store(enabled);
    sdr_white_nits = enabled && std::isfinite(white_nits) && white_nits > 0 ? white_nits : 80.0f;
}
void D3D12Presenter::SetHDR(bool enabled) {
    auto lock = ProfileSample(presenter_profile, "presenter_lock", 128,
                              [&] { return std::unique_lock{mutex}; });
    hdr_enabled = enabled;
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
    std::shared_ptr<VideoCore::ResidentFrame> resident;
    ComPtr<ID3D12Resource> source, converted;
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
    display_width = width;
    display_height = height;
    ComPtr<IDXGIFactory2> factory;
    winrt::check_hresult(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width;
    description.Height = height;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    format = description.Format;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain1> chain;
    winrt::check_hresult(factory->CreateSwapChainForComposition(context.DirectQueue(), &description,
                                                                nullptr, &chain));
    winrt::check_hresult(chain.As(&swapchain));
    UINT support{};
    float_supported = SUCCEEDED(swapchain->CheckColorSpaceSupport(
                          DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &support)) &&
                      (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0;
    winrt::check_hresult(swapchain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709));
    GetBuffers();
    for (auto& command_frame : command_frames) {
        context.CreateDirectCommands(command_frame.allocator, command_frame.commands);
        winrt::check_hresult(command_frame.commands->Close());
    }
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
    auto lock = ProfileSample(presenter_profile, "presenter_lock", 128,
                              [&] { return std::unique_lock{mutex}; });
    ProfileSample(presenter_profile, "presenter_render", 128, [&] { RenderFrame(); });
}
void D3D12Presenter::WaitForFrameUse(const Frame& frame) {
    for (auto& command_frame : command_frames) {
        if (command_frame.submitted_frame.get() != &frame || !command_frame.fence)
            continue;
        context.Wait(command_frame.fence);
        command_frame.submitted_frame.reset();
    }
}
void D3D12Presenter::ConvertResident(Frame& frame, CommandFrame& command_frame,
                                     ID3D12GraphicsCommandList* commands) {
    D3D12::ProfileScope timing{presenter_profile, "presenter_convert"};
    if (frame.converted && frame.upload_hdr == hdr_enabled &&
        frame.upload_sdr_white == sdr_white_nits)
        return;
    WaitForFrameUse(frame);
    if (!conversion_pipeline) {
        conversion_shaders.Initialize(context.Device());
        const auto binary =
            conversion_shaders.CompileHlslRaw(std::string(ResidentConversion), L"cs_6_0", {});
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER parameters[4]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[0].Descriptor.ShaderRegister = 0;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = {1, &range};
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[2].Constants = {0, 0, 8};
        parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[3].Descriptor.ShaderRegister = 1;
        D3D12_ROOT_SIGNATURE_DESC root_description{4, parameters};
        ComPtr<ID3DBlob> serialized, errors;
        winrt::check_hresult(D3D12SerializeRootSignature(
            &root_description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
        winrt::check_hresult(context.Device()->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&conversion_root)));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = conversion_root.Get();
        pipeline.CS = {binary->dxil.data(), binary->dxil.size()};
        winrt::check_hresult(context.Device()->CreateComputePipelineState(
            &pipeline, IID_PPV_ARGS(&conversion_pipeline)));
    }
    if (!command_frame.conversion_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        winrt::check_hresult(context.Device()->CreateDescriptorHeap(
            &heap, IID_PPV_ARGS(&command_frame.conversion_heap)));
    }
    if (!frame.source)
        winrt::check_hresult(context.Device()->OpenSharedHandle(frame.resident->handle.get(),
                                                                IID_PPV_ARGS(&frame.source)));
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = frame.width;
    description.Height = frame.height;
    description.DepthOrArraySize = description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    frame.converted.Reset();
    winrt::check_hresult(context.Device()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&frame.converted)));
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = format;
    view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    context.Device()->CreateUnorderedAccessView(
        frame.converted.Get(), nullptr, &view,
        command_frame.conversion_heap->GetCPUDescriptorHandleForHeapStart());
    using Format = Libraries::VideoOut::PixelFormat;
    const u32 mode = frame.format == Format::A8R8G8B8Srgb          ? 0
                     : frame.format == Format::A8B8G8R8Srgb        ? 1
                     : frame.format == Format::A2R10G10B10         ? 2
                     : frame.format == Format::A2R10G10B10Srgb     ? 3
                     : frame.format == Format::A2R10G10B10Bt2020Pq ? 4
                                                                   : 5;
    const std::array<u32, 8> constants{frame.width,
                                       frame.height,
                                       frame.resident->row_pitch,
                                       mode,
                                       u32(format == DXGI_FORMAT_R16G16B16A16_FLOAT),
                                       u32(hdr_enabled),
                                       std::bit_cast<u32>(frame.gamma),
                                       std::bit_cast<u32>(sdr_white_nits)};
    const auto components = ColorComponents(frame, format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    frame.upload.Reset();
    winrt::check_hresult(resources.CreateBuffer(sizeof(components), D3D12_HEAP_TYPE_UPLOAD,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, frame.upload));
    void* mapped{};
    const D3D12_RANGE empty{0, 0};
    winrt::check_hresult(frame.upload->Map(0, &empty, &mapped));
    std::memcpy(mapped, components.data(), sizeof(components));
    const D3D12_RANGE contents{0, sizeof(components)};
    frame.upload->Unmap(0, &contents);
    commands->SetComputeRootSignature(conversion_root.Get());
    commands->SetPipelineState(conversion_pipeline.Get());
    auto* descriptors = command_frame.conversion_heap.Get();
    commands->SetDescriptorHeaps(1, &descriptors);
    commands->SetComputeRootShaderResourceView(0, frame.source->GetGPUVirtualAddress());
    commands->SetComputeRootDescriptorTable(
        1, command_frame.conversion_heap->GetGPUDescriptorHandleForHeapStart());
    commands->SetComputeRoot32BitConstants(2, constants.size(), constants.data(), 0);
    commands->SetComputeRootShaderResourceView(3, frame.upload->GetGPUVirtualAddress());
    D3D12_RESOURCE_BARRIER source{};
    source.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    source.Transition = {frame.source.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                         D3D12_RESOURCE_STATE_COMMON,
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    commands->ResourceBarrier(1, &source);
    commands->Dispatch((frame.width + 7) / 8, (frame.height + 7) / 8, 1);
    std::swap(source.Transition.StateBefore, source.Transition.StateAfter);
    D3D12_RESOURCE_BARRIER output{};
    output.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    output.Transition = {frame.converted.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    const D3D12_RESOURCE_BARRIER barriers[]{source, output};
    commands->ResourceBarrier(2, barriers);
    frame.upload_hdr = hdr_enabled;
    frame.upload_sdr_white = sdr_white_nits;
}
void D3D12Presenter::RenderFrame() {
    presenter_profile.Frame("present_interval");
    D3D12::ProfileScope timing{presenter_profile, "present_frame"};
    const bool has_frame_size = last_frame && last_frame->width && last_frame->height;
    u32 output_width = has_frame_size ? last_frame->width : display_width;
    u32 output_height = has_frame_size ? last_frame->height : display_height;
    const bool floating_point = hdr_enabled ||
        (last_frame &&
         (last_frame->format == Libraries::VideoOut::PixelFormat::A16R16G16B16Float ||
          last_frame->format == Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq));
    bool upscale{};
    if (last_frame && output_width && output_height && !floating_point &&
        EmulatorSettings.IsFsrEnabled()) {
        const float ratio = std::min(float(display_width) / output_width,
                                     float(display_height) / output_height);
        if (ratio > 1.f) {
            const u32 target_width = std::max(output_width, u32(std::lround(output_width * ratio)));
            const u32 target_height = std::max(output_height, u32(std::lround(output_height * ratio)));
            if (D3D12::FsrPass::Supports(target_width, target_height)) {
                output_width = target_width;
                output_height = target_height;
                upscale = true;
            }
        }
    }
    ResizeBuffers(output_width, output_height,
                  floating_point ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM);
    const float scale = std::min(float(display_width) / width, float(display_height) / height);
    const DXGI_MATRIX_3X2_F transform{
        scale / composition_scale_x, 0, 0, scale / composition_scale_y,
        (display_width - width * scale) * 0.5f / composition_scale_x,
        (display_height - height * scale) * 0.5f / composition_scale_y};
    winrt::check_hresult(swapchain->SetMatrixTransform(&transform));
    const auto index = swapchain->GetCurrentBackBufferIndex();
    auto& command_frame = command_frames[index];
    if (command_frame.fence)
        ProfileSample(presenter_profile, "presenter_fence_wait", 128,
                      [&] { context.Wait(command_frame.fence); });
    command_frame.fence = 0;
    command_frame.submitted_frame.reset();
    if (!upscale)
        command_frame.fsr.ResetImages();
    winrt::check_hresult(command_frame.allocator->Reset());
    winrt::check_hresult(command_frame.commands->Reset(command_frame.allocator.Get(), nullptr));
    auto* commands = command_frame.commands.Get();
    D3D12CommandEncoder encoder{commands, transfers};
    encoder.Track(buffers[index].Get(), D3D12_RESOURCE_STATE_PRESENT);
    ID3D12Resource* upscale_source{};
    if (last_frame && last_frame->resident) {
        bool direct = !upscale && !floating_point && !hdr_enabled &&
                      last_frame->format == Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb &&
                      last_frame->gamma == 1.0f;
        if (direct) {
            const auto components = ColorComponents(*last_frame, false);
            for (u32 component = 0; component < 256; ++component)
                direct &= u32(std::clamp(components[component] * 255 + 0.5f, 0.0f, 255.0f)) ==
                          component;
        }
        encoder.Transition(buffers[index].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        if (direct) {
            if (!last_frame->source)
                winrt::check_hresult(context.Device()->OpenSharedHandle(
                    last_frame->resident->handle.get(), IID_PPV_ARGS(&last_frame->source)));
            encoder.Track(last_frame->source.Get(), D3D12_RESOURCE_STATE_COMMON);
            encoder.Transition(last_frame->source.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
            source.pResource = last_frame->source.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint.Footprint = {format, last_frame->width, last_frame->height, 1,
                                                last_frame->resident->row_pitch};
            destination.pResource = buffers[index].Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            encoder.Transition(last_frame->source.Get(), D3D12_RESOURCE_STATE_COMMON);
            if (presenter_profile.enabled)
                presenter_profile.Record("presenter_direct_bytes", 128, 0,
                                         u64(width) * height * 4);
        } else {
            ConvertResident(*last_frame, command_frame, commands);
            if (upscale)
                upscale_source = last_frame->converted.Get();
            else {
                if (presenter_profile.enabled)
                    presenter_profile.Record("presenter_copy_bytes", 128, 0,
                                             u64(width) * height * (floating_point ? 8 : 4));
                commands->CopyResource(buffers[index].Get(), last_frame->converted.Get());
            }
        }
    } else if (last_frame && !last_frame->pixels.empty()) {
        auto description = buffers[index]->GetDesc();
        description.Width = last_frame->width;
        description.Height = last_frame->height;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        u64 bytes;
        context.Device()->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr,
                                                &bytes);
        if (last_frame->upload.Get() && (last_frame->upload_hdr != hdr_enabled ||
                                         last_frame->upload_sdr_white != sdr_white_nits)) {
            WaitForFrameUse(*last_frame);
            last_frame->upload = {};
        }
        if (!last_frame->upload.Get()) {
            const auto pixels =
                ConvertPixels(*last_frame, format == DXGI_FORMAT_R16G16B16A16_FLOAT);
            const u32 pixel_bytes = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
            last_frame->upload_hdr = hdr_enabled;
            last_frame->upload_sdr_white = sdr_white_nits;
            winrt::check_hresult(resources.CreateBuffer(bytes, D3D12_HEAP_TYPE_UPLOAD,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ,
                                                        last_frame->upload));
            void* mapped;
            const D3D12_RANGE empty{0, 0};
            winrt::check_hresult(last_frame->upload->Map(0, &empty, &mapped));
            for (u32 row = 0; row < last_frame->height; ++row)
                std::memcpy(static_cast<u8*>(mapped) + row * footprint.Footprint.RowPitch,
                            pixels.data() + u64(row) * last_frame->width * pixel_bytes,
                            u64(last_frame->width) * pixel_bytes);
            const D3D12_RANGE contents{0, static_cast<SIZE_T>(bytes)};
            last_frame->upload->Unmap(0, &contents);
        }
        encoder.Transition(buffers[index].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
        source.pResource = last_frame->upload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;
        if (upscale) {
            if (!last_frame->spatial_input.Get())
                winrt::check_hresult(resources.CreateTexture2D(
                    description, D3D12_RESOURCE_STATE_COPY_SOURCE, last_frame->spatial_input));
            encoder.Track(last_frame->spatial_input.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            encoder.Transition(last_frame->spatial_input.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        }
        destination.pResource = upscale ? last_frame->spatial_input.Get() : buffers[index].Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        if (upscale) {
            encoder.Transition(last_frame->spatial_input.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            upscale_source = last_frame->spatial_input.Get();
        }
    } else {
        encoder.Transition(buffers[index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        encoder.ClearColor(buffers[index].Get(), descriptors.Cpu(index),
                           {0.025f, 0.035f, 0.05f, 1.0f});
    }
    if (upscale_source) {
        auto* filtered = command_frame.fsr.Record(context.Device(), commands, upscale_source, width,
                                                  height, EmulatorSettings.IsRcasEnabled(),
                                                  EmulatorSettings.GetRcasAttenuation() / 1000.f);
        commands->CopyResource(buffers[index].Get(), filtered);
    }
    encoder.Transition(buffers[index].Get(), D3D12_RESOURCE_STATE_PRESENT);
    winrt::check_hresult(commands->Close());
    const auto ticket = context.Submit(commands);
    command_frame.fence = ticket;
    command_frame.submitted_frame = last_frame;
    ProfileSample(presenter_profile, "swapchain_present", 128,
                  [&] { winrt::check_hresult(swapchain->Present(1, 0)); });
}

void D3D12Presenter::Resize(uint32_t new_width, uint32_t new_height, float scale_x, float scale_y) {
    auto lock = ProfileSample(presenter_profile, "presenter_lock", 128,
                              [&] { return std::unique_lock{mutex}; });
    if (!new_width || !new_height || !std::isfinite(scale_x) || !std::isfinite(scale_y) ||
        scale_x <= 0 || scale_y <= 0)
        return;
    if (display_width == new_width && display_height == new_height &&
        composition_scale_x == scale_x && composition_scale_y == scale_y)
        return;
    display_width = new_width;
    display_height = new_height;
    composition_scale_x = scale_x;
    composition_scale_y = scale_y;
    RenderFrame();
}
D3D12Presenter::~D3D12Presenter() {
    try {
        Drain();
    } catch (...) {
    }
}
u64 D3D12Presenter::MemoryBytes() const {
    auto lock = ProfileSample(presenter_profile, "presenter_lock", 128,
                              [&] { return std::unique_lock{mutex}; });
    u64 bytes =
        resources.Stats().live_bytes + conversion_shaders.MemoryBytes() + frame_bytes->load();
    for (const auto& command_frame : command_frames)
        bytes += command_frame.fsr.MemoryBytes();
    for (const auto& buffer : buffers)
        if (buffer) {
            const auto description = buffer->GetDesc();
            bytes += context.Device()->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
        }
    return bytes;
}
void D3D12Presenter::TrimMemory() {
    auto lock = ProfileSample(presenter_profile, "presenter_lock", 128,
                              [&] { return std::unique_lock{mutex}; });
    if (!context.DirectQueue())
        return;
    context.WaitForGpu();
    for (auto& command_frame : command_frames) {
        command_frame.fence = 0;
        command_frame.submitted_frame.reset();
    }
    for (auto& command_frame : command_frames)
        command_frame.fsr.TrimMemory();
    conversion_shaders.TrimMemory(0);
    static_cast<void>(context.TryTrim());
}
void D3D12Presenter::Drain() {
    auto lock = ProfileSample(presenter_profile, "presenter_lock", 128,
                              [&] { return std::unique_lock{mutex}; });
    if (context.DirectQueue()) {
        context.WaitForGpu();
        for (auto& command_frame : command_frames) {
            command_frame.fence = 0;
            command_frame.submitted_frame.reset();
        }
    }
}
void D3D12Presenter::ResizeBuffers(uint32_t new_width, uint32_t new_height, DXGI_FORMAT new_format) {
    if (!swapchain || !new_width || !new_height || (new_width == width && new_height == height && new_format == format)) {
        return;
    }
    context.WaitForGpu();
    for (auto& command_frame : command_frames) {
        command_frame.fence = 0;
        command_frame.submitted_frame.reset();
        command_frame.fsr.ResetImages();
    }
    D3D12::NativeTemporalJitter().Reset();
    for (auto& buffer : buffers) {
        buffer.Reset();
    }
    winrt::check_hresult(swapchain->ResizeBuffers(2, new_width, new_height, new_format, 0));
    const auto color_space = new_format == DXGI_FORMAT_R16G16B16A16_FLOAT
                                 ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT support{};
    winrt::check_hresult(swapchain->CheckColorSpaceSupport(color_space, &support));
    if (!(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
        throw std::runtime_error("D3D12 VideoOut color space is not supported");
    winrt::check_hresult(swapchain->SetColorSpace1(color_space));
    format = new_format;
    width = new_width;
    height = new_height;
    GetBuffers();
}
} // namespace Core::Uwp
