if(NOT DEFINED PROGRAM OR NOT EXISTS "${PROGRAM}")
  message(FATAL_ERROR "PROGRAM must name the built simulator")
endif()
if(NOT DEFINED ELF OR NOT EXISTS "${ELF}")
  message(FATAL_ERROR "ELF must name a built guest image")
endif()
if(NOT DEFINED EXPECTED_RESULT)
  message(FATAL_ERROR "EXPECTED_RESULT is required")
endif()
if(NOT DEFINED MAX_CYCLES)
  set(MAX_CYCLES 1000000)
endif()

set(_command "${PROGRAM}" --elf "${ELF}" --max-cycles "${MAX_CYCLES}")
if(DEFINED STALL_PROBABILITY)
  list(APPEND _command --stall-probability "${STALL_PROBABILITY}")
endif()
if(DEFINED SEED)
  list(APPEND _command --seed "${SEED}")
endif()
if(DEFINED UART_INPUT)
  list(APPEND _command --uart-in "${UART_INPUT}")
endif()
if(DEFINED UART_OUTPUT)
  list(APPEND _command --uart-out "${UART_OUTPUT}")
endif()

execute_process(
  COMMAND ${_command}
  OUTPUT_VARIABLE _stdout
  ERROR_VARIABLE _stderr
  RESULT_VARIABLE _result
)
if(NOT "${_result}" STREQUAL "${EXPECTED_RESULT}")
  message(FATAL_ERROR
    "Simulator returned ${_result}, expected ${EXPECTED_RESULT}.\n"
    "stdout:\n${_stdout}\n"
    "stderr:\n${_stderr}")
endif()

if(DEFINED EXPECTED_OUTPUT AND
   NOT "${_stdout}${_stderr}" MATCHES "${EXPECTED_OUTPUT}")
  message(FATAL_ERROR
    "Simulator output did not match '${EXPECTED_OUTPUT}'.\n"
    "stdout:\n${_stdout}\n"
    "stderr:\n${_stderr}")
endif()

if(_stdout)
  message("${_stdout}")
endif()
if(_stderr)
  message("${_stderr}")
endif()
