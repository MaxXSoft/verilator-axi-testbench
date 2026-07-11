cmake_minimum_required(VERSION 3.20)
project(axi_tb_invalid_map LANGUAGES CXX)

if(NOT DEFINED AXI_TB_SOURCE_ROOT)
  message(FATAL_ERROR "AXI_TB_SOURCE_ROOT is required")
endif()
set(AXI_TB_ROOT_DIR "${AXI_TB_SOURCE_ROOT}")
list(APPEND CMAKE_MODULE_PATH "${AXI_TB_SOURCE_ROOT}/cmake")
include(AxiTestbench)

add_library(axi_tb_core INTERFACE)
add_library(axi_tb::core ALIAS axi_tb_core)

# Exercise the multi-target case: a valid first invocation must not cache a
# successful try_compile result that masks the invalid map below.
add_axi_testbench(
  TARGET valid_map_before_invalid
  TOP axi_tb_canonical_top
  RTL_SOURCES "${AXI_TB_SOURCE_ROOT}/rtl/axi_tb_canonical_top.sv"
  NUM_AXI 1
  ADDR_WIDTH 64
  DATA_WIDTH 64
  ID_WIDTH 4
  ROM_BASE 0x00000000
  ROM_SIZE 0x00001000
  RAM_BASE 0x80000000
  RAM_SIZE 0x00001000
  UART_BASE 0x10000000
  UART_SIZE 0x00000100
  EXIT_BASE 0x10001000
  EXIT_SIZE 0x00000004
)

add_axi_testbench(
  TARGET invalid_map
  TOP axi_tb_canonical_top
  RTL_SOURCES "${AXI_TB_SOURCE_ROOT}/rtl/axi_tb_canonical_top.sv"
  NUM_AXI 1
  ADDR_WIDTH 64
  DATA_WIDTH 64
  ID_WIDTH 4
  ROM_BASE 0x00000000
  ROM_SIZE 0x00001000
  RAM_BASE 0x00000800
  RAM_SIZE 0x00001000
  UART_BASE 0x10000000
  UART_SIZE 0x00000100
  EXIT_BASE 0x10001000
  EXIT_SIZE 0x00000004
)
