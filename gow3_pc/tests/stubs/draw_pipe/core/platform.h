// Test stand-in for shadps4/core/platform.h (tests/test_draw_pipe.cpp): only the IRQ controller
// DrawPipe calls (FlushHeld), counting the calls; no logging or singleton backend to link.
#pragma once
#include <atomic>
namespace Platform {
struct IrqController {
    std::atomic<unsigned long> flushes{0};
    void FlushHeld() {
        flushes.fetch_add(1, std::memory_order_relaxed);
    }
};
struct IrqC {
    static IrqController* Instance() {
        static IrqController controller;
        return &controller;
    }
};
} // namespace Platform
