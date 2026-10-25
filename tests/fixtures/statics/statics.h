#pragma once

// Returns 3 (edit: 30). Every call bumps the file-static call counter and the static member total.
int statics_value();
int statics_calls();
int statics_total();
