#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <tuple>
#include <vector>
#include "d3d12_device_context.h"
#include "d3d12_resource_allocator.h"
#include "video_core/gpu_memory_sink.h"

namespace D3D12 {
class ResourceCache final : public VideoCore::GpuMemorySink {
public:
    using Read = std::function<void(VAddr, std::span<u8>)>;
    using Write = std::function<void(VAddr, std::span<const u8>)>;
    using Resolve = std::function<std::optional<VAddr>(VAddr, u64)>;
    struct Identity {
        VAddr address;
        bool physical;
    };
    struct Buffer {
        VAddr address;
        u64 size;
        Microsoft::WRL::ComPtr<ID3D12Resource> native;
        D3D12Resource upload;
        D3D12Resource readback;
        D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
        std::vector<u8> snapshot;
        u64 used{};
        u64 epoch{};
        u64 allocation_bytes{};
        u64 batch{};
        bool written{};
        bool host{};
        bool physical{};
        struct WriteTarget {
            VAddr address;
            u64 offset, size;
        };
        std::vector<WriteTarget> targets;
        std::vector<u8> host_data;
    };
    struct Range {
        VAddr address;
        u64 size;
    };
    struct View {
        Buffer* buffer;
        u64 offset;
        u64 size;
        VAddr guest_address{};
    };
    void Initialize(D3D12DeviceContext& device, Read read, Write write, Resolve resolve = {});
    Identity Canonicalize(VAddr address, u64 size) const;
    void MarkWritten(const View& view);
    void BeginBatch();
    Buffer& GetBuffer(VAddr address, u64 size, bool physical = false);
    Buffer& GetHostBuffer(VAddr identity, std::span<const u8> data);
    std::vector<View> GetViews(std::span<const Range> ranges);
    void Upload(Buffer& buffer, ID3D12GraphicsCommandList* commands);
    void Barrier(Buffer& buffer, ID3D12GraphicsCommandList* commands, D3D12_RESOURCE_STATES state);
    void PrepareDownload(ID3D12GraphicsCommandList* commands);
    void CommitDownloads();
    void MapMemory(VAddr address, u64 size) override;
    void RegisterMemory(VAddr address, u64 size) override;
    void UnmapMemory(VAddr address, u64 size) override;
    bool InvalidateMemory(VAddr address, u64 size, bool assume_locks = false) override;

private:
    struct Invalidation {
        VAddr address;
        u64 size;
        u64 epoch;
    };
    bool Changed(const Buffer& buffer);
    D3D12DeviceContext* context{};
    D3D12ResourceAllocator allocator;
    Read read;
    Write write;
    Resolve resolve;
    std::map<std::tuple<bool, VAddr, u64>, std::unique_ptr<Buffer>> buffers;
    std::mutex invalidation_mutex;
    std::vector<Invalidation> invalidations;
    u64 epoch{};
    u64 age{};
    u64 batch{};
    u64 native_bytes{};
    u64 invalidation_floor{};
};
} // namespace D3D12
