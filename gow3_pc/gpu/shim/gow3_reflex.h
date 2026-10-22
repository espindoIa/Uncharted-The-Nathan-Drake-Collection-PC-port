// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 22 Oct 2026: NVIDIA Reflex through VK_NV_low_latency2 (the Vulkan extension: no NVAPI, no
// extra DLL). With the setting `reflex` on, the driver paces the guest's frame start: a
// vkLatencySleepNV is waited for just before the first pad read of each guest frame, and the
// driver gets markers (simulation, input sample, render submit, present) for every frame.
// GOW3_REFLEX_LOG=<file> records the driver's own per-frame timings, also with reflex 0 (the
// baseline of an A/B). Without the extension (AMD, Intel, older NVIDIA drivers) or with
// GOW3_REFLEX_EXT=0 nothing here reaches Vulkan.
//
// A frame's id is the flip ordinal. The guest counts a flip when it asks for it (GuestFlipRequest)
// and the presenter when it prepares the frame (PreparedFlip); both count in order, so the same
// frame has the same id on the guest thread (simulation, input sampling, render submit) and on the
// present thread (render submit end, present). Every call is thread safe and costs next to nothing
// while nothing is active (reflex 0 without a log): a relaxed atomic load, plus a counter that
// keeps advancing so the ids still agree when the setting is switched on in the middle of a run.

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Gow3Reflex {

/// Device creation (vk_instance.cpp): false with GOW3_REFLEX_EXT=0, which keeps the extension off.
bool ExtensionWanted();

/// Once, after the device exists and the dispatcher loaded its functions. `extension_enabled`:
/// VK_NV_low_latency2 (and what it needs) went into the device; `revision` is the driver's
/// specVersion for it. Prints one line; creates the timeline semaphore the sleeps wait on and opens
/// GOW3_REFLEX_LOG.
void Init(vk::Device device, bool extension_enabled, u32 revision);

/// Destroys what Init created (device teardown).
void Shutdown(vk::Device device);

/// The extension is enabled and its entry points are loaded: swapchains are created with low
/// latency mode and every call below can reach Vulkan.
bool Available();

/// The swapchain markers and sleeps go to; null before it is destroyed (and none are sent until
/// the next one). A new swapchain starts with the mode off: the current one is applied here.
void OnSwapchain(vk::SwapchainKHR swapchain);

/// Guest thread, the pad read: the first read after a flip request sleeps for the driver's pacing
/// (reflex 1 or 2) and sets the frame's simulation start and input sample markers. Call it
/// outside any lock.
void InputSample();

/// Guest thread, a command buffer submit: the first of a frame sets its render submit start marker.
void GuestSubmit();

/// Guest thread, the game asked for a flip (it was accepted): counts the flip, sets the simulation
/// end marker of its frame and starts the next frame.
void GuestFlipRequest();

/// Presenter, when it prepares the frame of a flip (in order): the flip's ordinal, the frame's id.
u64 PreparedFlip();

/// Presenter, after the frame's last submission: sets the render submit end marker.
void RenderSubmitEnd(u64 serial);

/// Present thread, around vkQueuePresentKHR of a game frame (not redraws, black or loading frames).
/// PresentStart also applies a change of the mode (setting or toggle).
void PresentStart(u64 serial);
void PresentEnd(u64 serial);

/// Appends the driver's timings of frames not written yet to GOW3_REFLEX_LOG (PresentEnd calls it
/// every 48 game frames: the driver holds 64 reports).
void Report();

} // namespace Gow3Reflex
