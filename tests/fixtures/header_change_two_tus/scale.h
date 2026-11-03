#pragma once

// A header that holds no code: one constant, no function, no data definition. clang-cl's PDB does not list such a
// file for the translation units that include it (ADR-0013), so only the broker's include scan maps a change of it
// to those translation units. Patched by the `header_change_two_tus` scenario (1 -> 2).
constexpr int kScale = 1;

// end of scale.h
