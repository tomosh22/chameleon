/* Fixture `c_statics`: the translation unit the scenario edits. See c_statics.h. */
#include "c_statics.h"

#include "inc_state.c"

static int main_cell;
static Holder main_state;

void cs_init(void)
{
    inc_init();
    main_state.p = &main_cell;
}

int cs_read(int which)
{
    static int local_cell;
    static Holder local_state;
    Holder* h;
    if (local_state.p == 0) {
        local_state.p = &local_cell;
    }
    h = which == 0 ? &inc_state : (which == 1 ? &main_state : &local_state);
    return ++*h->p;
}
