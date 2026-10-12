#pragma once

#include <memory>
#include "common/types.h"
#include "core/libraries/videoout/buffer.h"

namespace VideoCore {
struct PresentationFrame {
    virtual ~PresentationFrame() = default;
};
using FrameHandle = std::shared_ptr<PresentationFrame>;
class PresentationSink {
public:
    virtual ~PresentationSink() = default;
    virtual void RegisterVideoOutSurface(const Libraries::VideoOut::BufferAttributeGroup& group,
                                         VAddr address) = 0;
    virtual FrameHandle PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& group,
                                     VAddr address) = 0;
    virtual FrameHandle PrepareBlankFrame(bool present_thread) = 0;
    virtual FrameHandle PrepareLastFrame() = 0;
    virtual void Present(const FrameHandle& frame, bool reuse = false, bool game = true) = 0;
    virtual bool IsHDRSupported() const = 0;
    virtual void SetHDR(bool enabled) = 0;
    virtual void SetGamma(float gamma) = 0;
};
void SetPresentationSink(std::shared_ptr<PresentationSink> sink);
std::shared_ptr<PresentationSink> GetPresentationSink();
} // namespace VideoCore
