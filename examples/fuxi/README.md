# Fuxi integration

This example is deliberately opt-in: it elaborates an external Fuxi checkout
and can import an external riscv-tests checkout.  Enabling it never modifies
either checkout.  Fuxi is copied into a private build-tree stage containing
only `build.sbt`, the two `project` files, `src/main/scala/**`, and
`verilog/FuxiWrapper.v`.  The stage also latches the pre-write AMO value to
preserve the original FPGA read-first BRAM semantics under current Verilator;
the compatibility rewrite is guarded by exact source-shape checks.

The integration expects:

- `AXI_TB_FUXI_SOURCE_DIR`: Fuxi checkout.
- `AXI_TB_FUXI_JAVA_HOME`: Java 11 home (defaults to Homebrew
  `/opt/homebrew/opt/openjdk@11`).
- `AXI_TB_FUXI_SBT_EXECUTABLE`: optional explicit `sbt` path.
- `AXI_TB_RISCV_TESTS_SOURCE_DIR`: riscv-tests checkout when
  `AXI_TB_FUXI_WITH_RISCV_TESTS=ON`; the `fuxi` preset enables it.
- `AXI_TB_RISCV_CLANG` and `AXI_TB_RISCV_LLD`: optional explicit tool paths.
  Discovery validates a real RV32 link; the macOS linker is never used.

`fuxi_sim` uses three 32-bit AXI lanes in instruction, data, uncached order.
The uncached region is `0x10000000..0x1fffffff`; UART and guest exit are at
`0x10000000` and `0x10001000`.  Guest code starts at Fuxi's reset PC `0x200`,
while data and BSS are loaded into executable RAM beginning at `0x80000000`.

The `fuxi_software` target builds four smoke guests and exactly 59 default
upstream ISA guests (41 I including `fence_i`, 8 M, and 10 A).  The additional
`fuxi_software_capability_misaligned_ma_data` target is non-gating and separate;
its CTest is disabled unless `AXI_TB_FUXI_RUN_MISALIGNED_CAPABILITY=ON`.
The enabled smoke/ISA tests inject deterministic 35% AXI ingress backpressure,
and a dedicated `fence_i` replay uses 80% stall to exercise D-cache W-channel
stability during dirty-line writeback.
