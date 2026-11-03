// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include "common/types.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace Vulkan {
class PipelineCompileProgress {
public:
    enum class Phase : u8 {
        Queued,
        WorkerStarted,
        DescriptorBindings,
        LayoutKey,
        LayoutCacheLock,
        LayoutCacheLookup,
        DescriptorSetLayout,
        PipelineLayout,
        VertexInput,
        PipelineState,
        AuxiliaryGeometry,
        AuxiliaryTessellationControl,
        AuxiliaryTessellationEval,
        AuxiliaryFragment,
        VulkanCreatePipeline,
        PipelineCreated,
        Complete,
        Failed,
        Count,
    };
    static constexpr std::array<const char*, size_t(Phase::Count)> Names{
        "queued",
        "worker_started",
        "descriptor_bindings",
        "layout_key",
        "layout_cache_lock",
        "layout_cache_lookup",
        "create_descriptor_set_layout",
        "create_pipeline_layout",
        "vertex_input",
        "pipeline_state",
        "auxiliary_geometry_shader",
        "auxiliary_tessellation_control_shader",
        "auxiliary_tessellation_eval_shader",
        "auxiliary_fragment_shader",
        "vk_create_graphics_pipeline",
        "pipeline_created",
        "complete",
        "failed"};

    struct Snapshot {
        Phase phase;
        Phase failed_in;
        u32 thread;
        u64 phase_ms;
        std::array<u64, size_t(Phase::Count)> completed_ms;
    };

    PipelineCompileProgress() : state{Now() << 8} {}

    class Scope {
    public:
        explicit Scope(PipelineCompileProgress& progress) : previous{current} {
            current = &progress;
#ifdef _WIN32
            progress.thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
#else
            progress.thread.store(u32(std::hash<std::thread::id>{}(std::this_thread::get_id())),
                                  std::memory_order_relaxed);
#endif
            Mark(Phase::WorkerStarted);
        }
        ~Scope() {
            current = previous;
        }

    private:
        PipelineCompileProgress* previous;
    };

    static void Mark(Phase phase) noexcept {
        if (!current)
            return;
        const auto now = Now();
        const auto previous = current->state.load(std::memory_order_relaxed);
        const auto old_phase = Phase(previous & 0xff);
        current->completed_ms[size_t(old_phase)].fetch_add(now - (previous >> 8),
                                                           std::memory_order_relaxed);
        if (phase == Phase::Failed)
            current->failed_in.store(old_phase, std::memory_order_relaxed);
        current->state.store((now << 8) | u8(phase), std::memory_order_release);
    }

    Snapshot Read() const noexcept {
        const auto value = state.load(std::memory_order_acquire);
        Snapshot snapshot{.phase = Phase(value & 0xff),
                          .failed_in = failed_in.load(std::memory_order_relaxed),
                          .thread = thread.load(std::memory_order_relaxed),
                          .phase_ms = Now() - (value >> 8)};
        for (size_t i = 0; i < snapshot.completed_ms.size(); ++i)
            snapshot.completed_ms[i] = completed_ms[i].load(std::memory_order_relaxed);
        return snapshot;
    }

private:
    static u64 Now() noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    inline static thread_local PipelineCompileProgress* current{};
    std::atomic<u64> state;
    std::atomic<u32> thread{};
    std::atomic<Phase> failed_in{Phase::Queued};
    std::array<std::atomic<u64>, size_t(Phase::Count)> completed_ms{};
};
} // namespace Vulkan
