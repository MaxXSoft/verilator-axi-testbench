# Reusable AXI4 Testbench for Verilator

This repository provides a reusable simulation environment for AXI initiator
DUTs. DUT-specific integration and minimal signal renaming stay in
SystemVerilog; the AXI fabric, protocol checks, ROM/RAM/UART/Exit devices, ELF
loading, and simulation loop are implemented in C++20. The build requires
Verilator 5 or newer.

The current implementation assumes little-endian operation, a single clock
domain, and uniform address, data, and ID widths across all ports. It supports
32- or 64-bit addresses, 32-, 64-, or 128-bit data, 1- to 32-bit IDs, the AXI4
AW/AR `ID/ADDR/LEN/SIZE/BURST/LOCK/CACHE/PROT` fields, and complete W/R/B
channels. It does not support AXI3 write-data interleaving, width conversion,
`USER/QOS/REGION`, ACE, or AXI5 ATOP.

## Architecture

```text
initiator DUT
    │
    ├─ thin SystemVerilog adapter (packed canonical ports)
    │
    └─ VerilatedAxiBinding
          │
          ├─ per-port AW/W/AR/R/B ring buffers
          ├─ AW/AR round-robin + ordered write/read routes
          └─ AddressSpace
                ├─ ROM
                ├─ shared RAM
                ├─ 16550-compatible UART subset
                └─ 32-bit Exit register
```

The port count and widths of `AxiFabric` are compile-time parameters.
Each ingress buffers all five channels independently, so W may arrive before
AW. AW and AR use independent round-robin arbitration across ports, while each
internal route retains both the port number and AXI ID. The fabric processes at
most one read beat and one write beat per cycle. It allows multiple outstanding
transactions, but does not intentionally complete them out of order or
interleave data from two read bursts.

Burst address calculation supports FIXED, INCR, and WRAP bursts; narrow
transfers; unaligned first beats; WSTRB; WLAST; up to 256 beats; and the 4 KiB
boundary rule. Transactions that violate AXI shape rules or VALID stability
under backpressure raise protocol errors with the cycle, port, channel, and
available ID/address context. Device writes begin only after every beat has
passed WLAST/WSTRB validation. Consequently, a late validation error or a
reset before WLAST leaves no partial write behind. Unmapped accesses return
DECERR; writes to ROM and unsupported but otherwise legal device accesses
return SLVERR. RAM implements exclusive reservations keyed by `{port,id}`.

ROM becomes read-only after its image is loaded. RAM uses lazy anonymous
mappings on macOS and Linux when available, with a standard container fallback
elsewhere. Every AXI port shares the same RAM object. The UART implements
RBR/THR, the LSR DR/THRE/TEMT bits, and the common DLAB, DLL/DLM, IER, LCR,
MCR, and SCR initialization registers. It does not model exact baud timing,
interrupts, or complete modem/FIFO behavior. Reset clears fabric, arbitration,
response, and exclusive state without clearing ROM or RAM contents. Each
per-beat device access uses a fixed function table instead of C++ virtual
dispatch, and UART RX uses a fixed 16-byte ring; the default simulation hot
path performs no memory allocation.

## Canonical SystemVerilog Adapter

`AXI_TB_INITIATOR_PORTS` in
[`rtl/axi_tb_ports.svh`](rtl/axi_tb_ports.svh) is the stable boundary
consumed by the C++ binding. The adapter top level must declare the
`NUM_AXI`, `ADDR_WIDTH`, `DATA_WIDTH`, and
`ID_WIDTH` parameters and expand that macro directly:

```systemverilog
`include "axi_tb_ports.svh"

module my_axi_adapter #(
  parameter int unsigned NUM_AXI    = 2,
  parameter int unsigned ADDR_WIDTH = 64,
  parameter int unsigned DATA_WIDTH = 64,
  parameter int unsigned ID_WIDTH   = 8
) (
  `AXI_TB_INITIATOR_PORTS
);
  // Instantiate the DUT here and connect its initiator ports to the axi_*
  // packed arrays. axi_*[0] is ingress 0, and so on. clk and active-low
  // aresetn also come from this boundary.
endmodule
```

The macro declares every initiator-facing `axi_aw_*`,
`axi_w_*`, `axi_b_*`, `axi_ar_*`, and
`axi_r_*` signal. Do not flatten or rename these Verilator-visible
signals outside the adapter.
[`rtl/axi_tb_canonical_top.sv`](rtl/axi_tb_canonical_top.sv) is a
minimal idle structural example, while
[`examples/fuxi/fuxi_axi_adapter.sv`](examples/fuxi/fuxi_axi_adapter.sv)
shows a real mapping from three named interfaces to the canonical arrays.

## CMake Integration

The simplest integration is to add this repository as a subdirectory. This
creates `axi_tb_core` and loads `cmake/AxiTestbench.cmake`:

```cmake
set(AXI_TB_BUILD_TESTS OFF CACHE BOOL "")
add_subdirectory(path/to/verilator-axi-testbench axi-testbench)
```

Then use `add_axi_testbench()` to generate a separate configuration,
Verilated model, and simulator executable for each DUT. The commonly used
options are shown below:

```cmake
add_axi_testbench(
  TARGET       my_core_sim
  TOP          my_axi_adapter
  RTL_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/my_axi_adapter.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/my_core.sv

  NUM_AXI      2
  ADDR_WIDTH   64
  DATA_WIDTH   64
  ID_WIDTH     8

  ROM_BASE     0x00000000
  ROM_SIZE     0x00010000
  RAM_BASE     0x80000000
  RAM_SIZE     0x08000000
  UART_BASE    0x10000000
  UART_SIZE    0x00000100
  EXIT_BASE    0x10001000
  EXIT_SIZE    0x00000004

  # Optional: compile VCD tracing for this target only.
  TRACE
  # Optional multi-value arguments:
  # CXX_SOURCES     ${CMAKE_CURRENT_SOURCE_DIR}/extra_support.cpp
  # INCLUDE_DIRS    ${CMAKE_CURRENT_SOURCE_DIR}/rtl/include
  # VERILATOR_ARGS  --timescale 1ns/1ps
)
```

`TARGET`, `TOP`, and `RTL_SOURCES` are required; all
other values have defaults. `EXIT_ADDRESS` is accepted as an alias for
`EXIT_BASE`. Each call generates the following beneath the current
binary directory:

- `axi_tb_generated/<target>/config.hpp`: C++ configuration.
- `axi_tb_generated/<target>/axi_tb_env.h`: guest software header.
- `axi_tb_generated/<target>/link.ld`: linker script using the same
  ROM/RAM addresses.
- `verilated/<target>/`: target-specific Verilator output.

All of these files come from one set of CMake parameters, so guest and host
code do not need separate copies of the address constants. During
configuration, every window is checked across the full `uint64_t` range
for overflow, the 32-bit address-width limit, and overlap with every other
device window. An invalid address map never reaches Verilation.

### Default Address Map

| Device | Base | Size |
|---|---:|---:|
| ROM | `0x00000000` | `0x00010000` (64 KiB) |
| RAM | `0x80000000` | `0x08000000` (128 MiB) |
| UART | `0x10000000` | `0x00000100` |
| Exit | `0x10001000` | `0x00000004` |

Exit accepts only aligned, full-strobe, single-beat 32-bit writes. When the
guest writes `0`, the host returns 0. For a nonzero value, the host
prints the complete guest code and returns 1.

## Simulator Command Line

```text
my_core_sim [options]

  --elf FILE                 Load PT_LOAD segments from a little-endian ELF32/ELF64
  --rom-image FILE           Load a raw image at the ROM base
  --ram-image FILE           Load a raw image at the RAM base
  --max-cycles N             Maximum active cycles after reset; default 10000000
  --reset-cycles N           Reset rising edges; default 5
  --uart-in FILE|-           UART input; default stdin, and - also means stdin
  --uart-out FILE|-          UART output; default stdout, and - also means stdout
  --seed N                   Fabric random-stall seed; default 1
  --stall-probability P      AW/W/AR READY stall probability in [0,1]; default 0
  --trace FILE               Write VCD; tracing must have been compiled in
  +NAME[=VALUE]              Pass a plusarg through to the Verilated RTL unchanged
```

`--elf` cannot be combined with either raw-image option. The ELF loader
validates every PT_LOAD segment before writing any data to ROM or RAM, then
zeroes each `p_memsz - p_filesz` region. Exit statuses are: guest PASS
`0`, nonzero guest exit `1`, configuration or image error
`2`, AXI protocol error (or premature DUT `$finish`)
`3`, timeout `124`, and SIGINT `130`.

## Building and Testing

Using the presets requires CMake 3.25 or newer, a C++20 compiler, and Verilator
5 or newer:

```sh
cmake --preset default
cmake --build --preset default -j
ctest --preset default
```

Run only the self-contained core or protocol groups with:

```sh
ctest --preset core
ctest --preset protocol
```

The fixed-seed, dual-port differential stress test runs 100,000 cycles of
random traffic and then drains the fabric. It covers different AxSIZE values,
INCR bursts, unaligned first beats, partial WSTRB, either AW/W arrival order,
and request/response backpressure, comparing RAM against a golden model byte
by byte:

```sh
cmake --build build/default --target axi_tb_host_fabric_stress -j
ctest --test-dir build/default -R '^core\.host_fabric_stress$' --output-on-failure
```

Artifacts from these presets, host tests, Verilator, and Fuxi staging all
remain under the repository's `build/` directory. Normal builds and
tests do not need to run executables from `/private/tmp`.

## Optional Fuxi Preset

The default build does not access the Fuxi or riscv-tests submodules. The
`fuxi` preset uses the in-tree checkouts at
`third_party/Fuxi` and `third_party/riscv-tests`. Initialize
them after cloning:

```sh
git submodule update --init
```

The preset enables both the Fuxi integration and the RV32I/M/A software suite:

```sh
cmake --preset fuxi
cmake --build --preset fuxi -j2
ctest --preset fuxi
```

This configuration uses Fuxi's Chisel 7.13.0 and Scala 2.13.18 build. It
requires JDK 17 or newer, sbt, a Clang that supports
`rv32ima_zicsr_zifencei/ilp32`, and `ld.lld`. The following
cache variables can override automatic tool discovery or the default submodule
paths:

- `AXI_TB_FUXI_SOURCE_DIR`
- `AXI_TB_FUXI_JAVA_HOME`
- `AXI_TB_FUXI_SBT_EXECUTABLE`
- `AXI_TB_RISCV_TESTS_SOURCE_DIR`
- `AXI_TB_RISCV_CLANG`
- `AXI_TB_RISCV_LLD`

During configuration, only the files required by Fuxi are copied into a build
staging directory. Fuxi's sbt build then generates `Fuxi.v` inside that stage.
Neither submodule checkout is modified, and
the staged Fuxi sources are not rewritten. Fuxi's three ports are mapped in
instruction/data/uncached order, its timer and external IRQs are tied low, and
its soft IRQ is driven only by the opt-in MMIO test hook. The integration
asserts that the legacy AXI3 `WID` remains 0.

The optional tests include UART/Exit smoke tests, an xRET/pending-IRQ
regression, four AXI-response access-fault regressions, and 59 RV32I/M/A guests
compiled directly from the upstream assembly. The xRET regression uses a
software-set supervisor pending interrupt and does not depend on an external
adapter IRQ. The MMIO interrupt regression uses an otherwise disabled Fuxi
adapter plusarg hook to raise a soft IRQ once after the UART W handshake but
before the BRESP completes, then verifies that the character is written
exactly once.

The SFENCE regressions use an `rs1` value in the uncached window. They
check both successful completion with a clean D-cache and, when a dirty ROM
line fails to write back, that cause 7 is not hidden by demand-address routing.
The response regressions cover RRESP for instruction fetches and cacheable
loads, BRESP for uncached stores and dirty D-cache evictions, and verify the
exception cause reported by Fuxi. `ma_data` is a separate,
misaligned-access capability test and is not part of the default 59-test gate.
It records whether the DUT chooses to trap or complete an unaligned access in
hardware. Whether these integration tests pass depends on the pinned submodule
revisions, the local toolchain, and the DUT; this README does not claim they
have been validated in every environment.

The Fuxi smoke, AXI-response, and 59 ISA regressions use deterministic 35% AXI
ingress backpressure. A separate `fence_i` regression uses an 80% stall
rate during D-cache writeback to check that WVALID and its payload remain
stable while WREADY is low.

After configuration succeeds, tests can be selected by CTest label:

```sh
ctest --preset fuxi -L fuxi
ctest --preset fuxi -L rv32i
ctest --preset fuxi -L rv32m
ctest --preset fuxi -L rv32a
```

To include Fuxi's `ma_data` capability case, which Fuxi expects to
handle with a trap, reconfigure with
`-DAXI_TB_FUXI_RUN_MISALIGNED_CAPABILITY=ON`. It can then be selected
separately with `-L capability`. This option is disabled by default.
