// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/polyfill_thread.h"
#include "common/rdtsc.h"
#include "common/thread.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_record_thread.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

namespace Vulkan {

RecordThread::RecordThread(const Instance& instance_, Semaphore& work_semaphore_)
    : instance{instance_}, work_semaphore{work_semaphore_},
      command_pool{instance, &work_semaphore} {
    BeginPrimary();
    thread = std::jthread([this](std::stop_token stop) {
        try {
            Run(stop);
        } catch (...) {
            {
                std::scoped_lock lock{mutex};
                failure = std::current_exception();
            }
            done_cv.notify_all();
        }
    });
}

RecordThread::~RecordThread() = default;

std::unique_ptr<CommandChunk> RecordThread::AcquireChunk() {
    std::scoped_lock lk{mutex};
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (free_chunks.empty()) {
        return std::make_unique<CommandChunk>();
    }
    auto chunk = std::move(free_chunks.back());
    free_chunks.pop_back();
    return chunk;
}

u64 RecordThread::Dispatch(std::unique_ptr<CommandChunk> chunk, const SubmitRequest* request) {
    u64 blocked = 0;
    {
        std::unique_lock lk{mutex};
        if (pending.size() >= MaxQueued) {
            const u64 t0 = Common::FencedRDTSC();
            done_cv.wait(lk, [this] { return failure || pending.size() < MaxQueued; });
            blocked = Common::FencedRDTSC() - t0;
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
        Work& work = pending.emplace_back();
        work.chunk = std::move(chunk);
        if (request) {
            work.submit = *request;
        }
        ++dispatched;
    }
    work_cv.notify_one();
    return blocked;
}

void RecordThread::WaitIdle() {
    std::unique_lock lk{mutex};
    done_cv.wait(lk, [this] { return failure || completed == dispatched; });
    if (failure) {
        std::rethrow_exception(failure);
    }
}

u64 RecordThread::WaitSubmitted(u64 tick) {
    std::unique_lock lk{mutex};
    if (submitted_tick >= tick) {
        return 0;
    }
    const u64 t0 = Common::FencedRDTSC();
    done_cv.wait(lk, [this, tick] { return failure || submitted_tick >= tick; });
    if (failure) {
        std::rethrow_exception(failure);
    }
    return Common::FencedRDTSC() - t0;
}

void RecordThread::ResetCommandBuffer() {
    WaitIdle();
    // With the queue drained the thread is parked, so the pool is free to use from here.
    Check(primary.reset());
}

void RecordThread::Run(std::stop_token stop) {
    Common::SetCurrentThreadName("shadPS4:VkRecord");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
    while (true) {
        Work work;
        {
            std::unique_lock lk{mutex};
            Common::CondvarWait(work_cv, lk, stop, [this] { return !pending.empty(); });
            // A stop request drains the queue first: a waiter may hold a tick that only a
            // queued submit signals.
            if (pending.empty()) {
                return;
            }
            work = std::move(pending.front());
            pending.pop_front();
        }
        done_cv.notify_all();

        work.chunk->ExecuteAll(primary);
        if (work.submit) {
            Submit(*work.submit);
        }
        work.chunk->Reset();
        {
            std::scoped_lock lk{mutex};
            free_chunks.push_back(std::move(work.chunk));
            ++completed;
            if (work.submit) {
                submitted_tick = work.submit->signal_tick;
            }
        }
        done_cv.notify_all();
    }
}

void RecordThread::BeginPrimary() {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };
    primary = command_pool.Commit();
    Check(primary.begin(begin_info));
}

void RecordThread::Submit(const SubmitRequest& request) {
    // Every batch ends with a device-to-host memory barrier, so the host may read the GPU writes
    // of a batch out of a host visible arena once its tick passes.
    const vk::MemoryBarrier2 barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    primary.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
    Check(primary.end());

    const SubmitInfo& info = request.info;
    std::array<vk::PipelineStageFlags, std::tuple_size_v<decltype(info.wait_semas)>>
        wait_stage_masks;
    wait_stage_masks.fill(vk::PipelineStageFlagBits::eAllCommands);

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = 1,
        .pCommandBuffers = &primary,
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    {
        std::scoped_lock lk{Scheduler::submit_mutex};
        ImGui::Core::TextureManager::Submit();
        const auto submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
        ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");
    }

    work_semaphore.Refresh();
    BeginPrimary();
}

} // namespace Vulkan
