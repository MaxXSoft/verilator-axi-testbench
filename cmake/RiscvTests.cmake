include_guard(GLOBAL)

include(CMakeParseArguments)

if(NOT DEFINED AXI_TB_ROOT_DIR)
  get_filename_component(AXI_TB_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(AXI_TB_RISCV_CLANG "" CACHE FILEPATH
    "Clang with a RISC-V backend; leave empty for validated discovery")
set(AXI_TB_RISCV_LLD "" CACHE FILEPATH
    "ELF ld.lld executable; leave empty for validated discovery")
set(AXI_TB_RISCV_TESTS_SOURCE_DIR "" CACHE PATH
    "Path to an upstream riscv-tests checkout")

function(_axi_tb_validate_lld candidate output_valid output_text)
  if(NOT candidate OR NOT EXISTS "${candidate}" OR IS_DIRECTORY "${candidate}")
    set(${output_valid} FALSE PARENT_SCOPE)
    set(${output_text} "not an executable file" PARENT_SCOPE)
    return()
  endif()
  execute_process(
    COMMAND "${candidate}" --version
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
    RESULT_VARIABLE _result
  )
  string(CONCAT _text "${_stdout}" "${_stderr}")
  if(_result EQUAL 0 AND _text MATCHES "(^|[ \t\r\n])LLD[ \t]+[0-9]+")
    set(${output_valid} TRUE PARENT_SCOPE)
  else()
    set(${output_valid} FALSE PARENT_SCOPE)
  endif()
  string(STRIP "${_text}" _text)
  set(${output_text} "${_text}" PARENT_SCOPE)
endfunction()

function(_axi_tb_find_riscv_lld output_var)
  set(_candidates)
  set(_explicit FALSE)
  if(AXI_TB_RISCV_LLD)
    set(_explicit TRUE)
    list(APPEND _candidates "${AXI_TB_RISCV_LLD}")
  else()
    # 1. PATH.
    unset(_path_lld CACHE)
    unset(_path_lld)
    find_program(_path_lld NAMES ld.lld)
    set(_path_lld_result "${_path_lld}")
    unset(_path_lld CACHE)
    if(_path_lld_result)
      list(APPEND _candidates "${_path_lld_result}")
    endif()

    # 2. Common Homebrew prefixes, including the separate lld formula.
    list(APPEND _candidates
      /opt/homebrew/opt/lld/bin/ld.lld
      /opt/homebrew/opt/llvm/bin/ld.lld
      /usr/local/opt/lld/bin/ld.lld
      /usr/local/opt/llvm/bin/ld.lld
    )
    unset(_brew CACHE)
    unset(_brew)
    find_program(_brew NAMES brew)
    set(_brew_result "${_brew}")
    unset(_brew CACHE)
    if(_brew_result)
      foreach(_formula IN ITEMS lld llvm)
        execute_process(
          COMMAND "${_brew_result}" --prefix "${_formula}"
          OUTPUT_VARIABLE _prefix
          ERROR_QUIET
          OUTPUT_STRIP_TRAILING_WHITESPACE
          RESULT_VARIABLE _prefix_result
        )
        if(_prefix_result EQUAL 0)
          list(APPEND _candidates "${_prefix}/bin/ld.lld")
        endif()
      endforeach()
    endif()

    # 3. Rust toolchains ship an ELF-flavoured LLD next to rustc.  Derive the
    # host component instead of assuming the build machine's architecture.
    unset(_rustc CACHE)
    unset(_rustc)
    find_program(_rustc NAMES rustc)
    set(_rustc_result "${_rustc}")
    unset(_rustc CACHE)
    if(_rustc_result)
      execute_process(
        COMMAND "${_rustc_result}" --print sysroot
        OUTPUT_VARIABLE _rust_sysroot
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _rust_sysroot_result
      )
      execute_process(
        COMMAND "${_rustc_result}" -vV
        OUTPUT_VARIABLE _rust_version
        ERROR_QUIET
        RESULT_VARIABLE _rust_version_result
      )
      if(_rust_sysroot_result EQUAL 0 AND _rust_version_result EQUAL 0 AND
         _rust_version MATCHES "host: ([^\r\n]+)")
        set(_rust_host "${CMAKE_MATCH_1}")
        list(APPEND _candidates
          "${_rust_sysroot}/lib/rustlib/${_rust_host}/bin/gcc-ld/ld.lld")
      endif()
    endif()
  endif()

  list(REMOVE_DUPLICATES _candidates)
  set(_diagnostics)
  foreach(_candidate IN LISTS _candidates)
    _axi_tb_validate_lld("${_candidate}" _valid _version)
    if(_valid)
      set(_lld "${_candidate}")
      break()
    endif()
    if(EXISTS "${_candidate}")
      string(APPEND _diagnostics "\n  ${_candidate}: ${_version}")
    endif()
  endforeach()

  if(NOT _lld)
    if(_explicit)
      set(_hint "AXI_TB_RISCV_LLD='${AXI_TB_RISCV_LLD}' is not LLVM LLD.")
    else()
      set(_hint
        "No ELF ld.lld was found in PATH, Homebrew, or rustc's sysroot.")
    endif()
    message(FATAL_ERROR
      "${_hint}\n"
      "Install LLD or set AXI_TB_RISCV_LLD explicitly. The macOS system "
      "linker is not a valid fallback.${_diagnostics}")
  endif()

  set(AXI_TB_RISCV_LLD "${_lld}" CACHE FILEPATH
      "ELF ld.lld executable; leave empty for validated discovery" FORCE)
  set(${output_var} "${_lld}" PARENT_SCOPE)
endfunction()

function(_axi_tb_find_riscv_toolchain output_clang output_lld)
  _axi_tb_find_riscv_lld(_lld)

  set(_clang_candidates)
  set(_explicit FALSE)
  if(AXI_TB_RISCV_CLANG)
    set(_explicit TRUE)
    list(APPEND _clang_candidates "${AXI_TB_RISCV_CLANG}")
  else()
    unset(_path_clang CACHE)
    unset(_path_clang)
    find_program(_path_clang NAMES clang)
    set(_path_clang_result "${_path_clang}")
    unset(_path_clang CACHE)
    if(_path_clang_result)
      list(APPEND _clang_candidates "${_path_clang_result}")
    endif()
    list(APPEND _clang_candidates
      /opt/homebrew/opt/llvm/bin/clang
      /usr/local/opt/llvm/bin/clang
    )
  endif()
  list(REMOVE_DUPLICATES _clang_candidates)

  set(_probe_dir "${CMAKE_BINARY_DIR}/axi-tb-riscv-toolchain-probe")
  set(_probe_tmp "${_probe_dir}/tmp")
  set(_probe_elf "${_probe_dir}/probe.elf")
  file(MAKE_DIRECTORY "${_probe_dir}" "${_probe_tmp}")
  set(_probe_source "${AXI_TB_ROOT_DIR}/software/runtime/toolchain_probe.S")
  set(_diagnostics)
  foreach(_candidate IN LISTS _clang_candidates)
    if(NOT EXISTS "${_candidate}" OR IS_DIRECTORY "${_candidate}")
      continue()
    endif()
    file(REMOVE "${_probe_elf}")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env "TMPDIR=${_probe_tmp}"
        "${_candidate}"
        --target=riscv32-unknown-elf
        -march=rv32ima_zicsr_zifencei
        -mabi=ilp32
        -nostdlib
        -static
        "-fuse-ld=${_lld}"
        "-Wl,--image-base=0,--no-relax,-Ttext=0x200"
        "${_probe_source}"
        -o "${_probe_elf}"
      OUTPUT_VARIABLE _stdout
      ERROR_VARIABLE _stderr
      RESULT_VARIABLE _result
    )
    if(_result EQUAL 0 AND EXISTS "${_probe_elf}")
      set(_clang "${_candidate}")
      break()
    endif()
    string(CONCAT _probe_text "${_stdout}" "${_stderr}")
    string(STRIP "${_probe_text}" _probe_text)
    string(APPEND _diagnostics "\n  ${_candidate}: ${_probe_text}")
  endforeach()

  if(NOT _clang)
    if(_explicit)
      set(_hint
        "AXI_TB_RISCV_CLANG='${AXI_TB_RISCV_CLANG}' failed the probe.")
    else()
      set(_hint "No discovered Clang could link the RV32IMA probe with LLD.")
    endif()
    message(FATAL_ERROR
      "${_hint}\n"
      "A Clang RISC-V backend compatible with ${_lld} is required. "
      "Set AXI_TB_RISCV_CLANG explicitly if needed.${_diagnostics}")
  endif()

  set(AXI_TB_RISCV_CLANG "${_clang}" CACHE FILEPATH
      "Clang with a RISC-V backend; leave empty for validated discovery" FORCE)
  set(${output_clang} "${_clang}" PARENT_SCOPE)
  set(${output_lld} "${_lld}" PARENT_SCOPE)
endfunction()

function(_axi_tb_add_riscv_elf)
  set(_one_value TARGET OUTPUT LINKER_SCRIPT CLANG LLD)
  set(_multi_value SOURCES INCLUDE_DIRS DEFINITIONS)
  cmake_parse_arguments(PARSE_ARGV 0 ELF "" "${_one_value}" "${_multi_value}")
  foreach(_required TARGET OUTPUT LINKER_SCRIPT CLANG LLD SOURCES)
    if(NOT ELF_${_required})
      message(FATAL_ERROR "_axi_tb_add_riscv_elf(): ${_required} is required")
    endif()
  endforeach()
  if(TARGET "${ELF_TARGET}")
    message(FATAL_ERROR "software target '${ELF_TARGET}' already exists")
  endif()

  get_filename_component(_output_dir "${ELF_OUTPUT}" DIRECTORY)
  set(_tmp_dir "${_output_dir}/tmp")
  set(_include_flags)
  foreach(_include IN LISTS ELF_INCLUDE_DIRS)
    list(APPEND _include_flags "-I${_include}")
  endforeach()
  set(_definition_flags)
  foreach(_definition IN LISTS ELF_DEFINITIONS)
    list(APPEND _definition_flags "-D${_definition}")
  endforeach()

  add_custom_command(
    OUTPUT "${ELF_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_output_dir}" "${_tmp_dir}"
    COMMAND "${CMAKE_COMMAND}" -E env "TMPDIR=${_tmp_dir}"
      "${ELF_CLANG}"
      --target=riscv32-unknown-elf
      -march=rv32ima_zicsr_zifencei
      -mabi=ilp32
      -nostdlib
      -static
      -fno-pic
      "-fuse-ld=${ELF_LLD}"
      ${_include_flags}
      ${_definition_flags}
      ${ELF_SOURCES}
      "-Wl,--image-base=0,--no-relax,--gc-sections,--build-id=none,-T,${ELF_LINKER_SCRIPT}"
      -o "${ELF_OUTPUT}"
    DEPENDS
      ${ELF_SOURCES}
      "${ELF_LINKER_SCRIPT}"
      "${AXI_TB_ROOT_DIR}/software/include/axi_tb_platform.h"
      "${AXI_TB_ROOT_DIR}/software/include/riscv_test.h"
    COMMAND_EXPAND_LISTS
    VERBATIM
    COMMENT "Building RV32 guest ${ELF_TARGET}"
  )
  add_custom_target("${ELF_TARGET}" DEPENDS "${ELF_OUTPUT}")
  set_property(TARGET "${ELF_TARGET}" PROPERTY AXI_TB_GUEST_ELF "${ELF_OUTPUT}")
endfunction()

function(_axi_tb_require_unsigned name value)
  if(NOT "${value}" MATCHES "^(0[xX][0-9A-Fa-f]+|[0-9]+)$")
    message(FATAL_ERROR "${name} must be an unsigned integer, got '${value}'")
  endif()
endfunction()

#[=======================================================================[.rst:
axi_tb_add_riscv_software
-------------------------

Build the self-contained smoke guests and, with ``WITH_RISCV_TESTS``, the
locked 59-test RV32 I/M/A suite plus a separate ``ma_data`` capability guest.
All guests use Clang's RV32 target and a validated ELF LLD.
#]=======================================================================]
function(axi_tb_add_riscv_software)
  set(_options WITH_RISCV_TESTS)
  set(_one_value
    TARGET OUTPUT_DIR RISCV_TESTS_DIR
    ROM_BASE ROM_SIZE RAM_BASE RAM_SIZE UART_BASE EXIT_BASE RESET_PC
    OUT_PASS OUT_FAIL OUT_TIMEOUT OUT_UART
    OUT_XRET_IRQ OUT_MMIO_STORE_IRQ
    OUT_ACCESS_LOAD OUT_ACCESS_STORE OUT_ACCESS_FETCH
    OUT_ACCESS_DCACHE_WRITEBACK
    OUT_RISCV_NAMES OUT_RISCV_ELFS OUT_MA_DATA
  )
  cmake_parse_arguments(PARSE_ARGV 0 SW "${_options}" "${_one_value}" "")
  if(SW_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "axi_tb_add_riscv_software(): unknown arguments: ${SW_UNPARSED_ARGUMENTS}")
  endif()
  foreach(_required
      TARGET OUTPUT_DIR ROM_BASE ROM_SIZE RAM_BASE RAM_SIZE UART_BASE EXIT_BASE
      RESET_PC)
    if(NOT SW_${_required})
      message(FATAL_ERROR
        "axi_tb_add_riscv_software(): ${_required} is required")
    endif()
  endforeach()
  if(TARGET "${SW_TARGET}")
    message(FATAL_ERROR "software target '${SW_TARGET}' already exists")
  endif()
  foreach(_address ROM_BASE ROM_SIZE RAM_BASE RAM_SIZE UART_BASE EXIT_BASE RESET_PC)
    _axi_tb_require_unsigned("${_address}" "${SW_${_address}}")
  endforeach()

  math(EXPR _rom_base "${SW_ROM_BASE}")
  math(EXPR _rom_size "${SW_ROM_SIZE}")
  math(EXPR _ram_base "${SW_RAM_BASE}")
  math(EXPR _ram_size "${SW_RAM_SIZE}")
  math(EXPR _reset_pc "${SW_RESET_PC}")
  if(_rom_size LESS_EQUAL 0 OR _ram_size LESS_EQUAL 0)
    message(FATAL_ERROR "ROM_SIZE and RAM_SIZE must be positive")
  endif()
  math(EXPR _rom_end "${_rom_base} + ${_rom_size}")
  math(EXPR _ram_end "${_ram_base} + ${_ram_size}")
  if(_reset_pc LESS _rom_base OR _reset_pc GREATER_EQUAL _rom_end)
    message(FATAL_ERROR "RESET_PC must lie inside ROM")
  endif()
  math(EXPR _reset_alignment "${_reset_pc} % 4")
  if(NOT _reset_alignment EQUAL 0)
    message(FATAL_ERROR "RESET_PC must be four-byte aligned")
  endif()
  if(_rom_base LESS _ram_end AND _ram_base LESS _rom_end)
    message(FATAL_ERROR "software ROM and RAM ranges overlap")
  endif()

  get_filename_component(_output_dir "${SW_OUTPUT_DIR}" ABSOLUTE
    BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
  file(MAKE_DIRECTORY "${_output_dir}")
  set(AXI_TB_SW_ROM_BASE "${SW_ROM_BASE}")
  set(AXI_TB_SW_ROM_SIZE "${SW_ROM_SIZE}")
  set(AXI_TB_SW_RAM_BASE "${SW_RAM_BASE}")
  set(AXI_TB_SW_RAM_SIZE "${SW_RAM_SIZE}")
  set(AXI_TB_SW_UART_BASE "${SW_UART_BASE}")
  set(AXI_TB_SW_EXIT_BASE "${SW_EXIT_BASE}")
  set(AXI_TB_SW_RESET_PC "${SW_RESET_PC}")
  set(_linker_script "${_output_dir}/link.ld")
  configure_file(
    "${AXI_TB_ROOT_DIR}/software/link.ld.in"
    "${_linker_script}"
    @ONLY NEWLINE_STYLE UNIX
  )

  _axi_tb_find_riscv_toolchain(_clang _lld)
  set(_runtime "${AXI_TB_ROOT_DIR}/software/runtime/start.S")
  set(_software_include "${AXI_TB_ROOT_DIR}/software/include")
  set(_definitions
    "AXI_TB_ROM_BASE=${SW_ROM_BASE}"
    "AXI_TB_ROM_SIZE=${SW_ROM_SIZE}"
    "AXI_TB_RAM_BASE=${SW_RAM_BASE}"
    "AXI_TB_RAM_SIZE=${SW_RAM_SIZE}"
    "AXI_TB_UART_BASE=${SW_UART_BASE}"
    "AXI_TB_EXIT_BASE=${SW_EXIT_BASE}"
    "AXI_TB_RESET_PC=${SW_RESET_PC}"
  )

  add_custom_target("${SW_TARGET}")
  foreach(_smoke IN ITEMS
      pass fail timeout uart xret_irq mmio_store_irq
      access_load access_store access_fetch access_dcache_writeback)
    set(_elf "${_output_dir}/smoke/${_smoke}.elf")
    set(_target "${SW_TARGET}_smoke_${_smoke}")
    _axi_tb_add_riscv_elf(
      TARGET "${_target}"
      OUTPUT "${_elf}"
      LINKER_SCRIPT "${_linker_script}"
      CLANG "${_clang}"
      LLD "${_lld}"
      SOURCES
        "${_runtime}"
        "${AXI_TB_ROOT_DIR}/software/smoke/${_smoke}.S"
      INCLUDE_DIRS "${_software_include}"
      DEFINITIONS ${_definitions}
    )
    add_dependencies("${SW_TARGET}" "${_target}")
    string(TOUPPER "${_smoke}" _smoke_upper)
    if(SW_OUT_${_smoke_upper})
      set(${SW_OUT_${_smoke_upper}} "${_elf}" PARENT_SCOPE)
    endif()
  endforeach()

  set(_riscv_names)
  set(_riscv_elfs)
  if(SW_WITH_RISCV_TESTS)
    if(SW_RISCV_TESTS_DIR)
      set(_riscv_tests_dir "${SW_RISCV_TESTS_DIR}")
    else()
      set(_riscv_tests_dir "${AXI_TB_RISCV_TESTS_SOURCE_DIR}")
    endif()
    if(NOT _riscv_tests_dir OR NOT IS_DIRECTORY "${_riscv_tests_dir}/isa")
      message(FATAL_ERROR
        "WITH_RISCV_TESTS requires an upstream checkout. Set "
        "AXI_TB_RISCV_TESTS_SOURCE_DIR or pass RISCV_TESTS_DIR.")
    endif()
    file(REAL_PATH "${_riscv_tests_dir}" _riscv_tests_dir)

    # 40 ordinary RV32I tests plus fence_i = 41 I tests.
    set(_i_tests
      simple add addi and andi auipc
      beq bge bgeu blt bltu bne
      fence_i jal jalr
      lb lbu ld_st lh lhu lui lw
      or ori sb sh sw st_ld
      sll slli slt slti sltiu sltu sra srai srl srli
      sub xor xori
    )
    set(_m_tests div divu mul mulh mulhsu mulhu rem remu)
    set(_a_tests
      amoadd_w amoand_w amomax_w amomaxu_w amomin_w amominu_w
      amoor_w amoxor_w amoswap_w lrsc
    )
    list(LENGTH _i_tests _i_count)
    list(LENGTH _m_tests _m_count)
    list(LENGTH _a_tests _a_count)
    math(EXPR _default_count "${_i_count} + ${_m_count} + ${_a_count}")
    if(NOT _i_count EQUAL 41 OR NOT _m_count EQUAL 8 OR
       NOT _a_count EQUAL 10 OR NOT _default_count EQUAL 59)
      message(FATAL_ERROR
        "Internal riscv-tests manifest must remain 41 I + 8 M + 10 A = 59")
    endif()

    set(_riscv_meta "${SW_TARGET}_riscv_tests")
    add_custom_target("${_riscv_meta}")
    foreach(_family IN ITEMS i m a)
      if(_family STREQUAL "i")
        set(_directory rv32ui)
      elseif(_family STREQUAL "m")
        set(_directory rv32um)
      else()
        set(_directory rv32ua)
      endif()
      foreach(_name IN LISTS _${_family}_tests)
        set(_source "${_riscv_tests_dir}/isa/${_directory}/${_name}.S")
        if(NOT EXISTS "${_source}")
          message(FATAL_ERROR
            "riscv-tests manifest source is missing: ${_source}")
        endif()
        set(_elf "${_output_dir}/riscv-tests/${_directory}/${_name}.elf")
        set(_target "${SW_TARGET}_${_directory}_${_name}")
        _axi_tb_add_riscv_elf(
          TARGET "${_target}"
          OUTPUT "${_elf}"
          LINKER_SCRIPT "${_linker_script}"
          CLANG "${_clang}"
          LLD "${_lld}"
          SOURCES "${_runtime}" "${_source}"
          INCLUDE_DIRS
            "${_software_include}"
            "${_riscv_tests_dir}/isa/macros/scalar"
          DEFINITIONS ${_definitions}
        )
        add_dependencies("${_riscv_meta}" "${_target}")
        add_dependencies("${SW_TARGET}" "${_target}")
        list(APPEND _riscv_names "${_directory}-${_name}")
        list(APPEND _riscv_elfs "${_elf}")
      endforeach()
    endforeach()
    list(LENGTH _riscv_elfs _built_count)
    if(NOT _built_count EQUAL 59)
      message(FATAL_ERROR
        "Default riscv-tests build has ${_built_count} guests; exactly 59 required")
    endif()

    # ma_data intentionally does not participate in the default 59-test
    # conformance gate: it measures hardware misaligned-access capability.
    set(_ma_source "${_riscv_tests_dir}/isa/rv32ui/ma_data.S")
    if(NOT EXISTS "${_ma_source}")
      message(FATAL_ERROR "riscv-tests capability source is missing: ${_ma_source}")
    endif()
    set(_ma_elf "${_output_dir}/riscv-tests/capability/ma_data.elf")
    set(_ma_target "${SW_TARGET}_capability_misaligned_ma_data")
    _axi_tb_add_riscv_elf(
      TARGET "${_ma_target}"
      OUTPUT "${_ma_elf}"
      LINKER_SCRIPT "${_linker_script}"
      CLANG "${_clang}"
      LLD "${_lld}"
      SOURCES "${_runtime}" "${_ma_source}"
      INCLUDE_DIRS
        "${_software_include}"
        "${_riscv_tests_dir}/isa/macros/scalar"
      DEFINITIONS ${_definitions}
    )
    set_property(TARGET "${_ma_target}" PROPERTY
      AXI_TB_CAPABILITY "misaligned")
    if(SW_OUT_MA_DATA)
      set(${SW_OUT_MA_DATA} "${_ma_elf}" PARENT_SCOPE)
    endif()
  endif()

  if(SW_OUT_RISCV_NAMES)
    set(${SW_OUT_RISCV_NAMES} "${_riscv_names}" PARENT_SCOPE)
  endif()
  if(SW_OUT_RISCV_ELFS)
    set(${SW_OUT_RISCV_ELFS} "${_riscv_elfs}" PARENT_SCOPE)
  endif()
endfunction()
