#pragma once

#if defined(GREETER_BUILD)
#define GREETER_API __declspec(dllexport)
#else
#define GREETER_API __declspec(dllimport)
#endif

// Builds the line the application prints; edit its body while the program runs.
GREETER_API int greeter_message(char* buffer, int capacity, int tick);
