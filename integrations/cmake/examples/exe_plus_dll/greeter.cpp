#include "greeter.h"

#include <cstdio>

int greeter_message(char* buffer, int capacity, int tick) {
    return std::snprintf(buffer, static_cast<size_t>(capacity), "hello from the DLL, tick %d", tick);
}
