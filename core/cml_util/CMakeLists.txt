cml_add_core_library(cml_util
    SOURCES
        src/diag.cpp
        src/fail_fast.cpp
        src/hash.cpp
        src/log.cpp
    LIBS cml_third_party_xxhash)
