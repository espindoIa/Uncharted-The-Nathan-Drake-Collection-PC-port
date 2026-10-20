// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 20 Oct 2026: the trigger files were checked where they are used: the capture trigger on
// every flip, the frame dump trigger (a remove) on every presented frame, the readback trigger on
// every submission, the DMA trigger every 500 ms on the draw recording thread. The launcher sets
// them for every player (diagnostics on). A U3 stall profile (GOW3_PROFILE_STALLS, museum and bar
// fight at 4K) caught single checks of a missing file taking 25-50 ms on the GPU command and draw
// recording threads (std::filesystem::exists, 50 of 62 samples of a 73 ms frame), so the game's
// next frame waited for the file system. One thread now polls them every 250 ms; the GPU threads
// only read an atomic flag.
#include "gow3_trigger_files.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace Gow3TriggerFiles {

namespace {
struct Slot {
    std::string path;
    std::atomic<bool> pending{false};
    std::string text; // guarded by mutex
};
std::array<Slot, 8> slots;
std::atomic<int> slot_count{0};
std::mutex mutex;

void Poll() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const int count = slot_count.load(std::memory_order_acquire);
        for (int i = 0; i < count; ++i) {
            Slot& slot = slots[i];
            if (slot.pending.load(std::memory_order_acquire)) {
                continue; // not taken yet: the next file waits for it
            }
            FILE* f = std::fopen(slot.path.c_str(), "r");
            if (!f) {
                continue;
            }
            char line[256]{};
            if (!std::fgets(line, sizeof(line), f)) {
                line[0] = '\0';
            }
            std::fclose(f);
            std::remove(slot.path.c_str());
            {
                std::scoped_lock lk{mutex};
                slot.text = line;
            }
            slot.pending.store(true, std::memory_order_release);
        }
    }
}
} // namespace

int Watch(const char* env) {
    const char* path = std::getenv(env);
    if (!path || !path[0]) {
        return -1;
    }
    std::scoped_lock lk{mutex};
    const int count = slot_count.load(std::memory_order_relaxed);
    for (int i = 0; i < count; ++i) {
        if (slots[i].path == path) {
            return i;
        }
    }
    if (count == int(slots.size())) {
        std::printf("Trigger files: no slot left for %s\n", env);
        return -1;
    }
    slots[count].path = path;
    slot_count.store(count + 1, std::memory_order_release);
    if (count == 0) {
        std::thread(Poll).detach();
    }
    return count;
}

std::optional<std::string> Take(int slot) {
    if (slot < 0 || !slots[slot].pending.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    std::string text;
    {
        std::scoped_lock lk{mutex};
        text = std::move(slots[slot].text);
    }
    slots[slot].pending.store(false, std::memory_order_release);
    return text;
}

} // namespace Gow3TriggerFiles
