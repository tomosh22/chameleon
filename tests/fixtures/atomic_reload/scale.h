#pragma once

// A header that holds no code: one constant and one switch, no function, no data definition. clang-cl's PDB does not list such
// a file for the translation units that include it (ADR-0013), so only the broker's include scan maps a change of it to them.
// Patched by the scenario `atomic_reload` (kScale 1 -> 2; CML_EXTRA_TLS 0 -> 1 -> 0).
constexpr int kScale = 1;

// 1: tu_c.cpp defines a new thread_local (refused by the planner, CML3400).
#define CML_EXTRA_TLS 0

// end of scale.h
