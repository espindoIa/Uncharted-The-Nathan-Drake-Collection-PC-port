// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/types.h"

#include <magic_enum/magic_enum.hpp>

#include <functional>
#include <array>
#include <mutex>
#include <unordered_map>
#include <queue>
#include <condition_variable>
#include <cstdlib>
#include <thread>
#include <vector>

#include "common/thread.h"

namespace Platform {

enum class InterruptId : u32 {
    Compute0RelMem = 0x00,
    Compute1RelMem = 0x01,
    Compute2RelMem = 0x02,
    Compute3RelMem = 0x03,
    Compute4RelMem = 0x04,
    Compute5RelMem = 0x05,
    Compute6RelMem = 0x06,
    GfxEop = 0x40,
    GfxFlip = 0x08,
    GpuIdle = 0x09,

    InterruptIdMax = 0x40, ///< Max possible value (GfxEop)
};

using IrqHandler = std::function<void(InterruptId)>;

struct IrqController {
    void RegisterOnce(InterruptId irq, IrqHandler handler) {
        ASSERT_MSG(static_cast<u32>(irq) <= static_cast<u32>(InterruptId::InterruptIdMax),
                   "Invalid IRQ number");
        auto& ctx = irq_contexts[static_cast<u32>(irq)];
        std::unique_lock lock{ctx.m_lock};
        ctx.one_time_subscribers.emplace(handler);
    }

    void Register(InterruptId irq, IrqHandler handler, void* uid) {
        ASSERT_MSG(static_cast<u32>(irq) <= static_cast<u32>(InterruptId::InterruptIdMax),
                   "Invalid IRQ number");
        auto& ctx = irq_contexts[static_cast<u32>(irq)];

        std::unique_lock lock{ctx.m_lock};
        ASSERT_MSG(ctx.persistent_handlers.find(uid) == ctx.persistent_handlers.cend(),
                   "The handler is already registered!");
        ctx.persistent_handlers.emplace(uid, handler);
    }

    void Unregister(InterruptId irq, void* uid) {
        ASSERT_MSG(static_cast<u32>(irq) <= static_cast<u32>(InterruptId::InterruptIdMax),
                   "Invalid IRQ number");
        auto& ctx = irq_contexts[static_cast<u32>(irq)];
        std::unique_lock lock{ctx.m_lock};
        ctx.persistent_handlers.erase(uid);
    }

    void Signal(InterruptId irq) {
        ASSERT_MSG(static_cast<u32>(irq) <= static_cast<u32>(InterruptId::InterruptIdMax),
                   "Unexpected IRQ signaled");
        // gow3 19 Oct 2026: a handler can raise an interrupt itself. The end-of-pipe flip
        // handler presents (Presenter::PrepareFrame), whose wait for the GPU lands fences held
        // for ray-cast readbacks, and one of those signalled GfxEop again on this thread: the
        // second lock of the same mutex froze Uncharted 3 on the title screen. An interrupt
        // raised inside a handler is delivered right after that handler, on the same thread
        // and in the order raised.
        thread_local u32 depth = 0;
        thread_local std::vector<InterruptId> raised_inside;
        if (depth) {
            raised_inside.push_back(irq);
            return;
        }
        ++depth;
        Deliver(irq);
        for (size_t i = 0; i < raised_inside.size(); ++i) {
            Deliver(raised_inside[i]);
        }
        raised_inside.clear();
        --depth;
    }

    /// gow3 18 Oct 2026: Signal on the interrupt delivery thread, in the order queued. The
    /// handlers wake the guest's interrupt threads (equeue events, a kernel call each): Uncharted
    /// 3 raises ~2,200 compute interrupts a frame, which the GPU command thread paid for. Labels
    /// and fences are written before the interrupt is queued, as before. Signals
    /// in place (the default: no measured gain on 4 or 16 cores, 18 Oct; GOW3_ASYNC_IRQ=1 on).
    void SignalAsync(InterruptId irq) {
        static const bool enabled = [] {
            const char* env = std::getenv("GOW3_ASYNC_IRQ");
            return env && env[0] == '1';
        }();
        if (!enabled) {
            Signal(irq);
            return;
        }
        std::call_once(async_started, [this] {
            std::thread([this] { DeliverAsync(); }).detach();
        });
        {
            std::scoped_lock lock{async_mutex};
            async_queue.push_back(irq);
            if (!async_waiting) {
                return; // the delivery thread is awake and takes it with the rest
            }
        }
        async_cv.notify_one();
    }

private:
    void Deliver(InterruptId irq) {
        auto& ctx = irq_contexts[static_cast<u32>(irq)];
        std::unique_lock lock{ctx.m_lock};

        LOG_TRACE(Core, "IRQ signaled: {}", magic_enum::enum_name(irq));

        for (auto& [uid, h] : ctx.persistent_handlers) {
            h(irq);
        }

        if (!ctx.one_time_subscribers.empty()) {
            const auto& h = ctx.one_time_subscribers.front();
            h(irq);

            ctx.one_time_subscribers.pop();
        }
    }

    void DeliverAsync() {
        Common::SetCurrentThreadName("gow3:Irq");
        std::vector<InterruptId> batch;
        while (true) {
            {
                std::unique_lock lock{async_mutex};
                while (async_queue.empty()) {
                    async_waiting = true;
                    async_cv.wait(lock);
                    async_waiting = false;
                }
                batch.swap(async_queue);
            }
            for (const InterruptId irq : batch) {
                Signal(irq);
            }
            batch.clear();
        }
    }

    std::once_flag async_started;
    std::mutex async_mutex;
    std::condition_variable async_cv;
    std::vector<InterruptId> async_queue;
    bool async_waiting = false;

    struct IrqContext {
        std::unordered_map<void*, IrqHandler> persistent_handlers{};
        std::queue<IrqHandler> one_time_subscribers{};
        std::mutex m_lock{};
    };
    std::array<IrqContext,static_cast<u32>(InterruptId::InterruptIdMax)+1> irq_contexts{};
};

using IrqC = Common::Singleton<IrqController>;

} // namespace Platform
