// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "common/scope_exit.h"
#include "common/types.h"
#ifdef SHADPS4_UWP
#include "core/address_space.h"
#endif

namespace Vulkan {

class AllocationRecoveryScope {
public:
    using Callback = u64 (*)(void*, bool, bool);

    AllocationRecoveryScope(Callback callback_, void* owner_)
        : callback{callback_}, owner{owner_}, previous{active} {
        active = this;
    }
    ~AllocationRecoveryScope() {
        active = previous;
    }
    AllocationRecoveryScope(const AllocationRecoveryScope&) = delete;
    AllocationRecoveryScope& operator=(const AllocationRecoveryScope&) = delete;

    class StagingScope {
    public:
        StagingScope() {
            ++staging_depth;
        }
        ~StagingScope() {
            --staging_depth;
        }
        StagingScope(const StagingScope&) = delete;
        StagingScope& operator=(const StagingScope&) = delete;
    };

    static bool CanTrimStaging() {
        return staging_depth == 0;
    }

    static u64 Recover(bool caches = false, bool aggressive = false) {
        auto* scope = active;
        if (!scope || scope->recovering)
            return 0;
        scope->recovering = true;
        SCOPE_EXIT {
            scope->recovering = false;
        };
        return scope->callback(scope->owner, caches, aggressive);
    }

    static void Prepare() {
#ifdef SHADPS4_UWP
        if (Core::AddressSpace::MemoryHeadroom() < 640_MB)
            Recover();
#endif
    }

    static void PrepareCaches() {
#ifdef SHADPS4_UWP
        if (Core::AddressSpace::MemoryHeadroom() < 640_MB)
            Recover(true);
#endif
    }

    static bool RecoverOrPage(u64 bytes) {
#ifdef SHADPS4_UWP
        if (Recover(false, true))
            return true;
        return Core::AddressSpace::ReclaimMemory(bytes) != 0;
#else
        return false;
#endif
    }

private:
    Callback callback;
    void* owner;
    AllocationRecoveryScope* previous;
    static inline thread_local bool recovering{};
    static inline thread_local AllocationRecoveryScope* active{};
    static inline thread_local unsigned staging_depth{};
};

} // namespace Vulkan
