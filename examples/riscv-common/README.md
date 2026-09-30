# Reusable RISC-V example support

This directory contains the parts of the RISC-V example flow that are not tied
to a particular DUT:

- `cmake/RiscvTests.cmake` discovers and validates Clang plus ELF LLD, builds
  the common guests, and optionally builds the pinned 59-test RV32 I/M/A
  manifest plus an RV32C guest from an upstream `riscv-tests` checkout.
- `software/` provides the reset runtime, platform headers, linker script,
  basic pass/fail/timeout/UART guests, and the CMake result checker.

A DUT example can include `cmake/RiscvTests.cmake`, call
`axi_tb_add_riscv_software()`, and optionally pass `REGRESSION_DIR` to add the
named protocol regression guests supplied by that DUT. The Fuxi example is the
reference consumer.

`WITH_RISCV_TESTS` builds the 59 I/M/A guests and makes the separate `ma_data`
capability target available. Adding `WITH_RVC_TESTS` builds `rv32uc/rvc.S`
with `rv32imac_zicsr_zifencei`; it requires `WITH_RISCV_TESTS`. Fuxi passes both
arguments by default. Other DUTs can select the I/M/A suite alone.
`OUT_RISCV_NAMES` and `OUT_RISCV_ELFS` return only the 59 I/M/A entries;
`OUT_RVC` returns the compressed guest separately.

Tool discovery validates an IMA link, or an IMAC link containing `c.nop` when
`WITH_RVC_TESTS` is present. Both use the ILP32 ABI, Zicsr, and Zifencei.
Common smoke and DUT regression guests retain the IMA compilation target;
enabling the C guest does not change existing assembly layouts. The internal
image helper accepts `MARCH` to select another ISA for an individual guest.

Additional example-specific guests can be passed through `REGRESSION_SOURCES`,
with extra `INCLUDE_DIRS` and explicit `DEPENDS` for generated platform headers.
The internal image helper also accepts `BINARY` for raw boot-ROM output.
