#pragma once

#include <memory>
#include <span>
#include <nlohmann/json_fwd.hpp>
#include "uwp/shadps4_api.h"

namespace Frontend {
class WindowSDL;
}

namespace Core::Uwp {
nlohmann::json DozenCompileDiagnostic(uint32_t thread);
void ConfigureDozenNativeSampling(uintptr_t device, uintptr_t layout,
                                  std::span<const uint32_t> bindings);
void ReleaseDozenBootstrap();
void ShutdownDozenDriver();
class DozenHost {
public:
    DozenHost();
    ~DozenHost();
    void Initialize(const ShadPS4Config& config);
    void DetachSurface();
    void Resize(uint32_t width, uint32_t height);
    void Render();
    static void CheckEnvironment();
    void ReleaseBootstrap();

private:
    ShadPS4AttachSwapChain attach{};
    void* attach_context{};
    std::unique_ptr<Frontend::WindowSDL> window;
    struct Bootstrap;
    std::unique_ptr<Bootstrap> bootstrap;
};
}
