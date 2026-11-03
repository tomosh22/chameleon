#pragma once

// small_value() is tiny and defined in logic.cpp, where compute_value() (same TU) inlines it at /O2.
int small_value();
int compute_value();
