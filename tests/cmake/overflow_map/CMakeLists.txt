cmake_minimum_required(VERSION 3.20)
project(axi_tb_overflow_map LANGUAGES CXX)

if(NOT DEFINED AXI_TB_SOURCE_ROOT)
  message(FATAL_ERROR "AXI_TB_SOURCE_ROOT is required")
endif()
set(AXI_TB_ROOT_DIR "${AXI_TB_SOURCE_ROOT}")
list(APPEND CMAKE_MODULE_PATH "${AXI_TB_SOURCE_ROOT}/cmake")
include(AxiTestbench)

add_library(axi_tb_core INTERFACE)
add_library(axi_tb::core ALIAS axi_tb_core)

# Both spellings of UINT64_MAX must pass the compiler-independent literal
# check before the overflowing value below is rejected.
set(AXI_TB_TARGET uint64_literal_boundary)
set(AXI_TB_BOUNDARY_VALUE 18446744073709551615)
_axi_tb_require_unsigned_literal(BOUNDARY_VALUE)
set(AXI_TB_BOUNDARY_VALUE 0xffffffffffffffff)
_axi_tb_require_unsigned_literal(BOUNDARY_VALUE)

if(NOT DEFINED AXI_TB_OVERFLOW_VALUE)
  set(AXI_TB_OVERFLOW_VALUE 18446744073709551616)
endif()

add_axi_testbench(
  TARGET overflow_map
  TOP axi_tb_canonical_top
  RTL_SOURCES "${AXI_TB_SOURCE_ROOT}/rtl/axi_tb_canonical_top.sv"
  NUM_AXI 1
  ADDR_WIDTH 64
  DATA_WIDTH 64
  ID_WIDTH 4
  ROM_BASE ${AXI_TB_OVERFLOW_VALUE}
  ROM_SIZE 1
  RAM_BASE 0x80000000
  RAM_SIZE 0x00001000
  UART_BASE 0x10000000
  UART_SIZE 0x00000100
  EXIT_BASE 0x10001000
  EXIT_SIZE 0x00000004
)
