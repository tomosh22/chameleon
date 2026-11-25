#pragma once

// Returns the value printed in every TICK line. Patched by the `pch` scenario (1 -> 2).
int compute_value();
// WP-37: the sum of a static overload set of overloads.cpp (another /Yu translation unit): 240, after the
// `pch_static_overloads` scenario's edit of pch_pick(int) 1240.
int pch_overloads();
