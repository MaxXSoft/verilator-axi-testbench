include_guard(GLOBAL)

include(CMakeParseArguments)

if(NOT DEFINED AXI_TB_ROOT_DIR)
  get_filename_component(AXI_TB_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

# Verilator's CMake package is normally installed below VERILATOR_ROOT rather
# than a platform-wide CMake package directory. Discover that root from the
# wrapper executable so a regular Homebrew/system installation works without
# an extra CMAKE_PREFIX_PATH.
find_program(AXI_TB_VERILATOR_EXECUTABLE NAMES verilator REQUIRED)
execute_process(
  COMMAND "${AXI_TB_VERILATOR_EXECUTABLE}" --version
  OUTPUT_VARIABLE _axi_tb_verilator_version_text
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE _axi_tb_verilator_version_result
)
if(NOT _axi_tb_verilator_version_result EQUAL 0 OR
   NOT _axi_tb_verilator_version_text MATCHES "Verilator ([0-9]+)\\.([0-9]+)")
  message(FATAL_ERROR
    "Unable to determine the Verilator version from: ${AXI_TB_VERILATOR_EXECUTABLE}")
endif()
set(_axi_tb_verilator_major "${CMAKE_MATCH_1}")
if(_axi_tb_verilator_major LESS 5)
  message(FATAL_ERROR
    "Verilator 5 or newer is required; found ${_axi_tb_verilator_version_text}")
endif()

set(_axi_tb_verilator_hints)
if(DEFINED VERILATOR_ROOT)
  list(APPEND _axi_tb_verilator_hints "${VERILATOR_ROOT}")
endif()
if(DEFINED ENV{VERILATOR_ROOT})
  list(APPEND _axi_tb_verilator_hints "$ENV{VERILATOR_ROOT}")
endif()
execute_process(
  COMMAND "${AXI_TB_VERILATOR_EXECUTABLE}" -V
  OUTPUT_VARIABLE _axi_tb_verilator_config
  ERROR_QUIET
)
if(_axi_tb_verilator_config MATCHES
   "VERILATOR_ROOT[ \t]*=[ \t]*([^\r\n]+)")
  string(STRIP "${CMAKE_MATCH_1}" _axi_tb_detected_verilator_root)
  list(APPEND _axi_tb_verilator_hints "${_axi_tb_detected_verilator_root}")
endif()

find_package(verilator 5 CONFIG REQUIRED HINTS ${_axi_tb_verilator_hints})

function(_axi_tb_default argument value)
  if(NOT DEFINED AXI_TB_${argument} OR "${AXI_TB_${argument}}" STREQUAL "")
    set(AXI_TB_${argument} "${value}" PARENT_SCOPE)
  endif()
endfunction()

function(_axi_tb_require_unsigned_literal argument)
  if(NOT "${AXI_TB_${argument}}" MATCHES "^(0[xX][0-9A-Fa-f]+|[0-9]+)$")
    message(FATAL_ERROR
      "add_axi_testbench(${AXI_TB_TARGET}): ${argument} must be an unsigned "
      "decimal or hexadecimal integer, got '${AXI_TB_${argument}}'")
  endif()
endfunction()

function(_axi_tb_validate_address_map)
  set(_check_dir
    "${CMAKE_CURRENT_BINARY_DIR}/axi_tb_range_checks/${AXI_TB_TARGET}")
  set(_check_source "${_check_dir}/address_map.cpp")
  file(MAKE_DIRECTORY "${_check_dir}")
  file(WRITE "${_check_source}" [=[
#include <cstdint>
#include <limits>

constexpr std::uint64_t rom_base = static_cast<std::uint64_t>(]=]
    "${AXI_TB_ROM_BASE}ULL);\n"
    "constexpr std::uint64_t rom_size = static_cast<std::uint64_t>(${AXI_TB_ROM_SIZE}ULL);\n"
    "constexpr std::uint64_t ram_base = static_cast<std::uint64_t>(${AXI_TB_RAM_BASE}ULL);\n"
    "constexpr std::uint64_t ram_size = static_cast<std::uint64_t>(${AXI_TB_RAM_SIZE}ULL);\n"
    "constexpr std::uint64_t uart_base = static_cast<std::uint64_t>(${AXI_TB_UART_BASE}ULL);\n"
    "constexpr std::uint64_t uart_size = static_cast<std::uint64_t>(${AXI_TB_UART_SIZE}ULL);\n"
    "constexpr std::uint64_t exit_base = static_cast<std::uint64_t>(${AXI_TB_EXIT_BASE}ULL);\n"
    "constexpr std::uint64_t exit_size = static_cast<std::uint64_t>(${AXI_TB_EXIT_SIZE}ULL);\n"
    "constexpr unsigned address_bits = ${AXI_TB_ADDR_WIDTH};\n"
    [=[
constexpr std::uint64_t max_value =
    std::numeric_limits<std::uint64_t>::max();

static_assert(rom_size != 0, "ROM_SIZE must be positive");
static_assert(ram_size != 0, "RAM_SIZE must be positive");
static_assert(uart_size != 0, "UART_SIZE must be positive");
static_assert(exit_size != 0, "EXIT_SIZE must be positive");
static_assert(rom_size <= max_value - rom_base, "ROM range overflows uint64");
static_assert(ram_size <= max_value - ram_base, "RAM range overflows uint64");
static_assert(uart_size <= max_value - uart_base, "UART range overflows uint64");
static_assert(exit_size <= max_value - exit_base, "EXIT range overflows uint64");

constexpr std::uint64_t rom_end = rom_base + rom_size;
constexpr std::uint64_t ram_end = ram_base + ram_size;
constexpr std::uint64_t uart_end = uart_base + uart_size;
constexpr std::uint64_t exit_end = exit_base + exit_size;
constexpr std::uint64_t limit32 = UINT64_C(0x100000000);

static_assert(address_bits == 64 || rom_end <= limit32,
              "ROM range does not fit ADDR_WIDTH=32");
static_assert(address_bits == 64 || ram_end <= limit32,
              "RAM range does not fit ADDR_WIDTH=32");
static_assert(address_bits == 64 || uart_end <= limit32,
              "UART range does not fit ADDR_WIDTH=32");
static_assert(address_bits == 64 || exit_end <= limit32,
              "EXIT range does not fit ADDR_WIDTH=32");

constexpr bool overlap(std::uint64_t lhs_base, std::uint64_t lhs_end,
                       std::uint64_t rhs_base, std::uint64_t rhs_end) {
  return lhs_base < rhs_end && rhs_base < lhs_end;
}
static_assert(!overlap(rom_base, rom_end, ram_base, ram_end),
              "ROM and RAM ranges overlap");
static_assert(!overlap(rom_base, rom_end, uart_base, uart_end),
              "ROM and UART ranges overlap");
static_assert(!overlap(rom_base, rom_end, exit_base, exit_end),
              "ROM and EXIT ranges overlap");
static_assert(!overlap(ram_base, ram_end, uart_base, uart_end),
              "RAM and UART ranges overlap");
static_assert(!overlap(ram_base, ram_end, exit_base, exit_end),
              "RAM and EXIT ranges overlap");
static_assert(!overlap(uart_base, uart_end, exit_base, exit_end),
              "UART and EXIT ranges overlap");

int main() { return 0; }
]=])

  # try_compile() stores its result in a cache entry before CMake 3.25.  Clear
  # it for every target so a prior valid map cannot mask a later invalid one.
  unset(_map_is_valid CACHE)
  unset(_map_is_valid)
  if(MSVC)
    set(_map_warning_flags "-DCMAKE_CXX_FLAGS=/WX")
  else()
    set(_map_warning_flags "-DCMAKE_CXX_FLAGS=-Werror")
  endif()
  try_compile(_map_is_valid
    "${_check_dir}/build"
    "${_check_source}"
    CMAKE_FLAGS
      "-DCMAKE_CXX_STANDARD=20"
      "-DCMAKE_CXX_EXTENSIONS=OFF"
      "${_map_warning_flags}"
    OUTPUT_VARIABLE _map_check_output)
  if(NOT _map_is_valid)
    message(FATAL_ERROR
      "add_axi_testbench(${AXI_TB_TARGET}): invalid address map.\n"
      "${_map_check_output}")
  endif()
endfunction()

#[=======================================================================[.rst:
add_axi_testbench
-----------------

Create one executable around a DUT that exposes the canonical packed AXI
initiator boundary from ``rtl/axi_tb_ports.svh``.  Every invocation receives
its own generated configuration directory and Verilator output directory.
The generated C++ model prefix is intentionally always ``Vaxi_tb_dut`` so the
shared ``src/sim_main.cpp`` and binding code never depend on a SystemVerilog
module name.

.. code-block:: cmake

  add_axi_testbench(
    TARGET       example_sim
    TOP          example_adapter
    RTL_SOURCES  example_adapter.sv core.sv
    NUM_AXI      1
    ADDR_WIDTH   64
    DATA_WIDTH   64
    ID_WIDTH     4
    THREADS      4
    ROM_BASE     0x00000000 ROM_SIZE  0x00010000
    RAM_BASE     0x80000000 RAM_SIZE  0x08000000
    UART_BASE    0x10000000 UART_SIZE 0x00000100
    EXIT_BASE    0x10001000 EXIT_SIZE 0x00000004
  )

``CXX_SOURCES``, ``INCLUDE_DIRS``, and ``VERILATOR_ARGS`` are optional
multi-value extensions. ``TRACE`` enables VCD for one target and ``TRACE_FST``
enables FST; otherwise the matching global option is used. A target cannot
enable both formats. ``THREADS`` selects the generated Verilator model's
thread count. ``EXIT_ADDRESS`` is accepted as an alias for ``EXIT_BASE``.
#]=======================================================================]
function(add_axi_testbench)
  if(DEFINED AXI_TB_THREADS)
    set(_default_threads "${AXI_TB_THREADS}")
  else()
    set(_default_threads 1)
  endif()
  set(_options TRACE TRACE_FST)
  set(_one_value
    TARGET TOP
    NUM_AXI ADDR_WIDTH DATA_WIDTH ID_WIDTH THREADS
    ROM_BASE ROM_SIZE
    RAM_BASE RAM_SIZE
    UART_BASE UART_SIZE
    EXIT_BASE EXIT_ADDRESS EXIT_SIZE
  )
  set(_multi_value RTL_SOURCES CXX_SOURCES INCLUDE_DIRS VERILATOR_ARGS)
  cmake_parse_arguments(PARSE_ARGV 0 AXI_TB
    "${_options}" "${_one_value}" "${_multi_value}")

  if(AXI_TB_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "add_axi_testbench(): unknown arguments: ${AXI_TB_UNPARSED_ARGUMENTS}")
  endif()
  foreach(_required TARGET TOP RTL_SOURCES)
    if(NOT AXI_TB_${_required})
      message(FATAL_ERROR "add_axi_testbench(): ${_required} is required")
    endif()
  endforeach()
  if(TARGET "${AXI_TB_TARGET}")
    message(FATAL_ERROR
      "add_axi_testbench(): target '${AXI_TB_TARGET}' already exists")
  endif()
  if(NOT TARGET axi_tb_core)
    message(FATAL_ERROR
      "add_axi_testbench(${AXI_TB_TARGET}) requires the axi_tb_core target")
  endif()

  _axi_tb_default(NUM_AXI 1)
  _axi_tb_default(ADDR_WIDTH 64)
  _axi_tb_default(DATA_WIDTH 64)
  _axi_tb_default(ID_WIDTH 4)
  _axi_tb_default(THREADS "${_default_threads}")
  _axi_tb_default(ROM_BASE 0x00000000)
  _axi_tb_default(ROM_SIZE 0x00010000)
  _axi_tb_default(RAM_BASE 0x80000000)
  _axi_tb_default(RAM_SIZE 0x08000000)
  _axi_tb_default(UART_BASE 0x10000000)
  _axi_tb_default(UART_SIZE 0x00000100)
  if(AXI_TB_EXIT_BASE AND AXI_TB_EXIT_ADDRESS AND
     NOT AXI_TB_EXIT_BASE STREQUAL AXI_TB_EXIT_ADDRESS)
    message(FATAL_ERROR
      "add_axi_testbench(${AXI_TB_TARGET}): EXIT_BASE and EXIT_ADDRESS disagree")
  endif()
  if(NOT AXI_TB_EXIT_BASE AND AXI_TB_EXIT_ADDRESS)
    set(AXI_TB_EXIT_BASE "${AXI_TB_EXIT_ADDRESS}")
  endif()
  _axi_tb_default(EXIT_BASE 0x10001000)
  _axi_tb_default(EXIT_SIZE 0x00000004)

  if(NOT AXI_TB_NUM_AXI MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR
      "add_axi_testbench(${AXI_TB_TARGET}): NUM_AXI must be positive")
  endif()
  if(NOT AXI_TB_ADDR_WIDTH MATCHES "^(32|64)$")
    message(FATAL_ERROR
      "add_axi_testbench(${AXI_TB_TARGET}): ADDR_WIDTH must be 32 or 64")
  endif()
  if(NOT AXI_TB_DATA_WIDTH MATCHES "^(32|64|128)$")
    message(FATAL_ERROR
      "add_axi_testbench(${AXI_TB_TARGET}): DATA_WIDTH must be 32, 64, or 128")
  endif()
  if(NOT AXI_TB_ID_WIDTH MATCHES "^[0-9]+$")
    message(FATAL_ERROR
      "add_axi_testbench(${AXI_TB_TARGET}): ID_WIDTH must be in [1, 32]")
  endif()
  if(AXI_TB_ID_WIDTH LESS 1 OR AXI_TB_ID_WIDTH GREATER 32)
    message(FATAL_ERROR
      "add_axi_testbench(${AXI_TB_TARGET}): ID_WIDTH must be in [1, 32]")
  endif()
  if(NOT AXI_TB_THREADS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR
      "add_axi_testbench(${AXI_TB_TARGET}): THREADS must be positive")
  endif()
  foreach(_argument IN LISTS AXI_TB_VERILATOR_ARGS)
    if(_argument MATCHES "^--threads($|[= ])")
      message(FATAL_ERROR
        "add_axi_testbench(${AXI_TB_TARGET}): use THREADS instead of passing "
        "${_argument} through VERILATOR_ARGS")
    endif()
  endforeach()
  foreach(_address_argument
      ROM_BASE ROM_SIZE RAM_BASE RAM_SIZE UART_BASE UART_SIZE
      EXIT_BASE EXIT_SIZE)
    _axi_tb_require_unsigned_literal(${_address_argument})
  endforeach()
  _axi_tb_validate_address_map()

  set(_trace_vcd_enabled FALSE)
  set(_trace_fst_enabled FALSE)
  if(AXI_TB_TRACE OR AXI_TB_ENABLE_TRACE)
    set(_trace_vcd_enabled TRUE)
  endif()
  if(AXI_TB_TRACE_FST OR AXI_TB_ENABLE_FST_TRACE)
    set(_trace_fst_enabled TRUE)
  endif()
  if(_trace_vcd_enabled AND _trace_fst_enabled)
    message(FATAL_ERROR
      "add_axi_testbench(${AXI_TB_TARGET}): VCD and FST tracing are mutually exclusive")
  endif()

  if(_trace_fst_enabled)
    set(AXI_TB_TRACE_ENABLED 1)
    set(AXI_TB_TRACE_FST_ENABLED 1)
    set(_trace_argument TRACE_FST)
  elseif(_trace_vcd_enabled)
    set(AXI_TB_TRACE_ENABLED 1)
    set(AXI_TB_TRACE_FST_ENABLED 0)
    set(_trace_argument TRACE_VCD)
  else()
    set(AXI_TB_TRACE_ENABLED 0)
    set(AXI_TB_TRACE_FST_ENABLED 0)
    set(_trace_argument)
  endif()

  set(_generated_dir
    "${CMAKE_CURRENT_BINARY_DIR}/axi_tb_generated/${AXI_TB_TARGET}")
  set(_verilated_dir
    "${CMAKE_CURRENT_BINARY_DIR}/verilated/${AXI_TB_TARGET}")
  file(MAKE_DIRECTORY "${_generated_dir}" "${_verilated_dir}")

  configure_file(
    "${AXI_TB_ROOT_DIR}/cmake/config.hpp.in"
    "${_generated_dir}/config.hpp"
    @ONLY NEWLINE_STYLE UNIX
  )
  configure_file(
    "${AXI_TB_ROOT_DIR}/cmake/axi_tb_env.h.in"
    "${_generated_dir}/axi_tb_env.h"
    @ONLY NEWLINE_STYLE UNIX
  )
  configure_file(
    "${AXI_TB_ROOT_DIR}/cmake/link.ld.in"
    "${_generated_dir}/link.ld"
    @ONLY NEWLINE_STYLE UNIX
  )

  set(_rtl_sources)
  foreach(_source IN LISTS AXI_TB_RTL_SOURCES)
    get_filename_component(_absolute_source "${_source}" ABSOLUTE
      BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${_absolute_source}")
      message(FATAL_ERROR
        "add_axi_testbench(${AXI_TB_TARGET}): RTL source not found: ${_source}")
    endif()
    list(APPEND _rtl_sources "${_absolute_source}")
  endforeach()

  add_executable("${AXI_TB_TARGET}"
    "${AXI_TB_ROOT_DIR}/src/sim_main.cpp"
    ${AXI_TB_CXX_SOURCES}
  )
  target_compile_features("${AXI_TB_TARGET}" PRIVATE cxx_std_20)
  target_link_libraries("${AXI_TB_TARGET}" PRIVATE axi_tb::core)
  if(_trace_fst_enabled)
    find_path(AXI_TB_LZ4_INCLUDE_DIR NAMES lz4.h REQUIRED)
    find_library(AXI_TB_LZ4_LIBRARY NAMES lz4 REQUIRED)
    find_package(ZLIB REQUIRED)
    get_filename_component(_lz4_library_dir
      "${AXI_TB_LZ4_LIBRARY}" DIRECTORY)
    set(_fst_library_dirs "${_lz4_library_dir}")
    foreach(_zlib_library IN LISTS ZLIB_LIBRARIES)
      if(IS_ABSOLUTE "${_zlib_library}")
        get_filename_component(_zlib_library_dir
          "${_zlib_library}" DIRECTORY)
        list(APPEND _fst_library_dirs "${_zlib_library_dir}")
        break()
      endif()
    endforeach()
    target_include_directories("${AXI_TB_TARGET}" SYSTEM PRIVATE
      "${AXI_TB_LZ4_INCLUDE_DIR}" ${ZLIB_INCLUDE_DIRS})
    # Verilator links FST runtimes with -llz4 -lz. Add the discovered search
    # directories because package-manager prefixes are not always compiler
    # defaults (notably Homebrew on Apple Silicon).
    target_link_directories("${AXI_TB_TARGET}" PRIVATE
      ${_fst_library_dirs})
  endif()
  target_include_directories("${AXI_TB_TARGET}" PRIVATE
    "${_generated_dir}"
    ${AXI_TB_INCLUDE_DIRS}
  )
  set_target_properties("${AXI_TB_TARGET}" PROPERTIES
    AXI_TB_CONFIG_HEADER "${_generated_dir}/config.hpp"
    AXI_TB_ENV_HEADER "${_generated_dir}/axi_tb_env.h"
    AXI_TB_LINKER_SCRIPT "${_generated_dir}/link.ld"
    AXI_TB_TOP_MODULE "${AXI_TB_TOP}"
    AXI_TB_NUM_PORTS "${AXI_TB_NUM_AXI}"
    AXI_TB_THREADS "${AXI_TB_THREADS}"
  )

  verilate("${AXI_TB_TARGET}"
    PREFIX Vaxi_tb_dut
    TOP_MODULE "${AXI_TB_TOP}"
    THREADS "${AXI_TB_THREADS}"
    DIRECTORY "${_verilated_dir}"
    SOURCES ${_rtl_sources}
    INCLUDE_DIRS "${AXI_TB_ROOT_DIR}/rtl" ${AXI_TB_INCLUDE_DIRS}
    ${_trace_argument}
    VERILATOR_ARGS
      --assert
      -Wall
      -Wno-fatal
      "-GNUM_AXI=${AXI_TB_NUM_AXI}"
      "-GADDR_WIDTH=${AXI_TB_ADDR_WIDTH}"
      "-GDATA_WIDTH=${AXI_TB_DATA_WIDTH}"
      "-GID_WIDTH=${AXI_TB_ID_WIDTH}"
      ${AXI_TB_VERILATOR_ARGS}
  )
endfunction()
