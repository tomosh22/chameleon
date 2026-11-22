#pragma once

/* Fixture `c_statics`: three `static` structs, each holding a pointer to its storage. cs_init() fills the two file-scope
 * ones at run time (zero-initialised until then, like raylib's `static PlatformData platform`); the function-local one is
 * filled by its own function on first use. cs_read(0|1|2) reads the one in the #included .c file, the main TU's, and the
 * function-local one through the pointer and counts: the k-th call returns k (the edit adds 1000). */
#ifdef __cplusplus
extern "C" {
#endif
void cs_init(void);
int cs_read(int which);
#ifdef __cplusplus
}
#endif
