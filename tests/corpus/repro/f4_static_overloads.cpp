// F-4 (docs/wp/WP-26.md): two overloads of a function with internal linkage in one translation unit. The old PDB has two
// records named `scale`, the patch object two decorated symbols. Until WP-37 the planner could not pair them and refused
// every reload of the translation unit with CML3214 (Dear ImGui's imgui_internal.h declares
// `static inline float ImFloor(float)` and `static inline ImVec2 ImFloor(const ImVec2&)`, so every translation unit that
// includes it was refused). Fixed by WP-37 (ADR-0042): the overloads are paired by their signature from the type records
// (the PDB's TPI stream on the old side, the object's .debug$T or its compiler PDB on the new side). The fixture
// `static_overloads` and the corpus edits `static-overloads-*` of Dear ImGui assert it. CML3214 is left for overloads with
// the same signature and for a patch object whose type records cannot be read.
static float scale(float v) {
    return v * 2.0f;  // edit: 3.0f
}

static double scale(double v) {
    return v * 2.0;
}

float use(float v, double w) {
    return scale(v) + static_cast<float>(scale(w));
}
