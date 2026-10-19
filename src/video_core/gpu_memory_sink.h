#pragma once

#include <memory>
#include "common/types.h"
#include "video_core/resident_frame.h"

namespace VideoCore {
class GpuMemorySink {
public:
    virtual ~GpuMemorySink() = default;
    virtual std::shared_ptr<void> AcquireCpuAccess(VAddr, u64, bool) {
        return {};
    }
    virtual std::shared_ptr<void> AcquireMemoryMappingAccess() {
        return {};
    }
    virtual std::shared_ptr<ResidentFrame> CapturePresentation(const ImageInfo&) {
        return {};
    }
    virtual void MapMemory(VAddr address, u64 size) = 0;
    virtual void RegisterMemory(VAddr address, u64 size) = 0;
    virtual void UnmapMemory(VAddr address, u64 size) = 0;
    virtual bool InvalidateMemory(VAddr address, u64 size, bool assume_locks = false) = 0;
};
} // namespace VideoCore
