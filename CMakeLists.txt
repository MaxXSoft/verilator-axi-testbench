cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW)
endif()

project(
  verilator-axi-testbench
  VERSION 0.1.0
  DESCRIPTION "A reusable AXI4 testbench for Verilated initiators"
  LANGUAGES CXX
)

option(AXI_TB_BUILD_TESTS "Build the self-contained test suite" ON)
option(AXI_TB_ENABLE_TRACE "Enable VCD tracing for Verilated testbenches" OFF)
option(AXI_TB_ENABLE_FST_TRACE
  "Enable FST tracing for Verilated testbenches" OFF)
set(AXI_TB_THREADS "1" CACHE STRING
  "Default number of threads in each generated Verilator model")
option(AXI_TB_WARNINGS_AS_ERRORS "Treat warnings in handwritten C++ as errors" OFF)
option(AXI_TB_BUILD_FUXI "Build the external Fuxi integration example" OFF)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Keep the source root available to add_axi_testbench(), including when the
# function is called from a nested tests/examples directory.
set(AXI_TB_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL
    "verilator-axi-testbench source root")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(AxiTestbench)

add_library(axi_tb_core STATIC
  src/devices.cpp
  src/elf_loader.cpp
)
add_library(axi_tb::core ALIAS axi_tb_core)

target_compile_features(axi_tb_core PUBLIC cxx_std_20)
target_include_directories(axi_tb_core
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

if(MSVC)
  target_compile_options(axi_tb_core PRIVATE /W4)
  if(AXI_TB_WARNINGS_AS_ERRORS)
    target_compile_options(axi_tb_core PRIVATE /WX)
  endif()
else()
  target_compile_options(axi_tb_core PRIVATE -Wall -Wextra -Wpedantic)
  if(AXI_TB_WARNINGS_AS_ERRORS)
    target_compile_options(axi_tb_core PRIVATE -Werror)
  endif()
endif()

include(CTest)
if(BUILD_TESTING AND AXI_TB_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(AXI_TB_BUILD_FUXI)
  add_subdirectory(examples/fuxi)
endif()
