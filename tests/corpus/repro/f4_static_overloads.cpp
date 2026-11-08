// F-4 (docs/wp/WP-26.md): two overloads of a function with internal linkage in one translation unit. The old PDB has two
// records named `scale` and no types (cml_pdb reads none yet), the patch object two decorated symbols, so the planner cannot
// pair them and refuses every reload of the translation unit with CML3214. Dear ImGui's imgui_internal.h declares
// `static inline float ImFloor(float)` and `static inline ImVec2 ImFloor(const ImVec2&)`: every translation unit that
// includes it (all of imgui, imgui_impl_win32.cpp under cl) is refused.
static float scale(float v) {
    return v * 2.0f;  // edit: 3.0f
}

static double scale(double v) {
    return v * 2.0;
}

float use(float v, double w) {
    return scale(v) + static_cast<float>(scale(w));
}
