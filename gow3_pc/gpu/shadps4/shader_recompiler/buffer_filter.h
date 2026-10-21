// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include "common/types.h"
#include "video_core/amdgpu/resource.h"

namespace Shader {

/// gow3: set once by the rasterizer. Returns true for a guest buffer V# that binding treats as
/// null (garbage), so specialization must treat it as null too. Called from several threads.
using BufferSharpFilter = bool (*)(const AmdGpu::Buffer&, u64 pgm_hash);
inline std::atomic<BufferSharpFilter> g_garbage_buffer_filter{};

} // namespace Shader
