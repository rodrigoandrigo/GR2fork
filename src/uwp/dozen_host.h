#pragma once

#include <memory>
#include "uwp/shadps4_api.h"

namespace Frontend {
class WindowSDL;
}

namespace Core::Uwp {
void ReleaseDozenBootstrap();
void ShutdownDozenDriver();
class DozenHost {
public:
    DozenHost();
    ~DozenHost();
    void Initialize(const ShadPS4Config& config);
    void Resize(uint32_t width, uint32_t height);
    void Render();
    static void CheckEnvironment();
    void ReleaseBootstrap();

private:
    std::unique_ptr<Frontend::WindowSDL> window;
    struct Bootstrap;
    std::unique_ptr<Bootstrap> bootstrap;
};
}
