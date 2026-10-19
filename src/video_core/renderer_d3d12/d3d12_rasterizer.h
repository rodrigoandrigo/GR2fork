#pragma once

#include <string>
#include <vector>
#include <d3d12sdklayers.h>
#include <fmt/format.h>
#include <winrt/base.h>
#include "uwp/session.h"
#include "video_core/gpu_command_sink.h"
#include "video_core/renderer_d3d12/command_runtime.h"

namespace AmdGpu {
struct Liverpool;
}
namespace Core {
class MemoryManager;
}
namespace D3D12 {
namespace {
std::string RuntimeErrorTrace(D3D12DeviceContext& device) {
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> queue;
    if (FAILED(device.Device()->QueryInterface(IID_PPV_ARGS(&queue))))
        return {};
    std::string trace;
    const UINT count = queue->GetNumStoredMessages();
    for (UINT index = 0; index < count && trace.size() < 4096; ++index) {
        SIZE_T size{};
        if (FAILED(queue->GetMessage(index, nullptr, &size)) || size > 4096 - trace.size())
            continue;
        std::vector<u8> storage(size);
        const auto message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (FAILED(queue->GetMessage(index, message, &size)))
            continue;
        fmt::format_to(std::back_inserter(trace), " [id={} sev={} {}]",
                       static_cast<u32>(message->ID), static_cast<u32>(message->Severity),
                       message->pDescription ? message->pDescription : "");
    }
    return trace;
}
} // namespace

class Rasterizer final : public VideoCore::GpuCommandSink, public VideoCore::GpuMemorySink {
public:
    Rasterizer(AmdGpu::Liverpool& liverpool, Core::MemoryManager& memory);
    ~Rasterizer() override;
    void Draw(bool indexed, u32 index_offset = 0) override;
    void DrawIndirect(bool indexed, VAddr address, u32 offset, u32 stride, u32 count,
                      VAddr count_address) override;
    void DrawIndirect(bool indexed, VAddr address, u32 offset, u32 stride, u32 count,
                      VAddr count_address, u16 vertex_sgpr, u16 instance_sgpr) override;
    void DispatchDirect() override;
    void DispatchIndirect(VAddr address, u32 offset, u32 size) override;
    void FillBuffer(VAddr address, u32 size, u32 value, bool gds) override;
    void CopyBuffer(VAddr dst, VAddr src, u32 size, bool dst_gds, bool src_gds) override;
    u32 ReadDataFromGds(u32 offset) override;
    void ProcessDownloadImages() override {}
    void CpSync() override;
    u64 Flush() override;
    void Finish() override;
    void OnSubmit() override {}
    void ScopeMarkerBegin(std::string_view, bool = false) override {}
    void ScopeMarkerEnd(bool = false) override {}
    void ScopedMarkerInsertColor(std::string_view, u32, bool = false) override {}
    std::shared_ptr<void> AcquireCpuAccess(VAddr address, u64 size, bool write) override;
    std::shared_ptr<void> AcquireMemoryMappingAccess() override;
    std::shared_ptr<VideoCore::ResidentFrame> CapturePresentation(
        const VideoCore::ImageInfo& info) override;
    void MapMemory(VAddr address, u64 size) override;
    void RegisterMemory(VAddr address, u64 size) override;
    void UnmapMemory(VAddr address, u64 size) override;
    bool InvalidateMemory(VAddr address, u64 size, bool assume_locks = false) override;

private:
    template <typename Function>
    void Execute(Function&& function) {
        if (failed)
            return;
        try {
            function();
        } catch (const winrt::hresult_error& error) {
            failed = true;
            Core::Uwp::FailGuestSession(fmt::format(
                "D3D12 HRESULT {:08x}: {}{}", u32(error.code().value),
                winrt::to_string(error.message()), RuntimeErrorTrace(runtime.Device())));
        } catch (const std::exception& error) {
            failed = true;
            Core::Uwp::FailGuestSession(
                fmt::format("{}{}", error.what(), RuntimeErrorTrace(runtime.Device())));
        } catch (...) {
            failed = true;
            Core::Uwp::FailGuestSession("D3D12 guest GPU execution failed");
        }
    }
    void DrawIndirectImpl(bool indexed, VAddr address, u32 offset, u32 stride, u32 count,
                          VAddr count_address, u16 vertex_sgpr, u16 instance_sgpr);
    void DispatchIndirectImpl(VAddr address, u32 offset, u32 size);
    void FillBufferImpl(VAddr address, u32 size, u32 value, bool gds);
    void CopyBufferImpl(VAddr dst, VAddr src, u32 size, bool dst_gds, bool src_gds);
    bool failed{};
    bool logged_draw{}, logged_dispatch{};
    struct IndirectDraw {
        u32 count, instances, first, first_instance;
        s32 base_vertex;
        u16 vertex_sgpr, instance_sgpr;
    };
    void DrawImpl(bool indexed, u32 index_offset, std::optional<IndirectDraw> indirect);
    void Read(VAddr address, std::span<u8> data);
    void Write(VAddr address, std::span<const u8> data);
    void Dispatch(std::array<u32, 3> groups);
    AmdGpu::Liverpool& liverpool;
    Core::MemoryManager& memory;
    CommandRuntime runtime;
};
} // namespace D3D12
