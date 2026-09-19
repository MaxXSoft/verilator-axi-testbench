# Fuxi integration

This example is deliberately opt-in: it elaborates an external Fuxi checkout
and can import an external riscv-tests checkout.  Enabling it never modifies
either checkout.  Fuxi is copied into a private build-tree stage containing
only `build.sbt`, the two `project` files, `src/main/scala/**`, and
`verilog/FuxiWrapper.v`.  The staged Fuxi sources are elaborated without
modification.

Fuxi-specific RTL is under `rtl/`, C++ peripherals are under `sim/`,
elaboration support is under `cmake/`, and
the interrupt, SFENCE, and AXI-response regression guests are under
`software/`. Generic RISC-V runtime, linker, smoke-guest, and upstream
`riscv-tests` support is shared from the sibling `../riscv-common/` example.

The integration expects:

- `AXI_TB_FUXI_SOURCE_DIR`: Fuxi checkout.
- `AXI_TB_FUXI_JAVA_HOME`: optional Java 17-or-newer home; when unset, `java`
  is discovered from `PATH`.
- `AXI_TB_FUXI_SBT_EXECUTABLE`: optional explicit `sbt` path.
- `AXI_TB_RISCV_TESTS_SOURCE_DIR`: riscv-tests checkout when
  `AXI_TB_FUXI_WITH_RISCV_TESTS=ON`; the `fuxi` preset enables it.
- `AXI_TB_RISCV_CLANG` and `AXI_TB_RISCV_LLD`: optional explicit tool paths.
  Discovery validates a real RV32 link; the macOS linker is never used.

`fuxi_sim` uses three 32-bit AXI lanes in instruction, data, uncached order.
The uncached region is `0x10000000..0x1fffffff`; UART and guest exit are at
`0x10000000` and `0x10001000`.  Guest code starts at Fuxi's reset PC `0x200`,
while data and BSS are loaded into executable RAM beginning at `0x80000000`.

The `fuxi_software` target builds four general smoke guests, three interrupt
guests, a raw-ROM/RAM-ELF boot pair, two SFENCE maintenance guests, four AXI-response fault guests, and
exactly 59 default upstream ISA guests (41 I including `fence_i`, 8 M, and
10 A).  The xRET guest uses a
software-pending supervisor interrupt and therefore does not need an external
adapter IRQ.  The fault guests check instruction/load RRESP,
uncached-store BRESP, and dirty D-cache eviction BRESP propagation to the
architectural access-fault causes.  The additional
`fuxi_software_capability_misaligned_ma_data` target is non-gating and separate;
its CTest is disabled unless `AXI_TB_FUXI_RUN_MISALIGNED_CAPABILITY=ON`.
The enabled smoke, AXI-response, and ISA tests inject deterministic 35% AXI
ingress backpressure, and a dedicated `fence_i` replay uses 80% stall to
exercise D-cache W-channel stability during dirty-line writeback.

`fuxi_protocol_mmio_store_irq_once` enables the otherwise inert
`+fuxi-mmio-store-irq` adapter hook.  It raises a one-shot soft IRQ after a
marked UART W handshake, keeps it pending through B, and verifies exact stdout
so replaying the interrupted MMIO store is detected.

The SFENCE maintenance regressions use an `rs1` value inside the uncached
window.  One checks clean-cache completion; the other dirties a ROM line and
requires the flush BRESP failure to emerge as store-access-fault cause 7.

## Simulator platform and interrupts

`sim/fuxi_platform.cpp` registers CLINT and PLIC through the public C++ registry,
starts with the default ROM/RAM/UART/Exit platform, and adds the interrupt
connections. It needs no Fuxi-specific code under `src/`. The compile-time
sideband binding is in `sim/fuxi_platform.hpp`; the SV adapter only wires
signals. See [the extension API](../../docs/platform.md) for other platforms.

| Device | Base | Aperture | Connection |
|---|---:|---:|---|
| ROM | `0x00000000` | `0x10000` | Reset PC `0x200` |
| RAM | `0x80000000` | `0x08000000` | Shared by all AXI lanes |
| ns16550a UART | `0x10000000` | `0x100` | PLIC source 10 |
| Exit | `0x10001000` | `4` | B-handshake completion |
| CLINT | `0x11000000` | `0x10000` | MSIP, MTIP, 64-bit mtime |
| PLIC | `0x12000000` | `0x04000000` | Context 0 M, context 1 S |

CLINT and PLIC are deliberately relocated into the existing core's uncached
window. Their register offsets follow the conventional CLINT and standard
PLIC layouts; they do not use the FPGA SoC's AXI INTC programming model.
`generated/fuxi_platform.h` exports the CMake-selected peripheral bases and
UART IRQ to both the simulator platform and the assembly guests.

CLINT implements one hart: `msip` at `+0`, `mtimecmp` at `+0x4000`, and `mtime`
at `+0xbff8`. Timer registers support aligned 32-bit low/high accesses and
64-bit accesses. `msip` uses bit 0. The timer runs once per active CPU cycle
by default and pauses during reset; reset sets `mtime=0`, `msip=0`, and
`mtimecmp=UINT64_MAX`. Use `--set clint.divider=N` for one tick per N cycles.
The nominal guest timebase is 50 MHz with divider 1 (`FUXI_TIMEBASE_HZ`);
software must adjust its timebase if the divider changes. This is simulated
time, independent of host wall-clock speed.

PLIC has 31 external level-sensitive sources by default, priorities 0..7,
and two contexts. `--set plic.sources=N` supports up to 1023; the reusable
model supports 1..64 contexts, while this adapter requires at least two.
Source 0 is reserved. Priorities are at `+4*ID`, pending bits at `+0x1000`,
context enables at `+0x2000 + context*0x80`, and threshold/claim-complete at
`+0x200000 + context*0x1000` and `+4`. Registers require aligned 32-bit
accesses. Priority zero masks a source; ties favor the lower source ID.
Threshold gates notification, not claim. Claim clears global pending and
completion releases the gateway, retriggering if the source remains high.
A completion is ignored if its ID is invalid or not enabled for that context.
Reserved PLIC registers read zero and ignore writes. Gateways retain pending
requests when an input falls, and allow only one outstanding request per source.
These rules follow the [RISC-V PLIC specification](https://github.com/riscv/riscv-plic-spec/blob/master/riscv-plic.adoc).

The UART uses the QEMU virt convention: contiguous byte registers and IRQ
source 10. Its `irq` output is optional outside this platform. It implements
receive/FIFO-timeout, THRE, overrun and modem-status interrupts, including
loopback for driver probing. Baud-accurate serial timing is not modeled.

## Scope of OS support

The peripherals now supply the console, local timer/software interrupt and
external interrupt controller needed for GeeOS and an initramfs-based Linux
bring-up. OS boot itself is not validated here. A GeeOS `fuxi_sim` target
should use the map above; the existing FPGA `fuxi` target can keep AXI INTC.
Device-tree generation, SBI firmware, storage/virtio, and processor privilege,
MMU and CSR changes remain separate work.

The current Fuxi core has a single external IRQ input, which it reflects in
both M/S pending bits. The adapter ORs the separate PLIC M/S outputs into that
legacy input. The new regression checks both contexts using the appropriate
interrupt enables/delegation; this does not establish independent M/S pending
semantics when both contexts are active. A later core upgrade can wire the
already separate `irq_meip` and `irq_seip` inputs independently. Likewise,
`rtc_time` carries CLINT mtime to the adapter boundary but is unused inside
the current core: it does not implement `time/timeh` CSR behavior by itself.

## Added regressions and image loading

`fuxi_peripheral_irq` exercises real CLINT MSIP and MTIP interrupts, followed
by UART loopback receive through PLIC context 0 into M-mode and context 1 into
S-mode, with 35% AXI backpressure. Host tests additionally cover timer carry,
64-bit MMIO, PLIC arbitration, masking, gateway completion, pending retention,
invalid accesses, reset, and connections independent of device creation order.

`fuxi_mixed_rom_elf` boots a raw ROM stub at `0x200`, jumps into an independently
loaded RAM-only ELF, and checks ROM bytes, ELF data and zeroed BSS:

```sh
build/fuxi/examples/fuxi/fuxi_sim \
  --rom-image build/fuxi/examples/fuxi/software/mixed_boot.bin \
  --elf build/fuxi/examples/fuxi/software/mixed_payload.elf
```

Repeat `--elf` or `--load ADDRESS=FILE` for further images. All image ranges
must be disjoint, including ELF BSS; the loader rejects overlap before changing
memory. Loading an ELF does not alter Fuxi's reset PC.
