/*
 * Fixture `imgrel` (WP-36, ADR-0042): image-relative references to data of the running image.
 *
 * imgrel.c is the translation unit the scenario edits. Under cl with optimisation (the RelWithDebInfo cells) its two
 * functions keep `__ImageBase` in a register and address the globals below as `[reg + index*scale + sym@IMGREL]`:
 * IMAGE_REL_AMD64_ADDR32NB relocations on the displacement of a memory operand (corpus finding F-2, raylib's
 * PlayAutomationEvent). main.c reads and writes the same globals directly, from code that is never patched.
 */
#ifndef CML_FIXTURE_IMGREL_H
#define CML_FIXTURE_IMGREL_H

struct Core {
    char keys[512];
    char prev[512];
    int q[16];
    int count;
    float f[32];
};

extern struct Core CORE;         /* defined in imgrel.c */
extern int (*fptab[8])(int);     /* defined in imgrel.c */

int ig_g(int p);                 /* defined in main.c: p + 4242 */

/* A switch with a jump table whose cases read and write CORE, a file-static array and a table of function pointers. */
int ig_mixed(int type, int p);
/* Indexes two global arrays (no jump table): the second shape that makes cl address image-relatively. */
long long ig_wide(long long i);

#endif
