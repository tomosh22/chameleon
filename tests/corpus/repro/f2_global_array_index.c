/* F-2 (docs/wp/WP-26.md): with cl /O2 /Ob0 (the raylib cell) an element of a global array indexed by a variable is
 * addressed as  lea rcx, __ImageBase / mov eax, [rcx + rax*4 + table@IMGREL]  : an IMAGE_REL_AMD64_ADDR32NB reference to the
 * old image's data. Any edit of this translation unit is then refused with CML3101 ("references 'table' in <module>
 * image-relatively (ADDR32NB); this cannot be retargeted"), whichever function was edited. raylib 5.5's rcore.c
 * (PlayAutomationEvent references CORE) hits it; this reduction is the expected shape (hypothesis: the corpus log names only
 * the symbol, not the instruction). The clang-cl cell did not refuse the same reload (see F-3). */
int table[1024];

int lookup(int i) {
    return table[i & 1023]; /* edit anything in this file */
}
