// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: diagnostics asked for by creating a file (GOW3_CAPTURE_TRIGGER, GOW3_READBACK_TRIGGER,
// GOW3_DMA_TRIGGER, GOW3_FRAME_DUMP_TRIGGER), polled on one background thread
// (gow3_trigger_files.cpp) instead of by the GPU threads on every flip or submission.
#pragma once

#include <optional>
#include <string>

namespace Gow3TriggerFiles {

/// Watches the file named by the environment variable `env`; -1 when it is unset or empty.
int Watch(const char* env);

/// The first line of the watched file if it appeared since the last call (the watcher removed
/// it); nothing otherwise. One atomic load when nothing is pending.
std::optional<std::string> Take(int slot);

} // namespace Gow3TriggerFiles
