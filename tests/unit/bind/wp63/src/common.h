struct Obj { int v; int hits; };
Obj make(int id);
int make_int(int v);
void sink(int);
void keep(int*);
// A static with a destructor (n3 / s3: its initialisation registers it with atexit, and the EH funclets clear or abort
// the guard when the constructor throws).
struct D { int v; explicit D(int x); ~D(); };
