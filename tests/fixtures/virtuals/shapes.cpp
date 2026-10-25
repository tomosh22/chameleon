#include "shapes.h"

namespace {
const Shape* g_shapes[2] = {nullptr, nullptr};
}

int Square::area() const
{
    return side * side;
}

int Rect::area() const
{
    return w * h;
}

void shapes_create()
{
    g_shapes[0] = new Square(3);
    g_shapes[1] = new Rect(2, 5);
}

int shapes_count()
{
    return 2;
}

const Shape* shape_at(int i)
{
    return g_shapes[i];
}
