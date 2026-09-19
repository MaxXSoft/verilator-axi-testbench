# The host platform is independent of RTL elaboration. Building it for the
# self-contained tests also gives clang-tidy complete commands for every TU.
set(_fuxi_clint_base 0x11000000)
set(_fuxi_plic_base 0x12000000)
configure_file(fuxi_platform.h.in fuxi_platform.h @ONLY)

add_library(axi_tb_fuxi_platform OBJECT fuxi_platform.cpp interrupt_devices.cpp)
target_link_libraries(axi_tb_fuxi_platform PUBLIC axi_tb::core)
target_include_directories(axi_tb_fuxi_platform PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
set_target_properties(axi_tb_fuxi_platform PROPERTIES
  AXI_TB_PLATFORM_HEADER "${CMAKE_CURRENT_BINARY_DIR}/fuxi_platform.h")
