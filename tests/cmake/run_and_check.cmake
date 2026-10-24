# Runs CML_EXE with CML_ARGS and checks the exit code (CML_EXPECT_RC, default 0) and, when given,
# that stdout with trailing whitespace stripped equals CML_EXPECT_OUT exactly.
#
#   cmake -DCML_EXE=... -DCML_ARGS=--version -DCML_EXPECT_RC=0 -DCML_EXPECT_OUT="cml_broker 0.1.0" -P run_and_check.cmake

if(NOT DEFINED CML_EXE)
    message(FATAL_ERROR "CML_EXE not set")
endif()
if(NOT DEFINED CML_EXPECT_RC)
    set(CML_EXPECT_RC 0)
endif()

separate_arguments(_args NATIVE_COMMAND "${CML_ARGS}")
execute_process(
    COMMAND "${CML_EXE}" ${_args}
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
string(STRIP "${_out}" _out)

message(STATUS "command: ${CML_EXE} ${CML_ARGS}")
message(STATUS "exit code: ${_rc}")
message(STATUS "stdout: [${_out}]")
message(STATUS "stderr: [${_err}]")

if(NOT "${_rc}" STREQUAL "${CML_EXPECT_RC}")
    message(FATAL_ERROR "expected exit code ${CML_EXPECT_RC}, got ${_rc}")
endif()
if(DEFINED CML_EXPECT_OUT AND NOT "${_out}" STREQUAL "${CML_EXPECT_OUT}")
    message(FATAL_ERROR "expected stdout [${CML_EXPECT_OUT}], got [${_out}]")
endif()
