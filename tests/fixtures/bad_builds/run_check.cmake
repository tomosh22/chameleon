# Runs `cml_broker check` on one module and asserts the exit code and a diagnostic line (WP-18).
#
#   cmake -DBROKER=<cml_broker> -DMODULE=<exe> -DEXPECT_EXIT=<0|1> [-DEXPECT_LINE=<text>]... [-DNOT_LINE=<text>]
#         [-DCOPY_ALONE_TO=<dir>] -P run_check.cmake
#
# EXPECT_LINE is a text that must appear in the output (e.g. "FAIL CML1103"), several separated by ';'. NOT_LINE must not
# appear. COPY_ALONE_TO copies the module alone into that (emptied) directory first and runs from there, so a PDB that
# sits next to the original is not found (the link used /PDBALTPATH to record a bare file name).
if(NOT BROKER OR NOT MODULE OR "${EXPECT_EXIT}" STREQUAL "")
    message(FATAL_ERROR "run_check.cmake: BROKER, MODULE and EXPECT_EXIT are required")
endif()
set(_work "${CMAKE_CURRENT_LIST_DIR}")
if(COPY_ALONE_TO)
    file(REMOVE_RECURSE "${COPY_ALONE_TO}")
    file(MAKE_DIRECTORY "${COPY_ALONE_TO}")
    file(COPY "${MODULE}" DESTINATION "${COPY_ALONE_TO}")
    get_filename_component(_name "${MODULE}" NAME)
    set(MODULE "${COPY_ALONE_TO}/${_name}")
    set(_work "${COPY_ALONE_TO}")
endif()
execute_process(
    COMMAND "${BROKER}" check --verbose "${MODULE}"
    WORKING_DIRECTORY "${_work}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
message(STATUS "exit code ${_rc}")
message(STATUS "${_out}")
if(_err)
    message(STATUS "stderr: ${_err}")
endif()
if(NOT "${_rc}" STREQUAL "${EXPECT_EXIT}")
    message(FATAL_ERROR "cml_broker check exited with ${_rc}, expected ${EXPECT_EXIT}")
endif()
foreach(_line IN LISTS EXPECT_LINE)
    string(FIND "${_out}" "${_line}" _pos)
    if(_pos EQUAL -1)
        message(FATAL_ERROR "expected '${_line}' in the output of cml_broker check")
    endif()
endforeach()
if(NOT_LINE)
    string(FIND "${_out}" "${NOT_LINE}" _pos)
    if(NOT _pos EQUAL -1)
        message(FATAL_ERROR "did not expect '${NOT_LINE}' in the output of cml_broker check")
    endif()
endif()
