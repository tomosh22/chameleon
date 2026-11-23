# WP-32: every diagnostic id defined in code has an entry in docs/TROUBLESHOOTING.md.
#
#   cmake -DCML_SOURCE_DIR=<repo root> -P tools/package/check_troubleshooting.cmake
#
# Registered as ctest `docs.troubleshooting` (so it runs in every cell of the `ci` workflow) and run by package.yml.
#
# An id is "defined in code" when it is
#   * an `inline constexpr std::uint32_t kName = NNNN;` (NNNN in 1000..5999) in a header or source named *ids.h,
#     module_audit.h, fault_stages.h or pe_codeview.cpp (names ending in Ms or Budget, or starting with kMax / kMin,
#     are limits, not ids), or
#   * the first argument of make_diag( / rewrap( in core/cml_pe/src or core/cml_coff/src (the readers use bare numbers).
# An entry is a heading `### CMLNNNN` in the document. Ids in the document that no code defines are only reported
# (the integrations and the CMake helper own ids that are not defined in C++).
#
# The check fails when an id lacks an entry, when the scan finds suspiciously few ids (the patterns rotted), or when an
# entry is listed twice.

cmake_minimum_required(VERSION 3.24)
if(NOT CML_SOURCE_DIR)
    get_filename_component(CML_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()
get_filename_component(CML_SOURCE_DIR "${CML_SOURCE_DIR}" ABSOLUTE)
set(_doc "${CML_SOURCE_DIR}/docs/TROUBLESHOOTING.md")
if(NOT EXISTS "${_doc}")
    message(FATAL_ERROR "docs.troubleshooting: ${_doc} does not exist")
endif()

set(_code_ids "")
set(_where_ids "")

file(GLOB_RECURSE _headers RELATIVE "${CML_SOURCE_DIR}"
    "${CML_SOURCE_DIR}/agent/*.h" "${CML_SOURCE_DIR}/agent/*.cpp"
    "${CML_SOURCE_DIR}/broker/*.h" "${CML_SOURCE_DIR}/core/*.h" "${CML_SOURCE_DIR}/core/*.cpp")
foreach(_f IN LISTS _headers)
    get_filename_component(_base "${_f}" NAME)
    if(_base MATCHES "ids\\.h$" OR _base STREQUAL "module_audit.h" OR _base STREQUAL "fault_stages.h" OR _base STREQUAL "pe_codeview.cpp")
        file(STRINGS "${CML_SOURCE_DIR}/${_f}" _lines REGEX "constexpr +std::uint32_t +k[A-Za-z0-9_]+ *= *[1-5][0-9][0-9][0-9] *;")
        foreach(_line IN LISTS _lines)
            string(REGEX MATCH "(k[A-Za-z0-9_]+) *= *([1-5][0-9][0-9][0-9])" _m "${_line}")
            set(_name "${CMAKE_MATCH_1}")
            set(_num "${CMAKE_MATCH_2}")
            if(_name MATCHES "(Ms|Budget)$" OR _name MATCHES "^kM(ax|in)")
                continue()
            endif()
            list(APPEND _code_ids "${_num}")
            list(APPEND _where_ids "${_num}=${_f}")
        endforeach()
    endif()
    if(_f MATCHES "^core/cml_(pe|coff)/src/")
        file(STRINGS "${CML_SOURCE_DIR}/${_f}" _lines REGEX "(make_diag|rewrap)\\( *[1-5][0-9][0-9][0-9]")
        foreach(_line IN LISTS _lines)
            string(REGEX MATCHALL "(make_diag|rewrap)\\( *[1-5][0-9][0-9][0-9]" _calls "${_line}")
            foreach(_call IN LISTS _calls)
                string(REGEX MATCH "[1-5][0-9][0-9][0-9]" _num "${_call}")
                list(APPEND _code_ids "${_num}")
                list(APPEND _where_ids "${_num}=${_f}")
            endforeach()
        endforeach()
    endif()
endforeach()
list(REMOVE_DUPLICATES _code_ids)
list(SORT _code_ids)
list(LENGTH _code_ids _n_code)

file(STRINGS "${_doc}" _heads REGEX "^### CML[1-5][0-9][0-9][0-9]")
set(_doc_ids "")
set(_dups "")
foreach(_h IN LISTS _heads)
    string(REGEX MATCH "[1-5][0-9][0-9][0-9]" _num "${_h}")
    if(_num IN_LIST _doc_ids)
        list(APPEND _dups "CML${_num}")
    endif()
    list(APPEND _doc_ids "${_num}")
endforeach()
list(REMOVE_DUPLICATES _doc_ids)
list(LENGTH _doc_ids _n_doc)

set(_missing "")
foreach(_num IN LISTS _code_ids)
    if(NOT _num IN_LIST _doc_ids)
        set(_origin "")
        foreach(_w IN LISTS _where_ids)
            if(_w MATCHES "^${_num}=(.*)$")
                set(_origin "${CMAKE_MATCH_1}")
                break()
            endif()
        endforeach()
        list(APPEND _missing "CML${_num} (defined in ${_origin})")
    endif()
endforeach()
set(_stale "")
foreach(_num IN LISTS _doc_ids)
    if(NOT _num IN_LIST _code_ids)
        list(APPEND _stale "CML${_num}")
    endif()
endforeach()

message(STATUS "docs.troubleshooting: ${_n_code} diagnostic ids defined in code, ${_n_doc} entries in docs/TROUBLESHOOTING.md")
if(_stale)
    list(JOIN _stale " " _s)
    message(STATUS "docs.troubleshooting: entries without a definition in C++ (owned by an integration or the CMake helper, or stale): ${_s}")
endif()

set(_failed FALSE)
if(_n_code LESS 300)
    message(SEND_ERROR "docs.troubleshooting: only ${_n_code} ids found in code (expected 300 or more); the scan patterns in ${CMAKE_CURRENT_LIST_FILE} no longer match the source")
    set(_failed TRUE)
endif()
if(_dups)
    list(JOIN _dups " " _s)
    message(SEND_ERROR "docs.troubleshooting: duplicate entries: ${_s}")
    set(_failed TRUE)
endif()
if(_missing)
    list(JOIN _missing "\n  " _s)
    message(SEND_ERROR "docs.troubleshooting: no entry in docs/TROUBLESHOOTING.md for:\n  ${_s}\nAdd a `### CMLnnnn` section (cause, message, what to do) for each.")
    set(_failed TRUE)
endif()
if(NOT _failed)
    message(STATUS "docs.troubleshooting: OK")
endif()
