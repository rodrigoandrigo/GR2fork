// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stdexcept>
#include <string_view>
#include <fmt/format.h>
#include "common/types.h"

namespace VideoCore {
class GpuCommandSink {
public:
    virtual ~GpuCommandSink() = default;
    virtual void Draw(bool indexed, u32 index_offset = 0) = 0;
    virtual void DrawIndirect(bool indexed, VAddr address, u32 offset, u32 stride, u32 count,
                              VAddr count_address) = 0;
    virtual void DrawIndirect(bool indexed, VAddr address, u32 offset, u32 stride, u32 count,
                              VAddr count_address, u16 vertex_sgpr, u16 instance_sgpr) {
        if (vertex_sgpr || instance_sgpr)
            throw std::runtime_error("Indirect SGPR offsets are not supported by this renderer");
        DrawIndirect(indexed, address, offset, stride, count, count_address);
    }
    virtual void DispatchDirect() = 0;
    virtual void DispatchIndirect(VAddr address, u32 offset, u32 size) = 0;
    virtual void FillBuffer(VAddr address, u32 size, u32 value, bool gds) = 0;
    virtual void CopyBuffer(VAddr dst, VAddr src, u32 size, bool dst_gds, bool src_gds) = 0;
    virtual u32 ReadDataFromGds(u32 offset) = 0;
    virtual bool OcclusionCounter(u64&, bool = false) {
        return false;
    }
    virtual void ProcessDownloadImages() = 0;
    virtual void CpSync() = 0;
    virtual u64 Flush() = 0;
    virtual void Finish() = 0;
    virtual void RequestFlush(u64) {
        Flush();
    }
    virtual void FlushRequested(bool = false) {}
    virtual void MarkAsGuestMemoryChanged(VAddr, const void*, u64) {}
    virtual void OnSubmit() = 0;
    virtual void OnFence() {
        ProcessDownloadImages();
    }
    virtual bool TryCpWriteBacking(VAddr, const void*, u64) {
        return false;
    }
    virtual void BeginPacketRun() {}
    virtual void EndPacketRun() {}
    virtual void DropCopyHoldForCommands() {}
    virtual bool HostMarkersEnabled() const {
        return false;
    }
    virtual void ScopeMarkerBegin(std::string_view label, bool from_guest = false) = 0;
    virtual void ScopeMarkerEnd(bool from_guest = false) = 0;
    virtual void ScopedMarkerInsertColor(std::string_view label, u32 color,
                                         bool from_guest = false) = 0;
    template <typename Function>
    void ScopeMarker(fmt::string_view format, fmt::format_args args, Function&& function) {
        if (HostMarkersEnabled()) {
            ScopeMarkerBegin(fmt::vformat(format, args));
            function();
            ScopeMarkerEnd();
        } else {
            function();
        }
    }
    class PacketRunGuard {
    public:
        explicit PacketRunGuard(GpuCommandSink* sink) : sink{sink} {
            if (sink)
                sink->BeginPacketRun();
        }
        ~PacketRunGuard() {
            if (sink)
                sink->EndPacketRun();
        }
        PacketRunGuard(const PacketRunGuard&) = delete;
        PacketRunGuard& operator=(const PacketRunGuard&) = delete;

    private:
        GpuCommandSink* sink;
    };
};
} // namespace VideoCore
