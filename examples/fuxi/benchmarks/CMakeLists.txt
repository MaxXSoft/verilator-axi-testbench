set(AXI_TB_DHRYSTONE_SOURCE_DIR "${AXI_TB_ROOT_DIR}/third_party/benchmark-dhrystone"
    CACHE PATH "SiFive benchmark-dhrystone checkout")
set(AXI_TB_COREMARK_SOURCE_DIR "${AXI_TB_ROOT_DIR}/third_party/coremark"
    CACHE PATH "EEMBC CoreMark checkout")
set(AXI_TB_DHRYSTONE_ITERATIONS 2000 CACHE STRING "Dhrystone measured iterations")
set(AXI_TB_COREMARK_ITERATIONS 10 CACHE STRING "CoreMark measured iterations")
option(AXI_TB_FUXI_BENCHMARK_FIXED_LAYOUT
       "Also build controlled-layout benchmark guests with rodata fixed at 0x5000" OFF)
option(AXI_TB_FUXI_BENCHMARK_PMU
       "Also build six event-group guests requiring Fuxi grouped PMU events and eight HPM counters" OFF)
foreach(_setting AXI_TB_DHRYSTONE_ITERATIONS AXI_TB_COREMARK_ITERATIONS)
  if(NOT "${${_setting}}" MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "${_setting} must be a positive integer")
  endif()
endforeach()
foreach(_source "${AXI_TB_DHRYSTONE_SOURCE_DIR}/dhry_1.c"
                "${AXI_TB_COREMARK_SOURCE_DIR}/core_main.c")
  if(NOT EXISTS "${_source}")
    message(FATAL_ERROR "Missing benchmark source ${_source}; initialize submodules")
  endif()
endforeach()

_axi_tb_find_riscv_toolchain(_clang _lld WITH_RVC)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_measurements baseline)
if(AXI_TB_FUXI_BENCHMARK_PMU)
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/pmu.py"
    --header "${CMAKE_CURRENT_BINARY_DIR}/pmu_groups.h"
    --cmake "${CMAKE_CURRENT_BINARY_DIR}/pmu_groups.cmake"
    COMMAND_ERROR_IS_FATAL ANY)
  include("${CMAKE_CURRENT_BINARY_DIR}/pmu_groups.cmake")
  list(APPEND _measurements ${FUXI_PMU_GROUP_NAMES})
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               "${CMAKE_CURRENT_SOURCE_DIR}/pmu.py")
endif()
if(BUILD_TESTING)
  add_test(NAME fuxi_benchmark_pmu_parser
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_pmu.py")
  set_tests_properties(fuxi_benchmark_pmu_parser PROPERTIES LABELS "fuxi;benchmark-tooling" TIMEOUT 30)
endif()

# Replace only the legacy floating-point score printer. Keep its workload,
# timing hooks and all reported final values byte-for-byte upstream.
file(READ "${AXI_TB_DHRYSTONE_SOURCE_DIR}/dhry_1.c" _dhry)
string(FIND "${_dhry}" "  User_Time = End_Time - Begin_Time;" _report_start)
string(FIND "${_dhry}" "\n}\n\n\nProc_1" _report_end)
if(_report_start LESS 0 OR _report_end LESS _report_start)
  message(FATAL_ERROR "Unsupported Dhrystone score-printer layout")
endif()
string(SUBSTRING "${_dhry}" 0 ${_report_start} _dhry_prefix)
string(SUBSTRING "${_dhry}" ${_report_end} -1 _dhry_suffix)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/dhry_1.c"
     "${_dhry_prefix}  /* Scores are computed from CSR deltas by run.py. */\n${_dhry_suffix}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${AXI_TB_DHRYSTONE_SOURCE_DIR}/dhry_1.c")

add_custom_target(fuxi_benchmarks)
set(_variants rv32ima rv32imac)
if(AXI_TB_FUXI_BENCHMARK_FIXED_LAYOUT)
  list(APPEND _variants rv32ima-fixed-rodata rv32imac-fixed-rodata)
  file(READ "${CMAKE_CURRENT_BINARY_DIR}/../software/link.ld" _fixed_script)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/fixed-rodata.ld" "${_fixed_script}\n"
    "ASSERT(ADDR(.text) + SIZEOF(.text) <= 0x5000, \"benchmark text exceeds fixed rodata base\");\n"
    "ASSERT(ADDR(.rodata) == 0x5000, \"benchmark rodata placement changed\");\n")
endif()
foreach(_variant IN LISTS _variants)
  string(REGEX REPLACE "-fixed-rodata$" "" _isa "${_variant}")
  set(_linker_script "${CMAKE_CURRENT_BINARY_DIR}/../software/link.ld")
  set(_layout_flags)
  set(_layout natural)
  if(_variant MATCHES "-fixed-rodata$")
    set(_linker_script "${CMAKE_CURRENT_BINARY_DIR}/fixed-rodata.ld")
    set(_layout_flags -Wl,--section-start=.rodata=0x5000)
    set(_layout fixed-rodata)
  endif()
  foreach(_measurement IN LISTS _measurements)
    set(_suffix)
    set(_pmu_definitions)
    set(_pmu_sources)
    set(_pmu_headers)
    set(_pmu_arguments)
    set(_pmu_label)
    if(NOT _measurement STREQUAL "baseline")
      set(_group "${FUXI_PMU_GROUP_${_measurement}}")
      set(_suffix "-pmu-${_measurement}")
      set(_pmu_definitions "-DFUXI_PMU_GROUP=${_group}")
      set(_pmu_sources pmu.S)
      set(_pmu_headers "${CMAKE_CURRENT_BINARY_DIR}/pmu_groups.h")
      set(_pmu_arguments --pmu-group "${_group}")
      set(_pmu_label ";pmu;pmu-${_measurement}")
    endif()
    foreach(_benchmark dhrystone coremark)
      set(_elf "${CMAKE_CURRENT_BINARY_DIR}/${_benchmark}-${_variant}${_suffix}.elf")
      set(_target "fuxi_benchmark_${_benchmark}_${_variant}${_suffix}")
      set(_flags -O3 -ffreestanding -fno-builtin -fno-common -fno-pic
                 -falign-functions=4 -ffunction-sections -fdata-sections)
      set(_sources start.S runtime.c ${_pmu_sources})
      set(_definitions ${_pmu_definitions})
      if(_benchmark STREQUAL "dhrystone")
        list(APPEND _sources dhrystone.c "${AXI_TB_DHRYSTONE_SOURCE_DIR}/dhry_2.c")
        set(_include "${AXI_TB_DHRYSTONE_SOURCE_DIR}")
        list(APPEND _flags -std=gnu89 -Wno-implicit-int -Wno-implicit-function-declaration
             -Wno-deprecated-non-prototype -Wno-return-type)
        list(APPEND _definitions -DTIME -DNOENUM "-DDHRY_ITERS=${AXI_TB_DHRYSTONE_ITERATIONS}")
        set(_headers "${AXI_TB_DHRYSTONE_SOURCE_DIR}/dhry.h"
                     "${AXI_TB_DHRYSTONE_SOURCE_DIR}/dhry_1.c")
      else()
        list(APPEND _sources core_portme.c)
        foreach(_source core_main core_list_join core_matrix core_state core_util)
          list(APPEND _sources "${AXI_TB_COREMARK_SOURCE_DIR}/${_source}.c")
        endforeach()
        set(_include "${AXI_TB_COREMARK_SOURCE_DIR}")
        list(APPEND _flags -std=c99)
        list(APPEND _definitions "-DITERATIONS=${AXI_TB_COREMARK_ITERATIONS}")
        set(_headers "${AXI_TB_COREMARK_SOURCE_DIR}/coremark.h" core_portme.h)
      endif()
      string(JOIN " " _flags_text ${_flags})
      set(_flags_text "${_flags_text} -march=${_isa}_zicsr_zifencei -mabi=ilp32 (no LTO, no relax)")
      list(APPEND _definitions "-DFLAGS_STR=\"${_flags_text}\"")
      add_custom_command(OUTPUT "${_elf}"
        COMMAND "${_clang}" --target=riscv32-unknown-elf
          "-march=${_isa}_zicsr_zifencei" -mabi=ilp32 -nostdlib -static
          "-fuse-ld=${_lld}" ${_flags} ${_definitions}
          "-I${CMAKE_CURRENT_BINARY_DIR}" "-I${CMAKE_CURRENT_SOURCE_DIR}/include" "-I${CMAKE_CURRENT_SOURCE_DIR}"
          "-I${_include}" ${_sources}
          "-Wl,--image-base=0,--no-relax,--gc-sections,--build-id=none,-T,${_linker_script}"
          ${_layout_flags}
          -o "${_elf}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        DEPENDS ${_sources} ${_headers} ${_pmu_headers} include/benchmark.h include/stdio.h
                "${_linker_script}"
        VERBATIM COMMENT "Building ${_benchmark} ${_variant}${_suffix}")
      add_custom_target("${_target}" DEPENDS "${_elf}")
      add_dependencies(fuxi_benchmarks "${_target}")
      if(BUILD_TESTING)
        add_test(NAME "${_target}"
          COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run.py"
            --simulator "$<TARGET_FILE:fuxi_sim>" --elf "${_elf}"
            --output "${CMAKE_CURRENT_BINARY_DIR}/results/${_benchmark}-${_variant}${_suffix}.json"
            ${_pmu_arguments})
        set_tests_properties("${_target}"
          PROPERTIES LABELS "fuxi;benchmark;${_isa};${_layout}${_pmu_label}" TIMEOUT 300)
      endif()
    endforeach()
  endforeach()
endforeach()
