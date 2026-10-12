#include "FrontendRenderer.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <imgui.h>
#include <imgui_impl_dx12.h>

namespace {
void Check(HRESULT result) {
    if (FAILED(result))
        throw std::runtime_error("D3D12 HRESULT " + std::to_string(static_cast<uint32_t>(result)));
}
D3D12_HEAP_PROPERTIES Heap(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    return heap;
}
D3D12_RESOURCE_DESC Buffer(uint64_t size) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return description;
}
void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
}
} // namespace

FrontendRenderer::~FrontendRenderer() {
    if (initialized) {
        try {
            context.WaitForGpu();
        } catch (...) {
        }
        ImGui_ImplDX12_Shutdown();
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE FrontendRenderer::Cpu(uint32_t index) const {
    auto handle = srv->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += index * srv_stride;
    return handle;
}
D3D12_GPU_DESCRIPTOR_HANDLE FrontendRenderer::Gpu(uint32_t index) const {
    auto handle = srv->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += index * srv_stride;
    return handle;
}

void FrontendRenderer::Initialize(uint32_t new_width, uint32_t new_height,
                                  std::function<HRESULT(IDXGISwapChain*)> attach,
                                  IUnknown* adapter) {
    context.Initialize(adapter);
    width = std::max(1U, new_width);
    height = std::max(1U, new_height);
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = ImageCapacity + 1;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Check(context.Device()->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srv)));
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = frames.size();
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    Check(context.Device()->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv)));
    srv_stride =
        context.Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    rtv_stride = context.Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (uint32_t index = 1; index <= ImageCapacity; ++index)
        free_descriptors.push_back(index);
    if (attach) {
        Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
        Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = width;
        description.Height = height;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = frames.size();
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        Microsoft::WRL::ComPtr<IDXGISwapChain1> chain;
        Check(factory->CreateSwapChainForComposition(context.DirectQueue(), &description, nullptr,
                                                     &chain));
        Check(chain.As(&swapchain));
        Check(attach(chain.Get()));
    }
    GetTargets();
    for (auto& frame : frames)
        Check(context.Device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(&frame.allocator)));
    Check(context.Device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              frames[0].allocator.Get(), nullptr,
                                              IID_PPV_ARGS(&commands)));
    Check(commands->Close());
    if (!ImGui_ImplDX12_Init(context.Device(), frames.size(), DXGI_FORMAT_R8G8B8A8_UNORM, srv.Get(),
                             Cpu(0), Gpu(0)))
        throw std::runtime_error("ImGui DX12 initialization failed");
    initialized = true;
}

void FrontendRenderer::GetTargets() {
    for (uint32_t index = 0; index < frames.size(); ++index) {
        if (swapchain)
            Check(swapchain->GetBuffer(index, IID_PPV_ARGS(&frames[index].target)));
        else {
            D3D12_RESOURCE_DESC description{};
            description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            description.Width = width;
            description.Height = height;
            description.DepthOrArraySize = 1;
            description.MipLevels = 1;
            description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            const auto heap = Heap(D3D12_HEAP_TYPE_DEFAULT);
            Check(context.Device()->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COMMON, nullptr,
                IID_PPV_ARGS(&frames[index].target)));
        }
        auto descriptor = rtv->GetCPUDescriptorHandleForHeapStart();
        descriptor.ptr += index * rtv_stride;
        context.Device()->CreateRenderTargetView(frames[index].target.Get(), nullptr, descriptor);
    }
}

void FrontendRenderer::Resize(uint32_t new_width, uint32_t new_height) {
    if (!new_width || !new_height || (new_width == width && new_height == height))
        return;
    context.WaitForGpu();
    for (auto& frame : frames) {
        frame.target.Reset();
        frame.uploads.clear();
    }
    width = new_width;
    height = new_height;
    if (swapchain)
        Check(
            swapchain->ResizeBuffers(frames.size(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0));
    GetTargets();
}

void FrontendRenderer::BeginFrame(float delta) {
    frame_index = swapchain ? swapchain->GetCurrentBackBufferIndex() : frame_number % frames.size();
    auto& frame = frames[frame_index];
    context.Wait(frame.ticket);
    frame.uploads.clear();
    Check(frame.allocator->Reset());
    Check(commands->Reset(frame.allocator.Get(), nullptr));
    ++frame_number;
    ImGui_ImplDX12_NewFrame();
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    io.DeltaTime = std::clamp(delta, 0.001f, 0.1f);
    ImGui::NewFrame();
}

bool FrontendRenderer::Evict() {
    const auto complete = context.CompletedValue();
    auto victim = images.end();
    for (auto it = images.begin(); it != images.end(); ++it) {
        if (it->second.last_frame == frame_number || it->second.ticket > complete)
            continue;
        if (victim == images.end() || it->second.last_frame < victim->second.last_frame)
            victim = it;
    }
    if (victim == images.end())
        return false;
    free_descriptors.push_back(victim->second.descriptor);
    images.erase(victim);
    return true;
}

bool FrontendRenderer::Upload(const ThumbnailPixels& pixels) {
    if (images.find(pixels.key) != images.end())
        return true;
    if (!pixels.width || !pixels.height || pixels.width > 256 || pixels.height > 256 ||
        pixels.pixels.size() != static_cast<size_t>(pixels.width) * pixels.height * 4)
        return false;
    if (free_descriptors.empty() && !Evict())
        return false;
    Texture image;
    image.descriptor = free_descriptors.back();
    image.last_frame = frame_number;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = pixels.width;
    description.Height = pixels.height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    const auto heap = Heap(D3D12_HEAP_TYPE_DEFAULT);
    Check(context.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&image.resource)));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    uint64_t bytes{};
    context.Device()->GetCopyableFootprints(&description, 0, 1, 0, &layout, nullptr, nullptr,
                                            &bytes);
    const auto upload_heap = Heap(D3D12_HEAP_TYPE_UPLOAD);
    const auto upload_desc = Buffer(bytes);
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    Check(context.Device()->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE,
                                                    &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                    nullptr, IID_PPV_ARGS(&upload)));
    uint8_t* mapped{};
    D3D12_RANGE empty{};
    Check(upload->Map(0, &empty, reinterpret_cast<void**>(&mapped)));
    for (uint32_t row = 0; row < pixels.height; ++row)
        std::memcpy(mapped + layout.Offset + row * layout.Footprint.RowPitch,
                    pixels.pixels.data() + row * pixels.width * 4, pixels.width * 4);
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = layout;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = image.resource.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    Transition(commands.Get(), image.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    frames[frame_index].uploads.push_back(std::move(upload));
    images.emplace(pixels.key, std::move(image));
    free_descriptors.pop_back();
    return true;
}

uintptr_t FrontendRenderer::Image(const std::string& key) {
    const auto found = images.find(key);
    if (found == images.end())
        return 0;
    found->second.last_frame = frame_number;
    return Gpu(found->second.descriptor).ptr;
}
size_t FrontendRenderer::ImageCount() const {
    return images.size();
}

void FrontendRenderer::EndFrame() {
    ImGui::Render();
    auto& frame = frames[frame_index];
    Transition(commands.Get(), frame.target.Get(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto target = rtv->GetCPUDescriptorHandleForHeapStart();
    target.ptr += frame_index * rtv_stride;
    const float opaque[]{0.025f, 0.035f, 0.055f, 1.0f};
    const float clear[]{0, 0, 0, 0};
    const float* color = transparent ? clear : opaque;
    commands->ClearRenderTargetView(target, color, 0, nullptr);
    commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
    auto* heaps = srv.Get();
    commands->SetDescriptorHeaps(1, &heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commands.Get());
    Transition(commands.Get(), frame.target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_COMMON);
    Check(commands->Close());
    frame.ticket = context.Submit(commands.Get());
    for (auto& [key, image] : images)
        if (image.last_frame == frame_number)
            image.ticket = frame.ticket;
    if (swapchain)
        Check(swapchain->Present(1, 0));
}

std::vector<uint8_t> FrontendRenderer::Readback() {
    context.WaitForGpu();
    auto& frame = frames[frame_index];
    Check(frame.allocator->Reset());
    Check(commands->Reset(frame.allocator.Get(), nullptr));
    const auto description = frame.target->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    uint64_t bytes{};
    context.Device()->GetCopyableFootprints(&description, 0, 1, 0, &layout, nullptr, nullptr,
                                            &bytes);
    const auto heap = Heap(D3D12_HEAP_TYPE_READBACK);
    const auto buffer = Buffer(bytes);
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    Check(context.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&readback)));
    Transition(commands.Get(), frame.target.Get(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = frame.target.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = layout;
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    Transition(commands.Get(), frame.target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_COMMON);
    Check(commands->Close());
    context.Wait(context.Submit(commands.Get()));
    uint8_t* mapped{};
    D3D12_RANGE range{0, static_cast<size_t>(bytes)};
    Check(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)));
    std::vector<uint8_t> result(static_cast<size_t>(width) * height * 4);
    for (uint32_t row = 0; row < height; ++row)
        std::memcpy(result.data() + row * width * 4,
                    mapped + layout.Offset + row * layout.Footprint.RowPitch, width * 4);
    readback->Unmap(0, nullptr);
    return result;
}
