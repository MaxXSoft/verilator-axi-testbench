if(NOT DEFINED TEST_SOURCE OR NOT IS_DIRECTORY "${TEST_SOURCE}")
  message(FATAL_ERROR "TEST_SOURCE must name a CMake source directory")
endif()
if(NOT DEFINED TEST_BINARY)
  message(FATAL_ERROR "TEST_BINARY is required")
endif()
if(NOT DEFINED EXPECTED_PATTERN)
  message(FATAL_ERROR "EXPECTED_PATTERN is required")
endif()
if(NOT DEFINED AXI_TB_SOURCE_ROOT)
  message(FATAL_ERROR "AXI_TB_SOURCE_ROOT is required")
endif()

file(REMOVE_RECURSE "${TEST_BINARY}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${TEST_SOURCE}" -B "${TEST_BINARY}"
    "-DAXI_TB_SOURCE_ROOT=${AXI_TB_SOURCE_ROOT}"
  RESULT_VARIABLE _result
  OUTPUT_VARIABLE _stdout
  ERROR_VARIABLE _stderr
)
string(CONCAT _output "${_stdout}" "${_stderr}")
if(_result EQUAL 0)
  message(FATAL_ERROR "Nested configure unexpectedly succeeded:\n${_output}")
endif()
if(NOT _output MATCHES "${EXPECTED_PATTERN}")
  message(FATAL_ERROR
    "Nested configure did not report '${EXPECTED_PATTERN}':\n${_output}")
endif()
