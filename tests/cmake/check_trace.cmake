foreach(_required PROGRAM OUTPUT FORMAT)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "${_required} is required")
  endif()
endforeach()

file(REMOVE "${OUTPUT}")
execute_process(
  COMMAND "${PROGRAM}"
    --max-cycles 1000
    --reset-cycles 5
    --trace "${OUTPUT}"
  RESULT_VARIABLE _result
  OUTPUT_VARIABLE _stdout
  ERROR_VARIABLE _stderr
)
if(NOT _result EQUAL 0)
  message(FATAL_ERROR
    "trace simulator failed with ${_result}\nstdout:\n${_stdout}\nstderr:\n${_stderr}")
endif()
if(NOT EXISTS "${OUTPUT}")
  message(FATAL_ERROR "trace output was not created: ${OUTPUT}")
endif()
file(SIZE "${OUTPUT}" _size)
if(_size EQUAL 0)
  message(FATAL_ERROR "trace output is empty: ${OUTPUT}")
endif()

file(READ "${OUTPUT}" _header LIMIT 9 HEX)
string(TOLOWER "${_header}" _header)
if(FORMAT STREQUAL "FST")
  if(NOT _header STREQUAL "000000000000000149")
    message(FATAL_ERROR "invalid FST header: ${_header}")
  endif()
elseif(FORMAT STREQUAL "VCD")
  string(SUBSTRING "${_header}" 0 2 _first_byte)
  if(NOT _first_byte STREQUAL "24")
    message(FATAL_ERROR "invalid VCD header: ${_header}")
  endif()
else()
  message(FATAL_ERROR "unsupported trace format: ${FORMAT}")
endif()
