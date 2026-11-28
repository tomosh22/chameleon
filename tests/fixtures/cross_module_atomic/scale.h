#pragma once

// A header that holds no code: one constant and one switch. Both modules of the fixture include it (exe_part.cpp in the exe,
// plugin.cpp in the DLL), so an edit of it recompiles a translation unit in each. clang-cl's PDB does not list such a file for
// the translation units that include it (ADR-0013); the broker's include scan maps a change of it to them, in each module.
// Patched by the scenario `cross_module_atomic` (kScale 1 -> 2; CML_PLUGIN_TLS 0 -> 1 -> 0).
constexpr int kScale = 1;

// 1: plugin.cpp (in the DLL) defines a new thread_local (refused by the planner, CML3400).
#define CML_PLUGIN_TLS 0

// end of scale.h
