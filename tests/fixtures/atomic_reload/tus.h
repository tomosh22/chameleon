#pragma once

// Implemented in tu_a.cpp, tu_b.cpp and tu_c.cpp; all three include shared.h (and so scale.h).
int a_value();  // shared_scale() * 10 + kScale
int b_value();  // shared_scale() * 100 + kScale
int c_value();  // shared_scale() * 1000 + kScale (+ t_extra with CML_EXTRA_TLS)
