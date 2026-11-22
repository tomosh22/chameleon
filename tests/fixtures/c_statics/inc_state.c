/* Included by c_statics.c (not compiled on its own): a file-scope static of an #included .c file. */
typedef struct Holder {
    int* p;
} Holder;

static int inc_cell;
static Holder inc_state;

static void inc_init(void)
{
    inc_state.p = &inc_cell;
}
