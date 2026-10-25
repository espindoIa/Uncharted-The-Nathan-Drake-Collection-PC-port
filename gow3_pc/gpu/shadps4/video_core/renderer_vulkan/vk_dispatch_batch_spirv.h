// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <span>
#include <string>
#include <vector>

#include "common/types.h"

namespace Vulkan {
// Each original resource becomes a descriptor array indexed by workgroup Y. Original shader
// Y coordinates remain zero (and NumWorkgroups.y remains one); original X/Z are unchanged.
// PushData becomes one more per-instance storage-buffer descriptor. No math/format changes.
std::vector<u32> MakeDispatchBatchSpirv(std::span<const u32> source, u32 capacity,
                                      u32 buffer_bindings, std::string& problem);
} // namespace Vulkan
