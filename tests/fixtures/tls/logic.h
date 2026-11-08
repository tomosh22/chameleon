#pragma once

struct TlsValues {
    int step;
    int ns;  // the calling thread's namespace-scope thread_local
    int fn;  // the calling thread's function-local static thread_local
};

// Adds the step to both thread_locals of the calling thread and returns the new values.
TlsValues tls_step();
