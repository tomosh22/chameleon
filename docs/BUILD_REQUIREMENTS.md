# Build requirements

Stub, extended by later work packages.

| Requirement | Version | Notes |
|---|---|---|
| Windows | 10/11 x64, Server 2022/2025 | Only x64 is supported; configuration fails elsewhere unless `CML_ALLOW_NON_WINDOWS=ON`. |
| CMake | 3.21 or newer | |
| Visual Studio / Build Tools | 2022 (MSVC v143), Windows 10/11 SDK | Needed for `cl`, `link`, the SDK and the VS generator. |
| LLVM clang-cl | 15 or newer (CI uses what is preinstalled on the GitHub images) | Optional second compiler. Uses MSVC's `link.exe` and CRT. |
| Ninja | any recent | Optional; CI uses it. |

CI matrix: windows-2022 and windows-2025, each with MSVC `cl` and `clang-cl`, in Debug and
RelWithDebInfo (`.github/workflows/ci.yml`).
