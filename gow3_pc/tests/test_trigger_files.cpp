// SPDX-License-Identifier: GPL-2.0-or-later
// Trigger files: the watcher thread finds a created file, hands over its first line once and
// removes it; unset variables give no slot.
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "../gpu/shim/gow3_trigger_files.h"

static bool WaitTaken(int slot, std::string& text) {
    for (int i = 0; i < 40; ++i) { // 2 s; the watcher polls every 250 ms
        if (auto taken = Gow3TriggerFiles::Take(slot)) {
            text = *taken;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

int main() {
    const std::string path = "trigger-files-test.trigger";
    std::remove(path.c_str());
#ifdef _WIN32
    _putenv_s("GOW3_TEST_TRIGGER", path.c_str());
#else
    setenv("GOW3_TEST_TRIGGER", path.c_str(), 1);
#endif
    assert(Gow3TriggerFiles::Watch("GOW3_TEST_TRIGGER_UNSET") == -1);
    assert(!Gow3TriggerFiles::Take(-1));
    const int slot = Gow3TriggerFiles::Watch("GOW3_TEST_TRIGGER");
    assert(slot >= 0);
    assert(Gow3TriggerFiles::Watch("GOW3_TEST_TRIGGER") == slot);
    assert(!Gow3TriggerFiles::Take(slot));

    FILE* f = std::fopen(path.c_str(), "w");
    assert(f);
    std::fputs("range 1000 2000\nsecond line\n", f);
    std::fclose(f);
    std::string text;
    assert(WaitTaken(slot, text));
    assert(text == "range 1000 2000\n");
    assert(!Gow3TriggerFiles::Take(slot));
    assert(!std::fopen(path.c_str(), "r")); // removed by the watcher

    // An empty file still triggers (the frame dump trigger is created empty).
    f = std::fopen(path.c_str(), "w");
    assert(f);
    std::fclose(f);
    assert(WaitTaken(slot, text) && text.empty());
    std::puts("trigger-files-test: ok");
    return 0;
}
