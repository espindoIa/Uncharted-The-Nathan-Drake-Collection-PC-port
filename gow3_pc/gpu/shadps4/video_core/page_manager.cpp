// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>
#include <boost/container/small_vector.hpp>
#include "gow3_toggles.h"
#include "common/assert.h"
#include "core/platform.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/error.h"
#include "common/range_lock.h"
#include "common/signal_context.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/signals.h"
#include "video_core/page_manager.h"
#include "video_core/page_state.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

#ifndef _WIN64
#include <sys/mman.h>
#include "common/adaptive_mutex.h"
#else
#include <windows.h>
#endif

#ifdef __linux__
#include <thread>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <unordered_set>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#else
#include "common/spin_lock.h"
#endif

namespace VideoCore {

constexpr size_t PM_PAGE_SIZE = 4_KB;
constexpr size_t PM_PAGE_BITS = 12;

// gow3 21 Oct 2026: file-read ranges prepared by Rasterizer::PrepareCpuWrite (count, bytes, ns),
// reported by the 5 s fault stats line; taken (and cleared) there.
std::atomic<u64> g_range_prepares{0}, g_range_prepare_bytes{0}, g_range_prepare_ns{0},
    g_range_prepare_pages{0}, g_range_prepare_discarded{0};

// gow3 21 Oct 2026: Rasterizer::HandleWriteAt's time split (GOW3_FAULT_STATS): ns in buffer-cache
// invalidate, texture-cache part, ExtendWriteFault, LandWatchedReadback; and the call count.
std::atomic<u64> g_wf_split_ns[4], g_wf_split_calls{0};

struct PageManager::Impl {
    using PageState = VideoCore::PageState;

    static constexpr size_t ADDRESS_BITS = 40;
    static constexpr size_t NUM_ADDRESS_PAGES = 1ULL << (40 - PM_PAGE_BITS);
    static constexpr size_t NUM_ADDRESS_LOCKS = NUM_ADDRESS_PAGES / PAGES_PER_LOCK;
    inline static Vulkan::Rasterizer* rasterizer;
    /// gow3: the instance, for the fault handler's look at a page's read watchers.
    inline static Impl* self = nullptr;

    Impl() {
        self = this;
    }
    virtual ~Impl() = default;

    static bool AnyWatched(VAddr addr, u64 size) {
        if (!self) {
            return false;
        }
        const u64 last = (addr + size - 1) >> PM_PAGE_BITS;
        if (last >= NUM_ADDRESS_PAGES) {
            return false;
        }
        for (u64 page = addr >> PM_PAGE_BITS; page <= last; ++page) {
            const auto& state = self->cached_pages[page];
            if (__atomic_load_n(&state.num_write_watchers, __ATOMIC_RELAXED) |
                __atomic_load_n(&state.num_read_watchers, __ATOMIC_RELAXED)) {
                return true;
            }
        }
        return false;
    }

    /// Write-watched and not read-watched: a store faults on the write branch alone.
    static bool OnlyWriteWatched(VAddr addr) {
        if (!self || (addr >> PM_PAGE_BITS) >= NUM_ADDRESS_PAGES) {
            return false;
        }
        const auto& state = self->cached_pages[addr >> PM_PAGE_BITS];
        return __atomic_load_n(&state.num_write_watchers, __ATOMIC_RELAXED) != 0 &&
               __atomic_load_n(&state.num_read_watchers, __ATOMIC_RELAXED) == 0;
    }

    virtual void OnMap(VAddr address, size_t size) {
        // No-op
    }

    virtual void OnUnmap(VAddr address, size_t size) {
        // No-op
    }

    virtual void Protect(VAddr address, size_t size, Core::MemoryPermission perms) = 0;

    // gow3: GOW3_FAULT_STATS=1 prints, every 5 s, the CPU read faults on GPU-modified pages
    // (each one waits for the GPU and downloads a window): count and time by thread kind, and the
    // hottest faulting instructions and 64 KiB windows. Input for frame drops (Uncharted temple).
    static void NoteReadFault(void* context, VAddr addr, bool gpu_thread,
                              std::chrono::steady_clock::duration took, bool write = false) {
        static const bool enabled = [] {
            const char* env = std::getenv("GOW3_FAULT_STATS");
            return env && env[0] == '1';
        }();
        if (!enabled) {
            return;
        }
        u64 rip = 0;
#ifdef _WIN32
        rip = static_cast<EXCEPTION_POINTERS*>(context)->ContextRecord->Rip;
        // Runtime code: as an offset in the executable (symbolize with llvm-symbolizer at
        // 0x140000000 + offset); guest code stays absolute.
        static const u64 exe_base = reinterpret_cast<u64>(GetModuleHandleW(nullptr));
        if (rip >= exe_base && rip < exe_base + 0x20000000) {
            rip = rip - exe_base + 0x140000000;
        }
#endif
        static std::mutex mutex;
        static std::unordered_map<u64, u32> by_rip, by_window;
        static u64 count[2], ns[2], write_count, write_ns;
        static auto last = std::chrono::steady_clock::now();
        std::scoped_lock lock{mutex};
        // Write faults that had to download GPU data are keyed separately (top bit).
        ++by_rip[rip | (write ? 1ULL << 63 : 0)];
        ++by_window[(addr >> 16) | (write ? 1ULL << 47 : 0)];
        const u64 took_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(took).count();
        if (write) {
            ++write_count;
            write_ns += took_ns;
        } else {
            ++count[gpu_thread];
            ns[gpu_thread] += took_ns;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::seconds{5}) {
            return;
        }
        last = now;
        const auto top = [](const std::unordered_map<u64, u32>& map) {
            std::vector<std::pair<u32, u64>> sorted;
            for (const auto& [key, n] : map) {
                sorted.emplace_back(n, key);
            }
            std::ranges::sort(sorted, std::greater{});
            sorted.resize(std::min<size_t>(sorted.size(), 8));
            return sorted;
        };
        std::printf("Fault stats (5 s): guest-thread read faults %llu (%.1f ms), GPU-side %llu (%.1f ms), "
                    "writes into GPU-written memory %llu (%.1f ms), file-read ranges prepared %llu "
                    "(%.1f MB, %llu pages, %llu discarded, %.1f ms)\n",
                    (unsigned long long)count[0], ns[0] / 1e6, (unsigned long long)count[1], ns[1] / 1e6,
                    (unsigned long long)write_count, write_ns / 1e6,
                    (unsigned long long)g_range_prepares.exchange(0),
                    g_range_prepare_bytes.exchange(0) / 1048576.0,
                    (unsigned long long)g_range_prepare_pages.exchange(0),
                    (unsigned long long)g_range_prepare_discarded.exchange(0),
                    g_range_prepare_ns.exchange(0) / 1e6);
        for (const auto& [n, key] : top(by_rip)) {
            std::printf("  %s rip %#llx: %u\n", key >> 63 ? "write" : "read",
                        (unsigned long long)(key & ~(1ULL << 63)), n);
        }
        for (const auto& [n, key] : top(by_window)) {
            std::printf("  %s window %#llx: %u\n", (key >> 47) & 1 ? "write" : "read",
                        (unsigned long long)((key & ~(1ULL << 47)) << 16), n);
        }
        std::fflush(stdout);
        by_rip.clear();
        by_window.clear();
        count[0] = count[1] = ns[0] = ns[1] = write_count = write_ns = 0;
    }

    // gow3 21 Oct 2026: every write fault (not just the ones that download), by 4 KiB page and by
    // RIP, plus the HandleWriteAt time split, printed every 5 s (GOW3_FAULT_STATS=1). Why: ~1,600
    // ordinary write faults/s standing in U3, cause unknown.
    static void NoteAnyWriteFault(void* context, VAddr addr) {
        static const bool enabled = [] {
            const char* env = std::getenv("GOW3_FAULT_STATS");
            return env && env[0] == '1';
        }();
        if (!enabled) {
            return;
        }
        u64 rip = 0;
#ifdef _WIN32
        rip = static_cast<EXCEPTION_POINTERS*>(context)->ContextRecord->Rip;
        static const u64 exe_base = reinterpret_cast<u64>(GetModuleHandleW(nullptr));
        if (rip >= exe_base && rip < exe_base + 0x20000000) {
            rip = rip - exe_base + 0x140000000;
        }
#endif
        static std::mutex mutex;
        static std::unordered_map<u64, u32> by_rip, by_page;
        static auto last = std::chrono::steady_clock::now();
        std::scoped_lock lock{mutex};
        ++by_rip[rip];
        ++by_page[addr >> PM_PAGE_BITS];
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::seconds{5}) {
            return;
        }
        last = now;
        const auto top = [](const std::unordered_map<u64, u32>& map) {
            std::vector<std::pair<u32, u64>> sorted;
            for (const auto& [key, n] : map) {
                sorted.emplace_back(n, key);
            }
            std::ranges::sort(sorted, std::greater{});
            sorted.resize(std::min<size_t>(sorted.size(), 8));
            return sorted;
        };
        const auto ms = [](int i) { return g_wf_split_ns[i].exchange(0) / 1e6; };
        const double buffer = ms(0), texture = ms(1), extend = ms(2), landing = ms(3);
        std::printf("Write fault split (5 s): %llu calls; buffer %.1f ms, texture %.1f ms, extend "
                    "%.1f ms, landing %.1f ms\n",
                    (unsigned long long)g_wf_split_calls.exchange(0), buffer, texture, extend,
                    landing);
        {
            using namespace Gow3Stats;
            const auto fms = [](int i) { return ff_ns[i].exchange(0) / 1e6; };
            const auto fn = [](int i) { return (unsigned long long)ff_n[i].exchange(0); };
            const double total = ff_total_ns.exchange(0) / 1e6;
            const double rb = fms(FF_RBWAIT), early = fms(FF_EARLY), fetch = fms(FF_FETCH),
                         lazy = fms(FF_LAZY), lazyw = fms(FF_LAZYWRITE), pre = fms(FF_PREWAIT),
                         down = fms(FF_DOWNLOAD), other = fms(FF_OTHER);
            std::printf("Fault flush (5 s): %llu calls %.1f ms; early %llu/%.1f ms, lazy %llu/%.1f ms, "
                        "lazywrite %llu/%.1f ms, prewait %llu/%.1f ms (waited %.1f ms), download "
                        "%llu/%.1f ms, other %llu/%.1f ms; also readback-complete %llu/%.1f ms, "
                        "page fetch %llu/%.1f ms; not in any phase (queue to GPU thread, etc.) %.1f ms, "
                        "prewait skipped %llu\n",
                        (unsigned long long)ff_total_n.exchange(0), total, fn(FF_EARLY), early,
                        fn(FF_LAZY), lazy, fn(FF_LAZYWRITE), lazyw, fn(FF_PREWAIT), pre,
                        ff_prewait_wait_ns.exchange(0) / 1e6, fn(FF_DOWNLOAD), down, fn(FF_OTHER),
                        other, fn(FF_RBWAIT), rb, fn(FF_FETCH), fetch,
                        total - (rb + early + fetch + lazy + lazyw + pre + down + other),
                        (unsigned long long)ff_prewait_skipped.exchange(0));
        }
        std::printf("buffer lock wait %.1f ms, under lock %.1f ms\n",
                    Gow3Stats::buffer_lock_wait_ns.exchange(0) / 1e6,
                    Gow3Stats::buffer_under_lock_ns.exchange(0) / 1e6);
        std::string pages = "Write fault pages (5 s):";
        for (const auto& [n, key] : top(by_page)) {
            char text[48];
            std::snprintf(text, sizeof(text), " %#llx:%u", (unsigned long long)(key << PM_PAGE_BITS), n);
            pages += text;
        }
        std::string rips = "Write fault rips (5 s):";
        for (const auto& [n, key] : top(by_rip)) {
            char text[48];
            std::snprintf(text, sizeof(text), " %#llx:%u", (unsigned long long)key, n);
            rips += text;
        }
        std::printf("%s\n%s\n", pages.c_str(), rips.c_str());
        std::fflush(stdout);
        by_rip.clear();
        by_page.clear();
    }

    static bool GuestFaultSignalHandler(void* context, void* fault_address) {
        const auto addr = reinterpret_cast<VAddr>(fault_address);
        // gow3: the draw recording thread handles its faults inline too (vk_draw_pipe.h).
        const auto is_gpu_thread = rasterizer->IsGpuSideThread();
        if (is_gpu_thread) {
            Gow3Stats::gpu_signal_faults.fetch_add(1, std::memory_order_relaxed);
        }
        // Interrupts of fences landed here wait for this thread's next safe point (it may hold
        // cache locks the interrupt handlers take: IrqController::HoldScope).
        std::optional<Platform::IrqController::HoldScope> hold_interrupts;
        if (is_gpu_thread) {
            hold_interrupts.emplace();
        }
        // gow3 18 Oct 2026: an instruction that keeps faulting on one page never gets past it
        // (Uncharted 3 rooftops: a read-modify-write into a page a readback still protected, the
        // game frozen; shorter loops were hitches). After 4 faults in a row on the same page the
        // readbacks holding it land (Rasterizer::ReleaseStuckPage).
        thread_local VAddr last_fault_page = 0;
        thread_local u32 fault_repeats = 0;
        const VAddr fault_page = addr & ~VAddr(4095);
        if (fault_page != last_fault_page) {
            last_fault_page = fault_page;
            fault_repeats = 0;
        } else if (++fault_repeats >= 4 && self && (addr >> PM_PAGE_BITS) < NUM_ADDRESS_PAGES &&
                   self->cached_pages[addr >> PM_PAGE_BITS].num_read_watchers != 0) {
            // (Only guest addresses: the SRT walker's garbage pointers fault at -1, reported for
            // non-canonical addresses; indexing the page table with that faulted inside this
            // handler, again and again: Uncharted 3 froze, 18 Oct evening.)
            // Only a read-protected page (a readback holds it) can keep an instruction faulting;
            // write faults repeat on hot pages the GPU keeps writing (not stuck).
            fault_repeats = 0;
            rasterizer->ReleaseStuckPage(fault_page, Common::IsWriteError(context), is_gpu_thread);
        }
        if (Common::IsWriteError(context)) {
            Gow3Stats::Timer timer{Gow3Stats::t_write_faults};
            if (!is_gpu_thread) {
                NoteAnyWriteFault(context, addr);
            }
            const u64 downloads = gow3_downloads;
            const auto start = std::chrono::steady_clock::now();
            const bool handled = rasterizer->OnWriteFault(addr, is_gpu_thread);
            if (gow3_downloads != downloads || is_gpu_thread) {
                // A CPU write into GPU-written memory: the window was downloaded first (and every
                // GPU-side thread write fault: the runtime writing guest memory itself).
                NoteReadFault(context, addr, is_gpu_thread, std::chrono::steady_clock::now() - start,
                              true);
            }
            return handled;
        } else {
            Gow3Stats::read_faults.fetch_add(1, std::memory_order_relaxed);
            Gow3Stats::Timer timer{Gow3Stats::t_read_faults};
            const auto start = std::chrono::steady_clock::now();
            const bool handled = rasterizer->ReadMemory(addr, 8, is_gpu_thread);
            NoteReadFault(context, addr, is_gpu_thread, std::chrono::steady_clock::now() - start);
            return handled;
        }
        return false;
    }

    template <bool track, bool is_read>
    void UpdatePageWatchers(VAddr addr, u64 size) {
        RENDERER_TRACE;

        size_t page = addr >> PM_PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PM_PAGE_SIZE);

        // Acquire locks for the range of pages
        const auto lock_start = locks.begin() + (page / PAGES_PER_LOCK);
        const auto lock_end = locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);

        auto perms = cached_pages[page].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                Protect(range_begin << PM_PAGE_BITS, range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate requested pages
        const u64 aligned_addr = page << PM_PAGE_BITS;
        const u64 aligned_end = page_end << PM_PAGE_BITS;
        if (!rasterizer->IsMapped(aligned_addr, aligned_end - aligned_addr)) {
            LOG_WARNING(Render,
                        "Tracking memory region {:#x} - {:#x} which is not fully GPU mapped.",
                        aligned_addr, aligned_end);
        }

        for (; page != page_end; ++page) {
            PageState& state = cached_pages[page];

            // Apply the change to the page state
            const u8 new_count = state.AddDelta<track ? 1 : -1, is_read>();

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // Only start a new range if the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current range up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    template <bool track, bool is_read>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) {
        RENDERER_TRACE;
        auto start_range = mask.FirstRange();
        auto end_range = mask.LastRange();

        if (start_range.second == end_range.second) {
            // if all pages are contiguous, use the regular UpdatePageWatchers
            const VAddr start_addr = base_addr + (start_range.first << PM_PAGE_BITS);
            const u64 size = (start_range.second - start_range.first) << PM_PAGE_BITS;
            return UpdatePageWatchers<track, is_read>(start_addr, size);
        }

        size_t base_page = (base_addr >> PM_PAGE_BITS);
        ASSERT(base_page % PAGES_PER_LOCK == 0);
        std::scoped_lock lk(locks[base_page / PAGES_PER_LOCK]);
        auto perms = cached_pages[base_page + start_range.first].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                Protect((range_begin << PM_PAGE_BITS), range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate pages
        for (size_t page = start_range.first; page < end_range.second; ++page) {
            PageState& state = cached_pages[base_page + page];
            const bool update = mask.Get(page);

            // Apply the change to the page state
            const u8 new_count =
                update ? state.AddDelta<track ? 1 : -1, is_read>() : state.AddDelta<0, is_read>();

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // If the page is not being updated, skip it
            if (!update) {
                continue;
            }

            // If the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = base_page + page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current rango up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    std::array<PageState, NUM_ADDRESS_PAGES> cached_pages{};
#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
    using LockType = Common::AdaptiveMutex;
#else
    using LockType = Common::SpinLock;
#endif
    std::array<LockType, NUM_ADDRESS_LOCKS> locks{};
};

#ifdef __linux__
// gow3: write tracking with userfaultfd write-protection instead of mprotect. mprotect takes
// the address space lock for writing and splits mappings; while guest threads fault on
// per-frame buffers and the GPU thread re-protects uploaded pages, every page fault in the
// process (copy threads included) waits for it. UFFDIO_WRITEPROTECT changes page table bits
// under the lock for reading. Read protection (readbacks) still uses mprotect: userfaultfd
// write-protection cannot deny reads. GOW3_UFFD=1.
struct UffdImpl : public PageManager::Impl {
private:
    std::jthread ufd_thread;
    int uffd;
    std::mutex read_revoked_mutex;
    std::unordered_set<u64> read_revoked_pages; ///< 4 KiB pages denied reads with mprotect
    std::atomic<size_t> num_read_revoked{0};

public:
    UffdImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;
        uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
        if (uffd == -1) {
            LOG_ERROR(Common_Memory,
                      "userfaultfd syscall failed: {}, falling back to signal implementation",
                      Common::GetLastErrorMsg());
            throw std::runtime_error("userfaultfd");
        }

        // Guest memory is a shared memfd mapping: write-protection there needs the shmem
        // feature, and unpopulated pages must be protectable too.
        uffdio_api api{};
        api.api = UFFD_API;
        api.features =
            UFFD_FEATURE_THREAD_ID | UFFD_FEATURE_WP_HUGETLBFS_SHMEM | UFFD_FEATURE_WP_UNPOPULATED;
        if (ioctl(uffd, UFFDIO_API, &api) != 0) {
            LOG_ERROR(Common_Memory,
                      "uffdio_api call failed: {}, falling back to signal implementation",
                      Common::GetLastErrorMsg());
            close(uffd);
            throw std::runtime_error("uffdio_api");
        }

        // Read faults (readbacks) still arrive as signals.
        Core::Signals::Instance()->RegisterAccessViolationHandler(
            GuestFaultSignalHandler, std::numeric_limits<u32>::min());

        ufd_thread = std::jthread([this](std::stop_token token) { UffdHandler(token); });
        std::printf("GPU: memory tracking with userfaultfd write-protection\n");
    }

    ~UffdImpl() = default;

    void OnMap(VAddr address, size_t size) override {
        uffdio_register reg{};
        reg.range.start = address;
        reg.range.len = size;
        reg.mode = UFFDIO_REGISTER_MODE_WP;
        if (ioctl(uffd, UFFDIO_REGISTER, &reg) == -1) {
            LOG_ERROR(Common_Memory, "Uffdio register {:#x}+{:#x} failed: {}", address, size,
                      Common::GetLastErrorMsg());
        }
    }

    void OnUnmap(VAddr address, size_t size) override {
        uffdio_range range{};
        range.start = address;
        range.len = size;
        ioctl(uffd, UFFDIO_UNREGISTER, &range);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        auto& address_space = Core::Memory::Instance()->GetAddressSpace();
        const bool allow_read = True(perms & Core::MemoryPermission::Read);
        const bool allow_write = True(perms & Core::MemoryPermission::Write);
        const u64 first = address >> 12, last = (address + size + 4095) >> 12;
        if (!allow_read) {
            // Readbacks: deny all access with mprotect.
            address_space.Protect(address, size, Core::MemoryPermission::None);
            std::scoped_lock lk{read_revoked_mutex};
            for (u64 page = first; page < last; ++page) {
                read_revoked_pages.insert(page);
            }
            num_read_revoked.store(read_revoked_pages.size(), std::memory_order_release);
            return;
        }
        if (num_read_revoked.load(std::memory_order_acquire) != 0) {
            std::scoped_lock lk{read_revoked_mutex};
            bool restore = false;
            for (u64 page = first; page < last; ++page) {
                restore |= read_revoked_pages.erase(page) != 0;
            }
            num_read_revoked.store(read_revoked_pages.size(), std::memory_order_release);
            if (restore) {
                address_space.Protect(address, size, Core::MemoryPermission::ReadWrite);
            }
        }
        uffdio_writeprotect wp{};
        wp.range.start = address;
        wp.range.len = size;
        wp.mode = allow_write ? UFFDIO_WRITEPROTECT_MODE_DONTWAKE : UFFDIO_WRITEPROTECT_MODE_WP;
        if (ioctl(uffd, UFFDIO_WRITEPROTECT, &wp) == -1) {
            LOG_ERROR(Common_Memory, "Uffdio writeprotect {:#x}+{:#x} failed: {}", address, size,
                      Common::GetLastErrorMsg());
        }
    }

    void UffdHandler(std::stop_token token) {
        Common::SetCurrentThreadName("gow3:Uffd");
        while (!token.stop_requested()) {
            pollfd pollfd{};
            pollfd.fd = uffd;
            pollfd.events = POLLIN;
            // Short timeout so the stop request is seen.
            const int pollres = poll(&pollfd, 1, 100);
            if (pollres <= 0 || !(pollfd.revents & POLLIN)) {
                continue;
            }
            uffd_msg msg;
            const int readret = read(uffd, &msg, sizeof(msg));
            if (readret != sizeof(msg)) {
                continue;
            }
            if (msg.event != UFFD_EVENT_PAGEFAULT) {
                continue;
            }
            const VAddr addr = msg.arg.pagefault.address;
            const auto ptid = msg.arg.pagefault.feat.ptid;
            {
                Gow3Stats::Timer timer{Gow3Stats::t_write_faults};
                rasterizer->OnWriteFault(addr, rasterizer->IsGpuSideThreadId(ptid));
            }
            // Protect() clears with DONTWAKE (it may run for pages nobody waits on).
            uffdio_range wake{};
            wake.start = addr & ~u64(PM_PAGE_SIZE - 1);
            wake.len = PM_PAGE_SIZE;
            ioctl(uffd, UFFDIO_WAKE, &wake);
        }
    }
};
#endif // __linux__

struct SignalImpl : public PageManager::Impl {
    SignalImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;

        // Should be called first.
        constexpr auto priority = std::numeric_limits<u32>::min();
        Core::Signals::Instance()->RegisterAccessViolationHandler(GuestFaultSignalHandler,
                                                                  priority);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        RENDERER_TRACE;
        auto* memory = Core::Memory::Instance();
        auto& impl = memory->GetAddressSpace();
        ASSERT_MSG(perms != Core::MemoryPermission::Write,
                   "Attempted to protect region as write-only which is not a valid permission");
        impl.Protect(address, size, perms);
    }

};

PageManager::PageManager(Vulkan::Rasterizer* rasterizer_) {
#ifdef __linux__
    if (std::getenv("GOW3_UFFD") && std::getenv("GOW3_UFFD")[0] == '1') {
        try {
            impl = std::make_unique<UffdImpl>(rasterizer_);
            LOG_INFO(Config, "Memory tracking method: userfaultfd");
            return;
        } catch (const std::runtime_error& e) {
            // if uffd is unsupported, falls back to SignalImpl
        }
    }
    LOG_INFO(Config, "Memory tracking method: signals");
#endif
    impl = std::make_unique<SignalImpl>(rasterizer_);
}

PageManager::~PageManager() = default;

void PageManager::OnGpuMap(VAddr address, size_t size) {
    impl->OnMap(address, size);
}

void PageManager::OnGpuUnmap(VAddr address, size_t size) {
    impl->OnUnmap(address, size);
}

// gow3 21 Oct 2026: true when any page of the range is write- or read-watched (protected); the
// cheap test of Rasterizer::PrepareCpuWrite for file reads. Lock-free hint, like IsReadWatched.
bool Gow3AnyPageWatched(VAddr addr, u64 size) {
    return SignalImpl::AnyWatched(addr, size); // (Impl is private: a friend names it)
}

bool Gow3PageOnlyWriteWatched(VAddr addr) {
    return SignalImpl::OnlyWriteWatched(addr);
}

bool PageManager::IsReadWatched(VAddr addr) const {
    const u64 page = addr >> PM_PAGE_BITS;
    if (page >= Impl::NUM_ADDRESS_PAGES) {
        return false;
    }
    return __atomic_load_n(&impl->cached_pages[page].num_read_watchers, __ATOMIC_RELAXED) != 0;
}

template <bool track, bool is_read>
void PageManager::UpdatePageWatchers(VAddr addr, u64 size) const {
    impl->UpdatePageWatchers<track, is_read>(addr, size);
}

template <bool track, bool is_read>
void PageManager::UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) const {
    impl->UpdatePageWatchersForRegion<track, is_read>(base_addr, mask);
}

template void PageManager::UpdatePageWatchers<true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<false>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<true, true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<false, true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchersForRegion<true, true>(VAddr base_addr,
                                                                   RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<true, false>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, true>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, false>(VAddr base_addr,
                                                                     RegionBits& mask) const;

} // namespace VideoCore
