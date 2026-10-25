#pragma once

// Stores a lambda capturing `base` by value in a std::function (once, at startup).
void lambdas_init(int base);
// Calls the stored std::function with n % 3.
int lambdas_value(int n);
