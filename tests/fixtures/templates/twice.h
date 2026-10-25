#pragma once

// Instantiated as twice<int> in both a.cpp and b.cpp: one COMDAT per object.
template <typename T> __declspec(noinline) T twice(T v)
{
    return v + v;
}

int templates_a();
int templates_b();
