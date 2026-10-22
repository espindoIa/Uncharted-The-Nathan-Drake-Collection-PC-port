// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace Gow3DisplayAdapter {

enum class Result { Same, Other, Unknown };

/// Whether the monitor showing `hwnd` is driven by the GPU with this 8-byte LUID (Vulkan's
/// deviceLUID, laid out as a Windows LUID). Other: a hybrid laptop, where another adapter shows the
/// frames. Unknown without a window or LUID, outside Windows, or when no output matches.
Result Compare(void* hwnd, const std::uint8_t* luid);

} // namespace Gow3DisplayAdapter
