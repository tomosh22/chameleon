#pragma once

struct Shape {
    virtual ~Shape() = default;
    virtual int area() const = 0;
};

struct Square : Shape {
    int side;
    explicit Square(int s) : side(s) {}
    int area() const override;
};

struct Rect : Shape {
    int w;
    int h;
    Rect(int a, int b) : w(a), h(b) {}
    int area() const override;
};

// Creates the shapes once; the objects (and their vtable pointers) live for the whole run.
void shapes_create();
int shapes_count();
const Shape* shape_at(int i);
