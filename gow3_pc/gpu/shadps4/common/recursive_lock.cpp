// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <unordered_map>
#include "common/assert.h"
#include "common/recursive_lock.h"
#include "gow3_toggles.h"

namespace Common::Detail {

struct RecursiveLockState {
    RecursiveLockType type;
    int count;
};

// gow3: the locks a thread holds, in a small fixed array searched linearly. The unordered_map
// alone allocated a node on every first acquisition and freed it on the last release: heap
// calls on each Rasterizer::IsMapped and BufferCache lookup, several per draw (~5% of the
// Uncharted 3 GPU command thread, 18 Oct profile). Threads rarely hold more than a few at
// once; the map takes the rest.
struct HeldLock {
    void* mutex;
    RecursiveLockState state;
};
thread_local std::array<HeldLock, 16> g_held{};
thread_local std::unordered_map<void*, RecursiveLockState> g_recursive_locks;

static RecursiveLockState& Find(void* mutex) {
    HeldLock* free_slot = nullptr;
    for (auto& held : g_held) {
        if (held.mutex == mutex) {
            return held.state;
        }
        if (!held.mutex && !free_slot) {
            free_slot = &held;
        }
    }
    if (const auto it = g_recursive_locks.find(mutex); it != g_recursive_locks.end()) {
        return it->second;
    }
    // Toggle bit 60 (A/B): new entries go to the map only; lookups still search both.
    if (free_slot && !Gow3Toggle::Disabled(Gow3Toggle::RecursiveLockArray)) {
        *free_slot = {mutex, {RecursiveLockType::None, 0}};
        return free_slot->state;
    }
    return g_recursive_locks[mutex];
}

static void Release(void* mutex) {
    for (auto& held : g_held) {
        if (held.mutex == mutex) {
            held = {};
            return;
        }
    }
    g_recursive_locks.erase(mutex);
}

bool IncrementRecursiveLock(void* mutex, RecursiveLockType type) {
    auto& state = Find(mutex);
    if (state.count == 0) {
        ASSERT(state.type == RecursiveLockType::None);
        state.type = type;
    }
    ASSERT(state.type == type);
    return state.count++ == 0;
}

bool DecrementRecursiveLock(void* mutex, RecursiveLockType type) {
    auto& state = Find(mutex);
    ASSERT(state.type == type && state.count > 0);
    if (--state.count == 0) {
        Release(mutex);
        return true;
    }
    return false;
}

} // namespace Common::Detail
