/* F-3 (docs/wp/WP-26.md): clang-cl, a C translation unit whose file-scope statics are initialised (zero) and used through
 * functions that are patched. In raylib 5.5 / clang-cl the reload of rcore.c is accepted ("new_globals": 21) and the
 * process then crashes in glfwWindowShouldClose+0x10 with a null window handle. Hypothesis (not proven): the patched
 * WindowShouldClose() reads a zero-filled copy of `static PlatformData platform` (declared in the included
 * rcore_desktop_glfw.c) instead of the running one. This reduction is the expected shape, not a confirmed reproducer.
 * Expected from a correct reload: `counter` keeps counting from where the running process had it.
 * Build with clang-cl -gcodeview-command-line (default) /Zi /Gy /hotpatch -Xclang -fno-inline, edit `+ 1` to `+ 2`, reload. */
static int counter = 0;
static struct { void* handle; int width; } platform = { 0, 0 };

int next(void) {
    counter = counter + 1; /* edit: + 2 */
    return counter + (platform.handle != 0);
}
