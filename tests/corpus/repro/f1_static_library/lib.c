/* F-1 (docs/wp/WP-26.md): this file is built into libgame.lib and the executable links the library.
 * Changing the return value and reloading patches nothing: the PDB lists lib.c.obj as a member of libgame.lib, which is
 * not a source compiland (ModuleIndex::is_source_compiland). Compile main.c and lib.c into one executable instead and
 * the same edit reloads. Build: cl /Zi /Gy /hotpatch /Ob0 /c lib.c & lib /OUT:libgame.lib lib.obj &
 *        cl /Zi /Gy /hotpatch main.c libgame.lib /link /DEBUG:FULL /OPT:NOREF /OPT:NOICF /FUNCTIONPADMIN */
int game_value(void) {
    return 1; /* edit: return 2; */
}
