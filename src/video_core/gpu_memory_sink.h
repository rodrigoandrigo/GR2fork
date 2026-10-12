#pragma once

#include "common/types.h"

namespace VideoCore {
class GpuMemorySink {
public:
    virtual ~GpuMemorySink() = default;
    virtual void MapMemory(VAddr address, u64 size) = 0;
    virtual void RegisterMemory(VAddr address, u64 size) = 0;
    virtual void UnmapMemory(VAddr address, u64 size) = 0;
    virtual bool InvalidateMemory(VAddr address, u64 size, bool assume_locks = false) = 0;
};
} // namespace VideoCore
