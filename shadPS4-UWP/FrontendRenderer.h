#pragma once

#include <array>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include "d3d12_device_context.h"

struct ThumbnailPixels {
    std::string key;
    uint32_t width{};
    uint32_t height{};
    std::vector<uint8_t> pixels;
};

class FrontendRenderer {
public:
    static constexpr size_t ImageCapacity = 64;
    ~FrontendRenderer();
    void Initialize(uint32_t width, uint32_t height, std::function<HRESULT(IDXGISwapChain*)> attach,
                    IUnknown* adapter = nullptr);
    void Resize(uint32_t width, uint32_t height);
    void BeginFrame(float delta);
    void EndFrame();
    bool Upload(const ThumbnailPixels& pixels);
    uintptr_t Image(const std::string& key);
    size_t ImageCount() const;
    std::vector<uint8_t> Readback();

private:
    struct Frame {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12Resource> target;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> uploads;
        uint64_t ticket{};
    };
    struct Texture {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        uint32_t descriptor{};
        uint64_t last_frame{};
        uint64_t ticket{};
    };
    void GetTargets();
    bool Evict();
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(uint32_t index) const;
    D3D12DeviceContext context;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
    std::array<Frame, 3> frames;
    std::unordered_map<std::string, Texture> images;
    std::vector<uint32_t> free_descriptors;
    uint32_t srv_stride{};
    uint32_t rtv_stride{};
    uint32_t width{};
    uint32_t height{};
    uint32_t frame_index{};
    uint64_t frame_number{};
    bool initialized{};
};
