include_guard(GLOBAL)

include(CMakeParseArguments)

set(AXI_TB_FUXI_SOURCE_DIR "" CACHE PATH
    "Path to an existing Fuxi checkout (used only when integration is enabled)")
set(AXI_TB_FUXI_JAVA_HOME "" CACHE PATH
    "Optional Java 17+ home used to elaborate Fuxi")
set(AXI_TB_FUXI_SBT_EXECUTABLE "" CACHE FILEPATH
    "Path to sbt; leave empty to search PATH")

# Stage exactly the source material needed by Fuxi's Chisel build and
# elaborate Fuxi.v during CMake configuration.  add_axi_testbench() asks
# Verilator to inspect all RTL at configure time, so deferring elaboration to a
# build rule would be too late.
function(axi_tb_prepare_fuxi)
  set(_one_value SOURCE_DIR STAGE_DIR OUTPUT_VAR)
  cmake_parse_arguments(PARSE_ARGV 0 FUXI "" "${_one_value}" "")

  if(FUXI_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "axi_tb_prepare_fuxi(): unknown arguments: ${FUXI_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT FUXI_OUTPUT_VAR)
    message(FATAL_ERROR "axi_tb_prepare_fuxi(): OUTPUT_VAR is required")
  endif()

  if(FUXI_SOURCE_DIR)
    set(_source_dir "${FUXI_SOURCE_DIR}")
  else()
    set(_source_dir "${AXI_TB_FUXI_SOURCE_DIR}")
  endif()
  if(NOT _source_dir OR NOT IS_DIRECTORY "${_source_dir}")
    message(FATAL_ERROR
      "Fuxi integration requires a checkout. Set AXI_TB_FUXI_SOURCE_DIR "
      "or pass SOURCE_DIR to axi_tb_prepare_fuxi().")
  endif()
  file(REAL_PATH "${_source_dir}" _source_dir)

  if(FUXI_STAGE_DIR)
    get_filename_component(_stage_dir "${FUXI_STAGE_DIR}" ABSOLUTE
      BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
  else()
    set(_stage_dir "${CMAKE_CURRENT_BINARY_DIR}/fuxi-stage")
  endif()
  file(REAL_PATH "${CMAKE_BINARY_DIR}" _binary_dir)
  cmake_path(IS_PREFIX _binary_dir "${_stage_dir}" NORMALIZE _stage_in_build)
  if(NOT _stage_in_build)
    message(FATAL_ERROR
      "Fuxi staging directory must be below CMAKE_BINARY_DIR; got ${_stage_dir}")
  endif()
  if(_stage_dir STREQUAL _source_dir)
    message(FATAL_ERROR "Fuxi staging directory must not be the source checkout")
  endif()

  set(_required_files
    build.sbt
    project/build.properties
    project/plugins.sbt
    verilog/FuxiWrapper.v
  )
  foreach(_relative IN LISTS _required_files)
    if(NOT EXISTS "${_source_dir}/${_relative}")
      message(FATAL_ERROR "Fuxi checkout is missing ${_relative}")
    endif()
  endforeach()
  file(GLOB_RECURSE _scala_sources CONFIGURE_DEPENDS
    RELATIVE "${_source_dir}"
    "${_source_dir}/src/main/scala/*.scala"
  )
  if(NOT _scala_sources)
    message(FATAL_ERROR "Fuxi checkout has no src/main/scala sources")
  endif()

  # Recreate the private stage so stale files can never become accidental
  # elaboration inputs.  Nothing is written into the external checkout.
  file(REMOVE_RECURSE "${_stage_dir}")
  foreach(_relative IN LISTS _required_files _scala_sources)
    get_filename_component(_destination_dir
      "${_stage_dir}/${_relative}" DIRECTORY)
    file(MAKE_DIRECTORY "${_destination_dir}")
    configure_file("${_source_dir}/${_relative}"
                   "${_stage_dir}/${_relative}" COPYONLY)
  endforeach()

  set(_generated_dir "${_stage_dir}/verilog/build")
  file(MAKE_DIRECTORY "${_generated_dir}")

  set(_java_environment)
  if(AXI_TB_FUXI_JAVA_HOME)
    if(NOT IS_DIRECTORY "${AXI_TB_FUXI_JAVA_HOME}" OR
       NOT EXISTS "${AXI_TB_FUXI_JAVA_HOME}/bin/java")
      message(FATAL_ERROR
        "AXI_TB_FUXI_JAVA_HOME must contain bin/java; got "
        "'${AXI_TB_FUXI_JAVA_HOME}'")
    endif()
    set(_java "${AXI_TB_FUXI_JAVA_HOME}/bin/java")
    list(APPEND _java_environment
      "JAVA_HOME=${AXI_TB_FUXI_JAVA_HOME}"
      "PATH=${AXI_TB_FUXI_JAVA_HOME}/bin:$ENV{PATH}"
    )
  else()
    find_program(_java NAMES java)
    if(NOT _java)
      message(FATAL_ERROR
        "Fuxi elaboration requires Java 17 or newer; set "
        "AXI_TB_FUXI_JAVA_HOME or add java to PATH")
    endif()
  endif()
  execute_process(
    COMMAND "${_java}" -version
    OUTPUT_VARIABLE _java_stdout
    ERROR_VARIABLE _java_stderr
    RESULT_VARIABLE _java_result
  )
  string(CONCAT _java_version_text "${_java_stdout}" "${_java_stderr}")
  if(NOT _java_result EQUAL 0 OR
     NOT _java_version_text MATCHES "version[ \t]+\"([0-9]+)")
    message(FATAL_ERROR
      "Unable to determine the Java version used for Fuxi:\n"
      "${_java_version_text}")
  endif()
  if(CMAKE_MATCH_1 LESS 17)
    message(FATAL_ERROR
      "Fuxi requires Java 17 or newer; version output was:\n"
      "${_java_version_text}")
  endif()

  if(AXI_TB_FUXI_SBT_EXECUTABLE)
    set(_sbt "${AXI_TB_FUXI_SBT_EXECUTABLE}")
  else()
    find_program(_sbt NAMES sbt)
  endif()
  if(NOT _sbt OR NOT EXISTS "${_sbt}")
    message(FATAL_ERROR
      "Fuxi elaboration requires sbt; set "
      "AXI_TB_FUXI_SBT_EXECUTABLE or add sbt to PATH")
  endif()

  set(_fuxi_v "${_generated_dir}/Fuxi.v")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
      ${_java_environment}
      "${_sbt}"
      "runMain Fuxi --target-dir ${_generated_dir}"
    WORKING_DIRECTORY "${_stage_dir}"
    OUTPUT_VARIABLE _sbt_stdout
    ERROR_VARIABLE _sbt_stderr
    RESULT_VARIABLE _sbt_result
  )
  if(NOT _sbt_result EQUAL 0 OR NOT EXISTS "${_fuxi_v}")
    message(FATAL_ERROR
      "Fuxi elaboration failed (sbt result ${_sbt_result}).\n"
      "stdout:\n${_sbt_stdout}\n"
      "stderr:\n${_sbt_stderr}")
  endif()

  set(${FUXI_OUTPUT_VAR}
      "${_fuxi_v};${_stage_dir}/verilog/FuxiWrapper.v"
      PARENT_SCOPE)
endfunction()
