# Changelog

All notable changes to the verilator-axi-testbench will be documented in this file.

## Unreleased

### Added

- Pinned Dhrystone and CoreMark RV32IMA/RV32IMAC Fuxi benchmark guests.
- Validated benchmark reports with workload counters and reproducibility metadata.
- Upstream RV32C simulator regression.
- Full-run per-port AXI read traffic statistics via `--stats`.
- Fixed-layout benchmark variants for controlled ISA comparisons.
- Opt-in frozen Fuxi PMU benchmark measurements with grouped event selectors,
  guest counter snapshots, derived metrics, and parser regressions.
- Fuxi peripheral regression coverage for independent M/S interrupt pending
  bits, CLINT-backed `time`/`timeh`, and UART IIR-before-RHR reads.

### Changed

- Update Fuxi for RV32IMAC, grouped PMU events, and Linux bring-up support;
  refresh riscv-tests.
- Run Fuxi RV32C, baseline benchmark, and PMU suites in CI.
- Limit CI triggers to pushes to `master` and pull requests targeting `master`.
- Connect PLIC M/S external interrupts and CLINT time to separate Fuxi wrapper
  inputs; external Fuxi checkouts must provide `irq_sextern` and `rtc_time`.
- Widen the Fuxi exit aperture to eight bytes for OpenSBI domain regions while
  retaining the single 32-bit exit register.
- Generate benchmark group names and IDs from the shared PMU definitions.
- Include RV32C in the default 60-test Fuxi ISA suite and remove its separate option.
- Validate compressed-instruction toolchain support for RVC tests and IMAC benchmarks.
- Refresh Fuxi benchmark and RV32C documentation.

### Fixed

- Update Fuxi to refetch instructions after SATP writes and preserve MMIO access
  widths so UART interrupt status reads do not consume received data.

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
