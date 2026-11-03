#pragma once

// Returns x + 1 (edit: x + 100). The exe hands it to the DLL as a callback.
int exe_callback(int x);
// Returns lib_offset(x) * 2 (edit: * 3). Imports the DLL function through the exe's IAT.
int exe_import_user(int x);
