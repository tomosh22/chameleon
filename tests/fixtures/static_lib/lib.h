#pragma once

// Both live in the static library fixture_static_lib_lib.
int static_lib_value();  // 7 (edit: 70)
int static_lib_calls();  // how often static_lib_value() ran
