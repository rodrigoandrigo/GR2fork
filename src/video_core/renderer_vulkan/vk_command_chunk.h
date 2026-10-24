// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/assert.h"
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

/// A 64 KiB arena of type-erased command closures, replayed in recording order into a command
/// buffer. A chunk belongs to one thread at a time.
class CommandChunk {
public:
    static constexpr size_t Capacity = 64_KB;
    /// Payloads above this size live on the heap and die with the chunk's records.
    static constexpr size_t MaxInlinePayload = 8_KB;

    CommandChunk() = default;
    CommandChunk(const CommandChunk&) = delete;
    CommandChunk& operator=(const CommandChunk&) = delete;

    /// Whether a record of func with a payload of size bytes fits in the remaining space.
    template <typename F>
    [[nodiscard]] bool Fits(size_t size) const noexcept {
        return used + RecordSize<std::remove_cvref_t<F>>(size) <= Capacity;
    }

    /// Stores func, called at replay as func(cmdbuf).
    template <typename F>
    void Record(F&& func) {
        Emplace(0, std::forward<F>(func));
    }

    /// Stores func with size payload bytes the caller fills before the next record; called at
    /// replay as func(cmdbuf, payload). Returns the payload, 16-byte aligned.
    template <typename F>
    [[nodiscard]] std::byte* RecordWithPayload(size_t size, F&& func) {
        return Emplace(size, std::forward<F>(func));
    }

    /// Replays every record in order.
    void ExecuteAll(vk::CommandBuffer cmdbuf) const {
        for (u32 offset = 0; offset < used;) {
            const auto& entry =
                *std::launder(reinterpret_cast<const Entry*>(storage.data() + offset));
            entry.invoke(entry, cmdbuf);
            offset = entry.next;
        }
    }

    /// Forgets every record, keeping the arena for reuse.
    void Reset() {
        used = 0;
        heap_payloads.clear();
    }

private:
    static constexpr size_t Align = 16;

    struct Entry {
        void (*invoke)(const Entry& entry, vk::CommandBuffer cmdbuf);
        const std::byte* payload;
        u32 next;
    };

    static constexpr size_t AlignUp(size_t size) noexcept {
        return (size + Align - 1) & ~(Align - 1);
    }

    template <typename F>
    static constexpr size_t RecordSize(size_t payload_size) noexcept {
        return AlignUp(sizeof(Entry)) + AlignUp(sizeof(F)) +
               (payload_size <= MaxInlinePayload ? AlignUp(payload_size) : 0);
    }

    template <typename F>
    static void Invoke(const Entry& entry, vk::CommandBuffer cmdbuf) {
        const auto* base = reinterpret_cast<const std::byte*>(&entry) + AlignUp(sizeof(Entry));
        const F& func = *std::launder(reinterpret_cast<const F*>(base));
        if constexpr (std::is_invocable_v<const F&, vk::CommandBuffer, const std::byte*>) {
            func(cmdbuf, entry.payload);
        } else {
            func(cmdbuf);
        }
    }

    template <typename F>
    std::byte* Emplace(size_t payload_size, F&& func) {
        using Func = std::remove_cvref_t<F>;
        // Reset() rewinds the arena without running destructors.
        static_assert(std::is_trivially_copyable_v<Func> && std::is_trivially_destructible_v<Func>);
        static_assert(alignof(Func) <= Align);
        static_assert(RecordSize<Func>(MaxInlinePayload) <= Capacity);
        DEBUG_ASSERT(Fits<Func>(payload_size));

        std::byte* const base = storage.data() + used;
        size_t size = AlignUp(sizeof(Entry)) + AlignUp(sizeof(Func));
        std::byte* payload = nullptr;
        if (payload_size > MaxInlinePayload) {
            payload = heap_payloads.emplace_back(new std::byte[payload_size]).get();
        } else if (payload_size != 0) {
            payload = base + size;
            size += AlignUp(payload_size);
        }
        new (base) Entry{
            .invoke = &Invoke<Func>,
            .payload = payload,
            .next = static_cast<u32>(used + size),
        };
        new (base + AlignUp(sizeof(Entry))) Func(std::forward<F>(func));
        used += static_cast<u32>(size);
        return payload;
    }

    alignas(Align) std::array<std::byte, Capacity> storage;
    u32 used{};
    // Heap payloads take their alignment from operator new.
    static_assert(__STDCPP_DEFAULT_NEW_ALIGNMENT__ >= Align);
    std::vector<std::unique_ptr<std::byte[]>> heap_payloads;
};

} // namespace Vulkan
