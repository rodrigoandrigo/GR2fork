// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_command_chunk.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

class Instance;
class Semaphore;

/// A queue submit the record thread issues once it has replayed the chunk carrying it.
struct SubmitRequest {
    SubmitInfo info;
    u64 signal_tick;
};

/// Owns a scheduler's command pool and primary command buffer: replays the chunks the GPU
/// command thread dispatches into it and issues the queue submits requested with them.
class RecordThread {
public:
    explicit RecordThread(const Instance& instance, Semaphore& work_semaphore);
    ~RecordThread();

    /// Returns an empty chunk to record into.
    [[nodiscard]] std::unique_ptr<CommandChunk> AcquireChunk();

    /// Queues a recorded chunk, with the submit to issue after replaying it. Blocks while 64
    /// chunks are queued and returns the time spent blocked in RDTSC ticks.
    u64 Dispatch(std::unique_ptr<CommandChunk> chunk, const SubmitRequest* request);

    /// Blocks until every dispatched chunk is replayed and every requested submit is issued.
    void WaitIdle();

    /// Blocks until the submit signalling tick is issued and returns the time spent blocked in
    /// RDTSC ticks.
    u64 WaitSubmitted(u64 tick);

    /// Drains the queue and resets the open primary command buffer.
    void ResetCommandBuffer();

private:
    static constexpr size_t MaxQueued = 64;

    struct Work {
        std::unique_ptr<CommandChunk> chunk;
        std::optional<SubmitRequest> submit;
    };

    void Run(std::stop_token stop);
    void BeginPrimary();
    void Submit(const SubmitRequest& request);

    const Instance& instance;
    Semaphore& work_semaphore;
    CommandPool command_pool;
    vk::CommandBuffer primary;
    std::mutex mutex;
    std::condition_variable_any work_cv;
    std::condition_variable_any done_cv;
    std::deque<Work> pending;
    std::vector<std::unique_ptr<CommandChunk>> free_chunks;
    u64 dispatched{};
    u64 completed{};
    u64 submitted_tick{};
    std::exception_ptr failure;
    std::jthread thread;
};

} // namespace Vulkan
