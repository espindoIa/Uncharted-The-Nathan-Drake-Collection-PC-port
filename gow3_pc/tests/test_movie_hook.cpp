// Uncharted movie frame pacing (src/movie_hook.h): the stub on a fake player and game clock at
// several frame rates, with frame-time jitter; then the signatures on the three games' code
// (analysis/<game>/boot-linked.bin, when present).
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <vector>
#include <windows.h>
#include "../src/movie_hook.h"

alignas(16) static unsigned char player[0x22600];
alignas(16) static unsigned char game_clock[0x100];

// The site's registers: eax the counter, rbx the player, rcx the clock.
static uint32_t step(unsigned char* stub, uint32_t counter) {
    uint32_t out;
    asm volatile("push %%rbx\n\t"
                 "mov %3, %%rbx\n\t"
                 "call *%2\n\t"
                 "pop %%rbx\n\t"
                 : "=a"(out)
                 : "a"(counter), "r"(stub), "r"(player), "c"(game_clock)
                 : "rdx", "xmm0", "memory", "cc");
    return out;
}

// Frames shown in `seconds` at a frame-limited `fps`: frame i starts at i/fps plus up to `jitter`
// seconds of noise (the limiter keeps the grid, so frame times are not a random walk).
static uint32_t run(unsigned char* stub, float* data, double fps, double seconds, double jitter, unsigned seed) {
    gow3_movie_init_data(data);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> noise(-jitter, jitter);
    uint32_t counter = 0;
    std::memset(player, 0, sizeof(player));
    const int frames = int(std::lround(fps * seconds));
    double last = 0.0;
    for (int i = 0; i < frames; ++i) {
        const double start = (i + 1) / fps + noise(rng);
        const float dt = float(start - last);
        last = start;
        std::memcpy(game_clock + 0x9c, &dt, 4);
        const uint32_t next = step(stub, counter);
        uint32_t stored;
        std::memcpy(&stored, player + 0x2254c, 4);
        assert(stored == next && (next == counter || next == counter + 1));
        counter = next;
    }
    return counter;
}

static std::vector<unsigned char> load(const char* path, size_t header) {
    std::ifstream f(path, std::ios::binary);
    std::vector<unsigned char> b{std::istreambuf_iterator<char>(f), {}};
    if (b.size() <= header) return {};
    return {b.begin() + std::ptrdiff_t(header), b.end()};
}

int main() {
    auto* stub = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    assert(stub);
    alignas(16) static float data[4];
    const unsigned n = gow3_movie_build_stub(stub, 0x9c, reinterpret_cast<uintptr_t>(data));
    assert(n == 52 && stub[n - 1] == 0xc3);

    // At or below 60 FPS every game frame still shows the next frame (the original behaviour),
    // also with +-3 ms of frame-time noise at 60 FPS.
    assert(run(stub, data, 60.0, 10.0, 0.0, 1) == 600);
    assert(run(stub, data, 60.0, 10.0, 0.003, 2) == 600);
    assert(run(stub, data, 45.0, 10.0, 0.002, 3) == 450);
    assert(run(stub, data, 30.0, 10.0, 0.002, 4) == 300);
    // Above 60 FPS the movie keeps 60 frames per second of game time.
    for (const double fps : {75.0, 90.0, 120.0, 144.0, 165.0, 240.0}) {
        const uint32_t shown = run(stub, data, fps, 10.0, 0.0005, unsigned(fps));
        std::printf("%.0f FPS: %u frames in 10 s\n", fps, shown);
        assert(shown >= 599 && shown <= 601);
    }
    // A paused game clock (dt 0) never advances it.
    gow3_movie_init_data(data);
    const float zero = 0.0f;
    std::memcpy(game_clock + 0x9c, &zero, 4);
    for (int i = 0; i < 100; ++i) assert(step(stub, 7) == 7);

    unsigned char call[GOW3_MOVIE_HOOK_LENGTH];
    gow3_movie_build_call(call, 0x12345678);
    assert(call[0] == 0xe8 && call[5] == 0x0f && call[6] == 0x1f && call[7] == 0x00);

    // The three games: one site each, the original step there, the clock's dt offset.
    struct Game { const char* path; size_t header; uint64_t site; int32_t dt; };
    const Game games[] = {
        {"../analysis/uncharted/boot-linked.bin", 0xb6bc8, 0x4a0e62, 0x6c},
        {"../analysis/big2-ps4_Shipping/boot-linked.bin", 0x183ce8, 0xb84805, 0x8c},
        {"../analysis/big3-ps4_Shipping/boot-linked.bin", 0x255e60, 0xdefd55, 0x9c},
    };
    for (const auto& g : games) {
        const auto image = load(g.path, g.header);
        if (image.empty()) {
            std::printf("%s: not present, skipped\n", g.path);
            continue;
        }
        int32_t dt = 0;
        const uint64_t at = gow3_movie_find(image.data(), image.size(), &dt);
        std::printf("%s: step at %#llx, clock dt +%#x\n", g.path, (unsigned long long)at, unsigned(dt));
        assert(at == g.site && dt == g.dt);
        assert(!std::memcmp(image.data() + at, gow3_movie_step_original, GOW3_MOVIE_HOOK_LENGTH));
    }
    VirtualFree(stub, 0, MEM_RELEASE);
    std::puts("movie hook ok");
    return 0;
}
