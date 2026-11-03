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
#ifdef SHADPS4_UWP
#include <vk_mem_alloc.h>
#include <winrt/Windows.System.h>
#include "uwp/session.h"
#endif

namespace Vulkan {

RecordThread::RecordThread(const Instance& instance_, Semaphore& work_semaphore_)
    : instance{instance_}, work_semaphore{work_semaphore_},
      command_pool{instance, &work_semaphore} {
    BeginPrimary();
    thread = std::jthread([this](std::stop_token stop) {
        try {
            Run(stop);
        } catch (...) {
            const auto error = std::current_exception();
            {
                std::scoped_lock lock{mutex};
                failure = error;
            }
            done_cv.notify_all();
            Diagnose(operation, true);
#ifdef SHADPS4_UWP
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& exception) {
                Core::Uwp::FailGuestSession(exception.what());
            } catch (...) {
                Core::Uwp::FailGuestSession("Vulkan recording failed with an unknown exception");
            }
#endif
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
    Diagnose("vkResetCommandBuffer");
    Check<"vkResetCommandBuffer: VkRecord">(primary.reset());
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

        active_tick = work.submit ? work.submit->signal_tick : work_semaphore.CurrentTick();
        Diagnose("CommandChunk::ExecuteAll");
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

void RecordThread::Diagnose(const char* next_operation, bool failed) noexcept {
    operation = next_operation;
#ifdef SHADPS4_UWP
    try {
        const auto now = GetTickCount64();
        if (!failed && now - memory_sample_time < 100)
            return;
        u64 usage{}, limit{};
        bool sample_valid{};
        try {
            usage = winrt::Windows::System::MemoryManager::AppMemoryUsage();
            limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
            sample_valid = true;
        } catch (const winrt::hresult_error&) {
            if (!failed)
                return;
        }
        const auto growth = usage > memory_sample_usage ? usage - memory_sample_usage : 0;
        const auto headroom = limit - std::min(usage, limit);
        const bool report =
            failed || (now - memory_report_time >= 1000 &&
                       ((memory_sample_time && growth >= 128_MB) || (limit && headroom < 640_MB)));
        if (report) {
            size_t queued{}, recycled{};
            u64 sent{}, done{}, submitted{};
            {
                std::scoped_lock lock{mutex};
                queued = pending.size();
                recycled = free_chunks.size();
                sent = dispatched;
                done = completed;
                submitted = submitted_tick;
            }
            LOG_WARNING(Render_Vulkan,
                        "[VkRecordMemory] failed={} operation={} tick={} app_sample_valid={} "
                        "app_bytes={} limit_bytes={} "
                        "headroom_bytes={} previous_bytes={} interval_ms={} queued={} recycled={} "
                        "dispatched={} completed={} submitted={} known_gpu={}",
                        failed, operation, active_tick, sample_valid, usage, limit, headroom,
                        memory_sample_usage, memory_sample_time ? now - memory_sample_time : 0,
                        queued, recycled, sent, done, submitted, work_semaphore.KnownGpuTick());
            memory_report_time = now;
        }
        memory_sample_time = now;
        memory_sample_usage = usage;
        if (failed) {
            std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
            vmaGetHeapBudgets(instance.GetAllocator(), budgets.data());
            const auto& properties = instance.GetMemoryProperties();
            for (u32 i = 0; i < properties.memoryHeapCount; ++i) {
                const auto& heap = budgets[i];
                LOG_ERROR(Render_Vulkan,
                          "[VkRecordMemory] heap={} flags={:#x} size={} usage={} budget={} "
                          "vma_block_bytes={} vma_allocation_bytes={} blocks={} allocations={}",
                          i, u32(properties.memoryHeaps[i].flags), properties.memoryHeaps[i].size,
                          heap.usage, heap.budget, heap.statistics.blockBytes,
                          heap.statistics.allocationBytes, heap.statistics.blockCount,
                          heap.statistics.allocationCount);
            }
            Common::Log::Flush();
        }
    } catch (...) {
    }
#endif
}

void RecordThread::BeginPrimary() {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };
    Diagnose("CommandPool::Commit");
    primary = command_pool.Commit();
    Diagnose("vkBeginCommandBuffer");
    Check<"vkBeginCommandBuffer: VkRecord">(primary.begin(begin_info));
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
    Diagnose("vkEndCommandBuffer");
    Check<"vkEndCommandBuffer: VkRecord">(primary.end());

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
        Diagnose("ImGui texture submission");
        ImGui::Core::TextureManager::Submit();
        Diagnose("vkQueueSubmit");
        const auto submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
        Check<"vkQueueSubmit: VkRecord">(submit_result);
    }

    Diagnose("timeline semaphore refresh");
    work_semaphore.Refresh();
    BeginPrimary();
}

} // namespace Vulkan
