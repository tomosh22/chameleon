#pragma once

// Always 0, defined in main.cpp: the arguments of the overloads are computed from it at run time, so an optimising
// compiler cannot fold the calls away and every overload stays a function of its translation unit.
extern int g_so_zero;

// Implemented in tu_a.cpp and tu_b.cpp; both include overloads.h.
int a_float();   // so_floor(7.9f)                      7, after the edit 107
int a_vec();     // so_floor({3.7f, 5.2f}) as x*10 + y  35 (never changes: the sibling overload is not edited)
int a_weight();  // so_weight(int) + so_weight(long long) + so_weight(const char*)
int a_calls();   // how often a_float() ran: a file-static of tu_a.cpp that must survive the reload
int b_float();   // so_floor(21.5f)                     21, after the edit 121
int b_vec();     // so_floor({8.1f, 2.9f}) as x*10 + y  82
int b_calls();
