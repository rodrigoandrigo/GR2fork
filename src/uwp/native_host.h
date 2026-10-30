#pragma once

#include <memory>
#include <mutex>
#include "uwp/shadps4_api.h"

namespace D3D12 {
class Rasterizer;
}
namespace Frontend {
class WindowSDL;
}
namespace Core::Uwp {
class D3D12Presenter;
bool NativeGraphicsSelected();
void InitializeNativeGpu();
void CheckNativeGraphicsProfile();
void CheckNativeGraphicsEnvironment();
class NativeHost {
public:
    NativeHost();
    ~NativeHost();
    void Initialize(const ShadPS4Config& config);
    void InitializeGpu();
    void DetachSurface();
    void Resize(uint32_t width, uint32_t height, float scale_x = 1.0f, float scale_y = 1.0f);
    void Render();
    void SetActive(bool active);
    void SetHDROutput(bool enabled, float sdr_white_nits = 80.0f);

private:
    std::mutex renderer_mutex;
    D3D12::Rasterizer* rasterizer{};
    bool active{true};
    ShadPS4AttachSwapChain attach{};
    void* attach_context{};
    std::unique_ptr<Frontend::WindowSDL> window;
    std::shared_ptr<D3D12Presenter> presentation;
};
}
