#pragma once

// Shared synthetic worlds of the bind planner tests.

#include <string>

#include "synth.h"

namespace cml_bind_test {

// Image: .text 0x1000, .rdata 0x2000, .data 0x3000.
//   logic.cpp.obj (compiland 0): GPROC compute_value @0x1010 (0x20 bytes), LPROC helper @0x1040
//   main.cpp.obj  (compiland 1): fixture_note @0x1100, g_tick @0x3080
//   CRT           (compiland 2): __security_cookie @0x3100, __security_check_cookie @0x1200, ...
// `open_now` false: the caller adds module records first and calls open() itself.
inline SynthWorld basic_world(bool open_now = true) {
    SynthWorld w;
    w.proc(w.tu(), "compute_value", 0x10);
    w.proc(w.tu(), "helper", 0x40, 0x10, true);
    w.proc(w.main_obj(), "fixture_note", 0x100);
    w.data(w.main_obj(), "g_tick", 0x80, false);
    w.pub_func("?compute_value@@YAHXZ", 0x10);
    w.pub_func("fixture_note", 0x100);
    w.pub_data("?g_tick@@3HA", 0x80);
    w.pub_data("?total@Accum@@2HA", 0x90);
    w.pub_data("__security_cookie", 0x100);
    w.pub_func("__security_check_cookie", 0x200);
    w.pub_func("__CxxFrameHandler4", 0x210);
    w.pub_data("__imp_Sleep", 0x10, SynthWorld::kRdataSec);
    if (open_now) {
        w.open();
    }
    return w;
}

template <class T>
inline std::string err(const cml::Result<T>& r) {
    return r.has_value() ? std::string() : r.error().format();
}

}  // namespace cml_bind_test
