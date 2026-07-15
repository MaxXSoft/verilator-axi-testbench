# Reusable RISC-V example support

This directory contains the parts of the RISC-V example flow that are not tied
to a particular DUT:

- `cmake/RiscvTests.cmake` discovers and validates Clang plus ELF LLD, builds
  the common guests, and optionally builds the pinned 59-test RV32 I/M/A
  manifest from an upstream `riscv-tests` checkout.
- `software/` provides the reset runtime, platform headers, linker script,
  basic pass/fail/timeout/UART guests, and the CMake result checker.

A DUT example can include `cmake/RiscvTests.cmake`, call
`axi_tb_add_riscv_software()`, and optionally pass `REGRESSION_DIR` to add the
named protocol regression guests supplied by that DUT. The Fuxi example is the
reference consumer.
