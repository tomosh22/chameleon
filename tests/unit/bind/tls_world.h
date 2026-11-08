#pragma once

// WP-24: the synthetic old module of the thread_local tests (basic_world plus a .tls image section).
#include "synth.h"
#include "worlds.h"

namespace cml_bind_test {

inline constexpr std::uint16_t kTlsSec = 4;  // .tls is the 4th image section, rva 0x4000: not the first

// basic_world plus a .tls image section with
//   g_tls_counter       S_GTHREAD32 at offset 0x8 (its S_PUB32 says 0x60: a thread-local's public is NOT its offset)
//   _Init_thread_epoch  S_GTHREAD32 at 0x4 (public 0x70)
//   ?only_pub@@3HA      a public only (no S_GTHREAD32)
// and, in the TU's compiland, f::calls (S_LTHREAD32 at 0x10) and f::plain (an ordinary S_LDATA32 static).
inline SynthWorld tls_world() {
    SynthWorld w = basic_world(false);
    w.spec.sections.push_back({".tls", 0x100, 0x4000, 0x100, 0xC0000040});
    w.pub_data("?g_tls_counter@@3HA", 0x60, kTlsSec);
    w.spec.symrec.data(0x1113, "g_tls_counter", kTlsSec, 0x8);
    w.pub_data("?only_pub@@3HA", 0x50, kTlsSec);
    w.pub_data("_Init_thread_epoch", 0x70, kTlsSec);
    w.spec.symrec.data(0x1113, "_Init_thread_epoch", kTlsSec, 0x4);
    w.pub_data("_tls_index", 0x120);
    w.tu().symbols.proc(0x1110, "f", SynthWorld::kTextSec, 0x50, 0x30);
    w.tu().symbols.data(0x1112, "calls", kTlsSec, 0x10);
    w.tu().symbols.data(0x110C, "plain", SynthWorld::kDataSec, 0x40);
    w.tu().symbols.end();
    w.open();
    return w;
}

}  // namespace cml_bind_test
