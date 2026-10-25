#pragma once

// Shared helpers of the hook installer tests (Windows): executable memory with hand-assembled
// functions placed with exact padding, and the machine code of the function shapes the tests use.
#if defined(_WIN32)

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

#if defined(_MSC_VER)
#define HOOKTEST_NOINLINE __declspec(noinline)
#else
#define HOOKTEST_NOINLINE __attribute__((noinline))
#endif

namespace hooktest {

using Bytes = std::vector<std::uint8_t>;
using IntFn = int (*)(int);
using VoidFn = void (*)(volatile char*);

inline Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& p : parts) {
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

inline Bytes imm32(std::int32_t v) {
    return {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v >> 16),
            static_cast<std::uint8_t>(v >> 24)};
}

// ---- machine code of the test functions (x64, Windows calling convention) -------------------------
//   int f(int x) shapes all return x + k.
inline Bytes lea_ret(std::int32_t k) { return cat({{0x8D, 0x81}, imm32(k), {0xC3}}); }  // lea eax,[rcx+k]; ret
// cl /O2 shape: sub rsp,28h ... add rsp,28h; ret          (ADR-0020: 48 83 ec 28)
inline Bytes cl_release(std::int32_t k) {
    return cat({{0x48, 0x83, 0xEC, 0x28}, {0x8D, 0x81}, imm32(k), {0x48, 0x83, 0xC4, 0x28, 0xC3}});
}
// cl /Od shape: push rdi (REX) ; sub rsp,20h             (ADR-0020: 40 57 48 83 ec 20)
inline Bytes cl_debug(std::int32_t k) {
    return cat({{0x40, 0x57, 0x48, 0x83, 0xEC, 0x20}, {0x8D, 0x81}, imm32(k), {0x48, 0x83, 0xC4, 0x20, 0x5F, 0xC3}});
}
// First instruction is one byte (push rbx), then sub rsp,20h.
inline Bytes push_first(std::int32_t k) {
    return cat({{0x53, 0x48, 0x83, 0xEC, 0x20}, {0x8D, 0x81}, imm32(k), {0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3}});
}
// 4 bytes: lea eax,[rcx+k8]; ret
inline Bytes short_lea(std::int8_t k) { return {0x8D, 0x41, static_cast<std::uint8_t>(k), 0xC3}; }
inline Bytes xor_ret() { return {0x31, 0xC0, 0xC3}; }  // 3 bytes, returns 0
inline Bytes ret_only() { return {0xC3}; }              // 1 byte
// int f(): mov eax, v; ret
inline Bytes const_ret(std::int32_t v) { return cat({{0xB8}, imm32(v), {0xC3}}); }
// void f(volatile char* flag): spins while *flag == 0. Every instruction lies in the first 5 bytes.
inline Bytes spin_until() { return {0x80, 0x39, 0x00, 0x74, 0xFB, 0xC3}; }
// int f(int n): loops n times inside its first 4 bytes, then returns v. Slow enough for a thread
// to be caught inside its first bytes.
inline Bytes slow_const(std::int32_t v) { return cat({{0xFF, 0xC9, 0x75, 0xFC, 0xB8}, imm32(v), {0xC3}}); }
// A call as the 1st instruction followed by more code: call rax ; sub rsp,28h ; ...
inline Bytes call_first() { return {0xFF, 0xD0, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x83, 0xC4, 0x28, 0xC3}; }
// vmovaps xmm0, xmm1 ; ret  (VEX: not covered by the length decoder)
inline Bytes vex_first() { return {0xC5, 0xF8, 0x28, 0xC1, 0xC3, 0x90, 0x90, 0x90}; }

// ---- executable memory -----------------------------------------------------------------------------

struct Placed {
    std::uint8_t* entry = nullptr;
    std::size_t size = 0;
    IntFn as_int() const { return reinterpret_cast<IntFn>(entry); }
    VoidFn as_void() const { return reinterpret_cast<VoidFn>(entry); }
    const void* address() const { return entry; }
};

// A block of pages that is read-write while functions are placed and PAGE_EXECUTE_READ after seal(),
// like the code of a real module. Everything not placed is 0x00 (which is not padding).
class Arena {
public:
    explicit Arena(std::size_t pages = 8) : size_(pages * 0x1000) {
        base_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size_, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    }
    ~Arena() {
        if (base_ != nullptr) {
            VirtualFree(base_, 0, MEM_RELEASE);
        }
    }
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    bool ok() const { return base_ != nullptr; }
    std::uint8_t* base() const { return base_; }
    std::size_t size() const { return size_; }

    // Places `before` bytes, then `code`, then `after` bytes. The first byte of `code` lands on an
    // address that is `misalign` bytes past a multiple of 16.
    Placed place(const Bytes& before, const Bytes& code, const Bytes& after, std::size_t misalign = 0) {
        std::size_t entry_offset = (cursor_ + before.size() + 15) & ~static_cast<std::size_t>(15);
        entry_offset += misalign;
        return place_at(entry_offset, before, code, after);
    }
    Placed place_at(std::size_t entry_offset, const Bytes& before, const Bytes& code, const Bytes& after) {
        Placed p;
        std::memcpy(base_ + entry_offset - before.size(), before.data(), before.size());
        std::memcpy(base_ + entry_offset, code.data(), code.size());
        std::memcpy(base_ + entry_offset + code.size(), after.data(), after.size());
        p.entry = base_ + entry_offset;
        p.size = code.size();
        const std::size_t end = entry_offset + code.size() + after.size() + 32;
        if (end > cursor_) {
            cursor_ = end;
        }
        return p;
    }
    // Entry preceded by `pad` bytes of `pad_byte` and followed by `tail` bytes of `tail_byte`.
    Placed place_padded(const Bytes& code, std::size_t pad, std::size_t tail, std::uint8_t pad_byte = 0xCC,
                        std::uint8_t tail_byte = 0xCC, std::size_t misalign = 0) {
        return place(Bytes(pad, pad_byte), code, Bytes(tail, tail_byte), misalign);
    }

    void seal(DWORD protection = PAGE_EXECUTE_READ) {
        DWORD old = 0;
        VirtualProtect(base_, size_, protection, &old);
        FlushInstructionCache(GetCurrentProcess(), base_, size_);
    }
    void unseal() {
        DWORD old = 0;
        VirtualProtect(base_, size_, PAGE_READWRITE, &old);
    }

private:
    std::uint8_t* base_ = nullptr;
    std::size_t size_ = 0;
    std::size_t cursor_ = 64;
};

inline Bytes snapshot(const void* address, std::size_t length) {
    const auto* p = static_cast<const std::uint8_t*>(address);
    return Bytes(p, p + length);
}

inline DWORD protection_of(const void* address) {
    MEMORY_BASIC_INFORMATION info;
    VirtualQuery(address, &info, sizeof(info));
    return info.Protect;
}

}  // namespace hooktest

#endif  // _WIN32
