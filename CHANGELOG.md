# Changelog

All notable changes to the verilator-axi-testbench will be documented in this file.

## Unreleased

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
