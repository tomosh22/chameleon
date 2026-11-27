# Fixture `atomic_reload` (WP-47, R-52): a header edit over three translation units, the third of which the planner refuses.
#
#   scale.h     `constexpr int kScale = 1;` and `#define CML_EXTRA_TLS 0`; holds no code (clang-cl's PDB does not list it, the
#               broker finds the three translation units through its include scan, ADR-0013).
#   shared.h    an inline (noinline) function returning kScale, included by the three: each has its own COMDAT copy.
#   tu_a.cpp    a_value() = shared_scale() * 10 + kScale      (11)
#   tu_b.cpp    b_value() = shared_scale() * 100 + kScale     (101)
#   tu_c.cpp    c_value() = shared_scale() * 1000 + kScale    (1001); with CML_EXTRA_TLS 1 it also defines and adds a NEW
#               thread_local, which the planner refuses (CML3400)
#   main.cpp    the loop, never patched; it owns a thread_local, so the old image has TLS and the refusal is that of the new one
#
# Output: `TICK n=<k> a=<a> b=<b> c=<c> frames=<main thread's thread_local>`.
#
# edits/scale_2_tls.edit       kScale 1 -> 2 and CML_EXTRA_TLS 0 -> 1: the three compilands recompile; tu_c.cpp (the third) is
#                              refused, so nothing is applied (a=11 b=101 c=1001 stay) and no patch_applied is published.
# edits/stacked/no_tls.edit    on top of it: CML_EXTRA_TLS back to 0, kScale stays 2: the same edit now reloads all three
#                              (a=22 b=202 c=2002). Outside edits/: it applies only on top of the first.
cml_add_fixture(atomic_reload SOURCES main.cpp tu_a.cpp tu_b.cpp tu_c.cpp)

add_test(NAME fixture.atomic_reload.runs COMMAND fixture_atomic_reload --ticks 3)
set_tests_properties(fixture.atomic_reload.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 a=11 b=101 c=1001 frames=3"
    TIMEOUT 30)
