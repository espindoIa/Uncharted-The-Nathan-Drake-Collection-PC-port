// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 diagnostics (GOW3_SUBMIT_LOG=<file>): one line per Vulkan submission of the logged
// schedulers (draw 0, present 1, flip 2) with where its CPU time went, the time the GPU finished
// it and two GPU timestamps (start of its command buffer, end), plus the flip and present steps
// of the presenter. Times are steady-clock ms (as GOW3_FRAME_LOG and GOW3_DISPLAY_LOG); GPU
// timestamps are raw device ms (align them by the smallest "done - end" gap). Lines:
//   U <t> <sched> <tick> <slot tick> <thread> <waits> <lock ms> <recording ms> <host copies ms> <driver ms>
//   T <t> <sched> <tick> <gpu start ms> <gpu end ms>       (the GPU reached the tick)
//   <event> <t> <thread> <values...>                       (Event: flip and present steps)

#pragma once

#include <cstdarg>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Semaphore;
} // namespace Vulkan

namespace Vulkan::SubmitLog {

/// Whether GOW3_SUBMIT_LOG is set.
bool Enabled();

/// Steady-clock ms, the time base of every line.
double NowMs();

/// "<tag> <now> <thread> <printf(fmt, ...)>".
void Event(const char* tag, const char* fmt = "", ...);

/// At the start of a command buffer: resets and writes its start timestamp; returns the query
/// slot to pass to End/Submitted, or ~0u (logging off).
u32 Begin(const Instance& instance, vk::CommandBuffer cmdbuf);

/// Before vkEndCommandBuffer: the end timestamp of `slot`.
void End(vk::CommandBuffer cmdbuf, u32 slot);

struct SubmitTimes {
    double start, locked, recorded, copied, submitted;
};

/// After vkQueueSubmit: logs the submission and watches for the GPU to reach `tick`.
void Submitted(int sched, Semaphore* semaphore, u64 tick, u64 slot_tick, u32 slot, u32 waits,
               const SubmitTimes& times);

} // namespace Vulkan::SubmitLog
