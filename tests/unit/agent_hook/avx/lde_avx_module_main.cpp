// cml_lde_avx_module: a module full of AVX2 and AVX-512 code for the length decoder's tools (WP-38, ADR-0042). No test
// runs it: cml_lde_probe plans a hook for each of its function entries and cml_lde_diff compares each of its
// instructions with the reference disassembler. main() only refers to the three translation units; it does not call
// them, because part of the module needs AVX-512.
#include <cstdint>
#include <cstdio>

namespace cml_lde_avx {
namespace optimised {
std::uintptr_t anchor();
}
namespace plain {
std::uintptr_t anchor();
}
namespace avx512 {
std::uintptr_t anchor();
}
}  // namespace cml_lde_avx

int main() {
    using Anchor = std::uintptr_t (*)();
    static Anchor const volatile anchors[] = {&cml_lde_avx::optimised::anchor, &cml_lde_avx::plain::anchor,
                                              &cml_lde_avx::avx512::anchor};
    int present = 0;
    for (const Anchor anchor : anchors) {
        present += anchor != nullptr ? 1 : 0;
    }
    std::printf("cml_lde_avx_module: %d translation units of AVX2 / AVX-512 code; nothing to run\n", present);
    return present == 3 ? 0 : 1;
}
