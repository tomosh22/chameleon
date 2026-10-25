# tests/fixtures

Placeholder. Later work packages add small console apps here, one per feature (see section 10 of docs/IMPLEMENTATION_PLAN.md). Each is a `TICK k=v ...` loop built with `add_executable` plus `cml_target_fixture_style()` (cmake/CmlTargets.cmake), which disables inlining in RelWithDebInfo (`/Ob0`). The top-level `tests/CMakeLists.txt` picks this directory up automatically once it contains a `CMakeLists.txt`.
