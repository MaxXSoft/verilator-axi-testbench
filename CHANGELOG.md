# Changelog

All notable changes to the verilator-axi-testbench will be documented in this file.

## Unreleased

### Added

- Pinned Dhrystone and CoreMark RV32IMA/RV32IMAC Fuxi benchmark guests.
- Validated benchmark reports with workload counters and reproducibility metadata.
- Opt-in upstream RV32C simulator regression.
- Full-run per-port AXI read traffic statistics via `--stats`.
- Fixed-layout benchmark variants for controlled ISA comparisons.

### Changed

- Update Fuxi for RV32IMAC support and refresh riscv-tests.
- Run Fuxi RV32C and benchmark suites in CI.
- Refresh Fuxi benchmark and RV32C documentation.

## 0.0.2 - 2026-09-25

### Added

- Static device registries and configurable platform specifications.
- Optional 16550 UART interrupt sidebands.
- Fuxi CLINT and PLIC platform with interrupt and mixed-boot regressions.
- Sv32 fetch-redirection and divider-reuse-across-interrupts regressions.
- Standalone Fuxi platform host test.
- Configurable UART `input-poll-cycles` option to back off empty input polls.

### Changed

- Update Fuxi and riscv-tests submodules.
- Refresh README and platform docs for GeeOS boot and smoke-test paths.

### Fixed

- Keep address-space lookup results immutable during simulation.
- Avoid expensive Darwin poll probes for interactive UART input.
- Resolve LLVM 23 clang-tidy diagnostics.

## 0.0.1 - 2026-07-16
