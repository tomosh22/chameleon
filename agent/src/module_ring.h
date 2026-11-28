#pragma once

// WP-48 (ADR-0044): the ring the loader notification callback records module loads and unloads in.
//
// The callback (agent.cpp, registered with LdrRegisterDllNotification) runs under the loader lock, on whatever thread is loading
// or unloading a DLL. There it may only append to this ring: it never calls the loader, allocates, takes a lock, logs or waits.
// So the ring is
//   * preallocated and fixed in size (kModuleRingCapacity slots, each with room for a path of kRawPathUnits UTF-16 units);
//   * lock-free for producers (a bounded multi-producer queue: one compare-and-swap claims a slot, one release store publishes
//     it). The loader lock serialises the callbacks in practice; the ring does not rely on that;
//   * drained by one consumer at a time, the agent thread's module poll (any thread may be the consumer, never two at once).
// A full ring drops the new event and counts it (lost()): the poll then completes its report from a module-list diff (CML4721).
// Each event that gets a slot gets the next sequence number of the ring (1, 2, ...): consumption order is sequence order.
//
// Portable (std::atomic only) so it is unit-tested anywhere.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace cml::agent {

inline constexpr std::size_t kModuleRingCapacity = 256;  // slots (a power of two)
inline constexpr std::size_t kRawPathUnits = 520;        // UTF-16 units of a path a slot holds; longer ones are cut (and flagged)

inline constexpr std::uint32_t kRawEventLoaded = 1;    // ipc::ModuleEventKind::Loaded
inline constexpr std::uint32_t kRawEventUnloaded = 2;  // ipc::ModuleEventKind::Unloaded

inline constexpr std::uint32_t kRawPathTruncated = 1u;  // the path was longer than kRawPathUnits: `path` holds its start only
inline constexpr std::uint32_t kRawPatchImage = 2u;     // the image the patch loader was loading or freeing (see patch_image_in_flight)

// One loader notification as the callback recorded it: plain data, no pointers into the loader's memory.
struct RawModuleEvent {
    std::uint32_t kind = 0;   // kRawEventLoaded / kRawEventUnloaded
    std::uint32_t flags = 0;  // kRawPathTruncated, kRawPatchImage
    std::uint64_t seq = 0;    // assigned by the ring
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    std::uint32_t path_units = 0;  // valid units in `path` (no terminator)
    char16_t path[kRawPathUnits];
};

// Fills `out` from what the loader reported. No allocation, no locks: callable under the loader lock.
inline void fill_raw_event(RawModuleEvent& out, std::uint64_t seq, std::uint32_t kind, std::uint64_t base, std::uint64_t size,
                           const char16_t* path, std::size_t units, bool patch_image) noexcept {
    out.kind = kind;
    out.seq = seq;
    out.base = base;
    out.size = size;
    out.flags = patch_image ? kRawPatchImage : 0u;
    if (path == nullptr) {
        units = 0;
    }
    if (units > kRawPathUnits) {
        units = kRawPathUnits;
        out.flags |= kRawPathTruncated;
    }
    if (units != 0) {
        std::memcpy(out.path, path, units * sizeof(char16_t));
    }
    out.path_units = static_cast<std::uint32_t>(units);
}

// The patch image the agent's patch loader is loading or freeing right now: its base and the thread that does it (0, 0: none). A
// loader notification for that base ON that thread carries kRawPatchImage and the poll drops it: patch DLLs are not application
// modules (the module-list snapshot leaves them out for the same reason). The thread matters: while the patch loader has released
// its reservation for the load, another thread may load an application DLL at that very base (the CML4202 collision window), and
// that load is a real module. Constant-initialised, so the callback may read it at any time (the callback passes its own thread id,
// GetCurrentThreadId, which only reads the TEB).
inline std::atomic<std::uint64_t> g_patch_image_in_flight{0};
inline std::atomic<std::uint32_t> g_patch_image_thread{0};

// Whether a notification for `base` on thread `thread` is the patch loader's own image.
inline bool is_patch_image_event(std::uint64_t base, std::uint32_t thread) noexcept {
    // Set: thread, then base (release). Cleared: base, then thread. A reader that sees the base also sees the thread that set it.
    return base != 0 && base == g_patch_image_in_flight.load(std::memory_order_acquire) &&
           thread == g_patch_image_thread.load(std::memory_order_relaxed);
}

class ScopedPatchImage {
public:
    ScopedPatchImage(std::uint64_t base, std::uint32_t thread) noexcept {
        g_patch_image_thread.store(thread, std::memory_order_relaxed);
        g_patch_image_in_flight.store(base, std::memory_order_release);
    }
    ~ScopedPatchImage() {
        g_patch_image_in_flight.store(0, std::memory_order_release);
        g_patch_image_thread.store(0, std::memory_order_relaxed);
    }
    ScopedPatchImage(const ScopedPatchImage&) = delete;
    ScopedPatchImage& operator=(const ScopedPatchImage&) = delete;
};

// Bounded lock-free multi-producer, single-consumer ring of T (T is copied in and out; it must be default-constructible).
template <class T, std::size_t N>
class EventRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "the capacity must be a power of two");

public:
    static constexpr std::size_t capacity = N;

    EventRing() noexcept {
        for (std::size_t i = 0; i < N; ++i) {
            turn_[i].store(i, std::memory_order_relaxed);
        }
    }
    EventRing(const EventRing&) = delete;
    EventRing& operator=(const EventRing&) = delete;

    // Producer (any thread, never blocks, never allocates): claims the next slot and calls fill(slot, seq) to write it, with seq
    // the slot's sequence number (1, 2, ...). False, and the event counts as lost, when the ring is full.
    template <class Fill>
    bool push(Fill&& fill) noexcept {
        std::uint64_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            const std::uint64_t turn = turn_[pos & kMask].load(std::memory_order_acquire);
            const auto diff = static_cast<std::int64_t>(turn - pos);
            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                lost_.fetch_add(1, std::memory_order_relaxed);  // the consumer has not freed this slot yet: full
                return false;
            } else {
                pos = head_.load(std::memory_order_relaxed);  // another producer took it
            }
        }
        fill(slots_[pos & kMask], pos + 1);
        turn_[pos & kMask].store(pos + 1, std::memory_order_release);
        return true;
    }

    // Consumer (one thread at a time): takes the oldest published event. False when there is none (or the producer of the oldest
    // slot is still writing it: it is taken by the next call).
    bool pop(T& out) noexcept {
        const std::uint64_t pos = tail_.load(std::memory_order_relaxed);
        if (turn_[pos & kMask].load(std::memory_order_acquire) != pos + 1) {
            return false;
        }
        out = slots_[pos & kMask];
        turn_[pos & kMask].store(pos + N, std::memory_order_release);  // free for the producer one lap later
        tail_.store(pos + 1, std::memory_order_relaxed);
        return true;
    }

    // Consumer: is an event waiting to be popped?
    bool has_pending() const noexcept {
        const std::uint64_t pos = tail_.load(std::memory_order_relaxed);
        return turn_[pos & kMask].load(std::memory_order_acquire) == pos + 1;
    }
    // Slots claimed and not yet popped (approximate while producers run).
    std::size_t size_approx() const noexcept {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        return head > tail ? static_cast<std::size_t>(head - tail) : 0;
    }
    // Events dropped because the ring was full, since it was created (monotonic).
    std::uint64_t lost() const noexcept { return lost_.load(std::memory_order_relaxed); }

private:
    static constexpr std::uint64_t kMask = N - 1;

    // (No alignas padding against false sharing: the ring is heap-allocated by the agent, and a few hundred events a second do not
    // need it.)
    std::atomic<std::uint64_t> head_{0};  // next position a producer claims
    std::atomic<std::uint64_t> tail_{0};  // next position the consumer takes (written by the consumer only)
    std::atomic<std::uint64_t> lost_{0};
    std::atomic<std::uint64_t> turn_[N];  // position + 1: slot published for the consumer; position + N: free for the next lap
    T slots_[N];
};

using ModuleRing = EventRing<RawModuleEvent, kModuleRingCapacity>;

}  // namespace cml::agent
