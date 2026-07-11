include(Fuxi)
include(RiscvTests)

option(AXI_TB_FUXI_WITH_RISCV_TESTS
       "Build the locked 59-test upstream RV32 I/M/A manifest" ON)
option(AXI_TB_FUXI_RUN_MISALIGNED_CAPABILITY
       "Enable the non-gating upstream ma_data capability test" OFF)

# One source of truth for both the generated host configuration and all guest
# linker scripts/headers.
set(_fuxi_rom_base 0x00000000)
set(_fuxi_rom_size 0x00010000)
set(_fuxi_ram_base 0x80000000)
set(_fuxi_ram_size 0x08000000)
set(_fuxi_uart_base 0x10000000)
set(_fuxi_uart_size 0x00000100)
set(_fuxi_exit_base 0x10001000)
set(_fuxi_exit_size 0x00000004)

axi_tb_prepare_fuxi(
  STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/fuxi-stage"
  OUTPUT_VAR _fuxi_rtl
)

add_axi_testbench(
  TARGET fuxi_sim
  TOP fuxi_axi_adapter
  RTL_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/fuxi_axi_adapter.sv"
    ${_fuxi_rtl}
  NUM_AXI 3
  ADDR_WIDTH 32
  DATA_WIDTH 32
  ID_WIDTH 4
  ROM_BASE ${_fuxi_rom_base}
  ROM_SIZE ${_fuxi_rom_size}
  RAM_BASE ${_fuxi_ram_base}
  RAM_SIZE ${_fuxi_ram_size}
  UART_BASE ${_fuxi_uart_base}
  UART_SIZE ${_fuxi_uart_size}
  EXIT_BASE ${_fuxi_exit_base}
  EXIT_SIZE ${_fuxi_exit_size}
  VERILATOR_ARGS
    -Wno-DECLFILENAME
    -Wno-UNUSEDSIGNAL
    -Wno-VARHIDDEN
)

set(_riscv_tests_arguments)
if(AXI_TB_FUXI_WITH_RISCV_TESTS)
  list(APPEND _riscv_tests_arguments WITH_RISCV_TESTS)
endif()
axi_tb_add_riscv_software(
  TARGET fuxi_software
  OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/software"
  ${_riscv_tests_arguments}
  ROM_BASE ${_fuxi_rom_base}
  ROM_SIZE ${_fuxi_rom_size}
  RAM_BASE ${_fuxi_ram_base}
  RAM_SIZE ${_fuxi_ram_size}
  UART_BASE ${_fuxi_uart_base}
  EXIT_BASE ${_fuxi_exit_base}
  RESET_PC 0x00000200
  OUT_PASS _fuxi_pass_elf
  OUT_FAIL _fuxi_fail_elf
  OUT_TIMEOUT _fuxi_timeout_elf
  OUT_UART _fuxi_uart_elf
  OUT_XRET_IRQ _fuxi_xret_irq_elf
  OUT_MMIO_STORE_IRQ _fuxi_mmio_store_irq_elf
  OUT_SFENCE_UNCACHED _fuxi_sfence_uncached_elf
  OUT_SFENCE_WRITEBACK_FAULT _fuxi_sfence_writeback_fault_elf
  OUT_ACCESS_LOAD _fuxi_access_load_elf
  OUT_ACCESS_STORE _fuxi_access_store_elf
  OUT_ACCESS_FETCH _fuxi_access_fetch_elf
  OUT_ACCESS_DCACHE_WRITEBACK _fuxi_access_dcache_writeback_elf
  OUT_RISCV_NAMES _fuxi_riscv_names
  OUT_RISCV_ELFS _fuxi_riscv_elfs
  OUT_MA_DATA _fuxi_ma_data_elf
)
add_dependencies(fuxi_sim fuxi_software)

if(BUILD_TESTING)
  function(_fuxi_add_expected_test name elf expected_result max_cycles)
    set(_expected_output_argument)
    if(ARGC GREATER 4)
      set(_expected_output_argument "-DEXPECTED_OUTPUT=${ARGV4}")
    endif()
    add_test(
      NAME "${name}"
      COMMAND "${CMAKE_COMMAND}"
        "-DPROGRAM=$<TARGET_FILE:fuxi_sim>"
        "-DELF=${elf}"
        "-DEXPECTED_RESULT=${expected_result}"
        "-DMAX_CYCLES=${max_cycles}"
        -DSTALL_PROBABILITY=0.35
        -DSEED=12345
        ${_expected_output_argument}
        -P "${AXI_TB_ROOT_DIR}/software/run_expect.cmake"
    )
  endfunction()

  _fuxi_add_expected_test(fuxi_smoke_pass
    "${_fuxi_pass_elf}" 0 1000000)
  _fuxi_add_expected_test(fuxi_smoke_expected_fail
    "${_fuxi_fail_elf}" 1 1000000)
  _fuxi_add_expected_test(fuxi_smoke_timeout
    "${_fuxi_timeout_elf}" 124 256)
  add_test(
    NAME fuxi_smoke_uart
    COMMAND "${CMAKE_COMMAND}"
      "-DPROGRAM=$<TARGET_FILE:fuxi_sim>"
      "-DELF=${_fuxi_uart_elf}"
      "-DUART_INPUT=${AXI_TB_ROOT_DIR}/software/smoke/uart.in"
      -DEXPECTED_RESULT=0
      -DMAX_CYCLES=1000000
      -DSTALL_PROBABILITY=0.35
      -DSEED=12345
      "-DEXPECTED_OUTPUT=Fuxi"
      -P "${AXI_TB_ROOT_DIR}/software/run_expect.cmake"
  )
  set_tests_properties(
    fuxi_smoke_pass
    fuxi_smoke_expected_fail
    fuxi_smoke_timeout
    fuxi_smoke_uart
    PROPERTIES LABELS "fuxi;smoke" TIMEOUT 120
  )

  _fuxi_add_expected_test(fuxi_protocol_xret_pending_irq
    "${_fuxi_xret_irq_elf}" 0 1000000)
  set_tests_properties(fuxi_protocol_xret_pending_irq PROPERTIES
    LABELS "fuxi;protocol;interrupt;xret"
    TIMEOUT 120
  )

  add_test(
    NAME fuxi_protocol_mmio_store_irq_once
    COMMAND "${CMAKE_COMMAND}"
      "-DPROGRAM=$<TARGET_FILE:fuxi_sim>"
      "-DELF=${_fuxi_mmio_store_irq_elf}"
      -DEXPECTED_RESULT=0
      -DMAX_CYCLES=1000000
      -DSTALL_PROBABILITY=0.35
      -DSEED=12345
      -DEXTRA_ARGUMENTS=+fuxi-mmio-store-irq
      -DEXPECTED_STDOUT=Q
      -P "${AXI_TB_ROOT_DIR}/software/run_expect.cmake"
  )
  set_tests_properties(fuxi_protocol_mmio_store_irq_once PROPERTIES
    LABELS "fuxi;protocol;interrupt;mmio"
    TIMEOUT 120
  )

  _fuxi_add_expected_test(fuxi_protocol_sfence_uncached_route
    "${_fuxi_sfence_uncached_elf}" 0 1000000)
  _fuxi_add_expected_test(fuxi_protocol_sfence_writeback_fault
    "${_fuxi_sfence_writeback_fault_elf}" 1 1000000 "0x80000007")
  set_tests_properties(
    fuxi_protocol_sfence_uncached_route
    fuxi_protocol_sfence_writeback_fault
    PROPERTIES LABELS "fuxi;protocol;cache-maintenance;sfence" TIMEOUT 120
  )

  _fuxi_add_expected_test(fuxi_protocol_rresp_load_access_fault
    "${_fuxi_access_load_elf}" 1 1000000 "0x80000005")
  _fuxi_add_expected_test(fuxi_protocol_bresp_store_access_fault
    "${_fuxi_access_store_elf}" 1 1000000 "0x80000007")
  _fuxi_add_expected_test(fuxi_protocol_rresp_fetch_access_fault
    "${_fuxi_access_fetch_elf}" 1 1000000 "0x80000001")
  _fuxi_add_expected_test(fuxi_protocol_bresp_dcache_writeback_fault
    "${_fuxi_access_dcache_writeback_elf}" 1 1000000 "0x80000007")
  set_tests_properties(
    fuxi_protocol_rresp_load_access_fault
    fuxi_protocol_bresp_store_access_fault
    fuxi_protocol_rresp_fetch_access_fault
    fuxi_protocol_bresp_dcache_writeback_fault
    PROPERTIES LABELS "fuxi;protocol;response;access-fault" TIMEOUT 120
  )

  if(AXI_TB_FUXI_WITH_RISCV_TESTS)
    list(LENGTH _fuxi_riscv_names _name_count)
    list(LENGTH _fuxi_riscv_elfs _elf_count)
    if(NOT _name_count EQUAL 59 OR NOT _elf_count EQUAL 59)
      message(FATAL_ERROR
        "Fuxi CTest registration requires exactly 59 default ISA guests")
    endif()
    math(EXPR _last_test "${_name_count} - 1")
    foreach(_index RANGE 0 ${_last_test})
      list(GET _fuxi_riscv_names ${_index} _name)
      list(GET _fuxi_riscv_elfs ${_index} _elf)
      set(_ctest_name "fuxi_riscv_${_name}")
      _fuxi_add_expected_test("${_ctest_name}" "${_elf}" 0 10000000)
      if(_name MATCHES "^rv32ui-")
        set(_family rv32i)
      elseif(_name MATCHES "^rv32um-")
        set(_family rv32m)
      else()
        set(_family rv32a)
      endif()
      set(_labels "fuxi;riscv-tests;${_family}")
      if(_name STREQUAL "rv32ui-fence_i")
        list(APPEND _labels fence-i)

        # fence.i writes back dirty D-cache lines.  A high-stall replay keeps
        # the W-channel VALID/payload stability rule under explicit regression.
        add_test(
          NAME fuxi_protocol_dcache_w_backpressure
          COMMAND "${CMAKE_COMMAND}"
            "-DPROGRAM=$<TARGET_FILE:fuxi_sim>"
            "-DELF=${_elf}"
            -DEXPECTED_RESULT=0
            -DMAX_CYCLES=1000000
            -DSTALL_PROBABILITY=0.8
            -DSEED=1
            -P "${AXI_TB_ROOT_DIR}/software/run_expect.cmake"
        )
        set_tests_properties(fuxi_protocol_dcache_w_backpressure PROPERTIES
          LABELS "fuxi;protocol;backpressure;fence-i"
          TIMEOUT 300
        )
      endif()
      set_tests_properties("${_ctest_name}" PROPERTIES
        LABELS "${_labels}"
        TIMEOUT 300
      )
    endforeach()

    _fuxi_add_expected_test(fuxi_capability_misaligned_ma_data
      "${_fuxi_ma_data_elf}" 0 10000000)
    set_tests_properties(fuxi_capability_misaligned_ma_data PROPERTIES
      LABELS "fuxi;capability;misaligned"
      TIMEOUT 300
    )
    if(AXI_TB_FUXI_RUN_MISALIGNED_CAPABILITY)
      set_tests_properties(fuxi_capability_misaligned_ma_data PROPERTIES
        DISABLED FALSE)
      add_dependencies(fuxi_sim
        fuxi_software_capability_misaligned_ma_data)
    else()
      set_tests_properties(fuxi_capability_misaligned_ma_data PROPERTIES
        DISABLED TRUE)
    endif()
  endif()
endif()
