function(axi_tb_add_host_test source)
  get_filename_component(_stem "${source}" NAME_WE)
  set(_target "axi_tb_${_stem}")
  add_executable("${_target}" "${source}")
  target_compile_features("${_target}" PRIVATE cxx_std_20)
  target_link_libraries("${_target}" PRIVATE axi_tb::core)
  add_test(NAME "core.${_stem}" COMMAND "${_target}")
  set_tests_properties("core.${_stem}" PROPERTIES
    LABELS "core"
    TIMEOUT 30
  )
endfunction()

# Standalone host tests use a host_*.cpp naming convention and each owns its
# main(). CONFIGURE_DEPENDS lets concurrently added suites appear on reconfigure.
file(GLOB _axi_tb_host_tests CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/host_*.cpp")
foreach(_source IN LISTS _axi_tb_host_tests)
  axi_tb_add_host_test("${_source}")
endforeach()

add_test(
  NAME core.cmake_invalid_address_map
  COMMAND "${CMAKE_COMMAND}"
    "-DTEST_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/invalid_map"
    "-DTEST_BINARY=${CMAKE_CURRENT_BINARY_DIR}/cmake-invalid-map"
    "-DAXI_TB_SOURCE_ROOT=${AXI_TB_ROOT_DIR}"
    "-DEXPECTED_PATTERN=ROM and RAM ranges overlap"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_configure_failure.cmake"
)
set_tests_properties(core.cmake_invalid_address_map PROPERTIES
  LABELS "core;cmake"
  TIMEOUT 30
)

add_test(
  NAME core.cmake_uint64_overflow
  COMMAND "${CMAKE_COMMAND}"
    "-DTEST_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/overflow_map"
    "-DTEST_BINARY=${CMAKE_CURRENT_BINARY_DIR}/cmake-overflow-map"
    "-DAXI_TB_SOURCE_ROOT=${AXI_TB_ROOT_DIR}"
    "-DEXPECTED_PATTERN=invalid address map"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_configure_failure.cmake"
)
set_tests_properties(core.cmake_uint64_overflow PROPERTIES
  LABELS "core;cmake"
  TIMEOUT 30
)

function(axi_tb_add_rtl_lint_test name top source num_axi addr_width
         data_width id_width labels)
  add_test(
    NAME "${name}"
    COMMAND "${AXI_TB_VERILATOR_EXECUTABLE}"
      --lint-only
      --sv
      --assert
      -Wall
      -Wno-fatal
      --top-module "${top}"
      "-I${AXI_TB_ROOT_DIR}/rtl"
      "-GNUM_AXI=${num_axi}"
      "-GADDR_WIDTH=${addr_width}"
      "-GDATA_WIDTH=${data_width}"
      "-GID_WIDTH=${id_width}"
      "${source}"
  )
  set_tests_properties("${name}" PROPERTIES
    LABELS "${labels};rtl"
    TIMEOUT 30
  )
endfunction()

axi_tb_add_rtl_lint_test(
  rtl.canonical_ports
  axi_tb_canonical_top
  "${AXI_TB_ROOT_DIR}/rtl/axi_tb_canonical_top.sv"
  2 64 64 4
  core
)

set(_protocol_fixture
  "${CMAKE_CURRENT_SOURCE_DIR}/rtl/axi_tb_protocol_fixture.sv")
axi_tb_add_rtl_lint_test(
  protocol.shape_32x32_id1
  axi_tb_protocol_fixture
  "${_protocol_fixture}"
  1 32 32 1
  protocol
)
axi_tb_add_rtl_lint_test(
  protocol.shape_64x64_id32
  axi_tb_protocol_fixture
  "${_protocol_fixture}"
  2 64 64 32
  protocol
)
axi_tb_add_rtl_lint_test(
  protocol.shape_64x128
  axi_tb_protocol_fixture
  "${_protocol_fixture}"
  3 64 128 8
  protocol
)

# Build and execute the C++ packed binding for every required public shape;
# lint alone would not catch a wrong Verilator scalar/QData/WData layout.
function(axi_tb_add_runtime_shape suffix num_axi addr_width data_width id_width)
  set(_target "axi_tb_runtime_${suffix}")
  set(_thread_arguments)
  if(ARGC GREATER 5)
    list(APPEND _thread_arguments THREADS "${ARGV5}")
  endif()
  add_axi_testbench(
    TARGET "${_target}"
    TOP axi_tb_protocol_fixture
    RTL_SOURCES "${_protocol_fixture}"
    NUM_AXI "${num_axi}"
    ADDR_WIDTH "${addr_width}"
    DATA_WIDTH "${data_width}"
    ID_WIDTH "${id_width}"
    ${_thread_arguments}
    ROM_BASE 0x00000000
    ROM_SIZE 0x00010000
    RAM_BASE 0x80000000
    RAM_SIZE 0x08000000
    UART_BASE 0x10000000
    UART_SIZE 0x00000100
    EXIT_BASE 0x10001000
    EXIT_SIZE 0x00000004
  )
  add_test(
    NAME "protocol.runtime_${suffix}"
    COMMAND "${_target}" --max-cycles 1000 --reset-cycles 5
  )
  set_tests_properties("protocol.runtime_${suffix}" PROPERTIES
    LABELS "core;protocol;rtl"
    TIMEOUT 30
  )

  if(suffix STREQUAL "32x32_n1" AND
     (AXI_TB_ENABLE_TRACE OR AXI_TB_ENABLE_FST_TRACE))
    if(AXI_TB_ENABLE_FST_TRACE)
      set(_trace_format FST)
      set(_trace_extension fst)
    else()
      set(_trace_format VCD)
      set(_trace_extension vcd)
    endif()
    set(_trace_output
      "${CMAKE_CURRENT_BINARY_DIR}/${_target}.${_trace_extension}")
    add_test(
      NAME "protocol.trace_${_trace_extension}"
      COMMAND "${CMAKE_COMMAND}"
        "-DPROGRAM=$<TARGET_FILE:${_target}>"
        "-DOUTPUT=${_trace_output}"
        "-DFORMAT=${_trace_format}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_trace.cmake"
    )
    set_tests_properties("protocol.trace_${_trace_extension}" PROPERTIES
      LABELS "core;protocol;rtl;trace;${_trace_extension}"
      TIMEOUT 30
    )
  endif()
endfunction()

axi_tb_add_runtime_shape(32x32_n1 1 32 32 1 2)
axi_tb_add_runtime_shape(64x64_n2 2 64 64 32)
axi_tb_add_runtime_shape(64x128_n3 3 64 128 8)
