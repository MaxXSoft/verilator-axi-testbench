# Fuxi benchmark guests

The pinned submodules are upstream [SiFive Dhrystone 2.1](https://github.com/sifive/benchmark-dhrystone)
and [EEMBC CoreMark](https://github.com/eembc/coremark). Their measured workloads
are unchanged. Initialize them with `git submodule update --init` and configure:

```sh
cmake --preset fuxi -DAXI_TB_FUXI_WITH_BENCHMARKS=ON
cmake --build build/fuxi --target fuxi_sim fuxi_benchmarks -j
python3 examples/fuxi/benchmarks/run.py \
  --simulator build/fuxi/examples/fuxi/fuxi_sim \
  --elf build/fuxi/examples/fuxi/benchmarks/dhrystone-rv32ima.elf \
  --output build/fuxi/examples/fuxi/benchmarks/results/dhrystone-rv32ima.json
```

Replace the ELF name with `coremark-rv32ima.elf`, `dhrystone-rv32imac.elf`, or
`coremark-rv32imac.elf`. The preset's pinned Fuxi supports both ISA variants;
an external checkout selected with `AXI_TB_FUXI_SOURCE_DIR` must also support
C to run the IMAC images. Benchmark tool discovery validates an IMAC link
including a compressed instruction, even when the upstream ISA suite is off.
The benchmark suite is opt-in to keep its extra dependencies and runtime out
of the default Fuxi build. CI enables it explicitly. Both ISA variants remain
available for controlled comparisons.

CTest label filters are regular expressions, so use anchors to select one
ISA variant:

```sh
ctest --preset fuxi -L '^benchmark$'
ctest --preset fuxi -L '^rv32ima$'
ctest --preset fuxi -L '^rv32imac$'
```

Without the optional fixed-layout variants below, these select all four
images, the two IMA images, and the two IMAC images, respectively. The `rv32c`
label selects the upstream compressed-instruction regression, which is
included in the default Fuxi ISA suite independently of benchmarks.

Both ISA variants use the same Clang and `-O3 -ffreestanding -fno-builtin
-fno-common -fno-pic -falign-functions=4 -ffunction-sections -fdata-sections`,
ILP32 ABI, and no LTO or linker relaxation. Only `-march` differs. Dhrystone
uses `NOENUM`, its normal two translation units, and portable bytewise C
`strcmp`, `strcpy`, and `memcpy`; the optional SiFive assembly strcmp is not
used. CMake stages Dhrystone's main file and removes only its floating-point
score printer; all final-value output and the measured loop are preserved.
The source-layout check fails if that adaptation cannot be applied. Its TIME
hooks collect CSR counters instead of calendar seconds. CoreMark uses its
unmodified algorithm and main files, volatile
performance seeds `0,0,0x66`, a 2000-byte static RAM buffer, and one context.
Code and constants execute in ROM, mutable data and stack are in RAM. Startup
relies on the simulator ELF loader to initialize data and zero BSS.

The default 2000 Dhrystone and 10 CoreMark iterations keep RTL regression runs
short. Set `AXI_TB_DHRYSTONE_ITERATIONS` and `AXI_TB_COREMARK_ITERATIONS` at
configure time to change them. These are normalized simulation estimates,
not official benchmark submissions. In particular the upstream CoreMark
ten-second validity warning is retained. The runner independently requires
the standard performance seed/list/matrix/state CRCs, rejects other upstream
errors, and checks Dhrystone's reported local results as well as its guest-side
global/record validation. A simulator exit status alone is not a passing
benchmark.

`cycle[h]` and `instret[h]` are sampled at the upstream timing hooks with
high/low/high rollover protection. Their deltas include the small, fixed
counter-sampling overhead and exclude initialization, result validation,
printing, and guest exit. The counters are sampled sequentially, not
atomically; use enough iterations to make that overhead negligible. No
counter overhead is estimated or subtracted. The CoreMark upstream tick
interface deliberately rejects runs of 2^32 cycles or longer instead of
silently wrapping. Guest UART prints exact hexadecimal 64-bit deltas. The
runner prints decimal instruction/cycle counts, IPC, cycles/iteration,
iterations/sec at the platform's nominal 50 MHz, DMIPS/MHz (Dhrystone), and
iterations/cycle times 1e6 (CoreMark/MHz estimate). Host wall time is reported
separately and does not enter these scores.

The JSON records simulator and ELF SHA-256 digests, exact invocation, AXI stall
probability/seed, counters, and validation status. The adjacent `.log` preserves
all guest and simulator output.

The runner enables simulator `--stats` to collect accepted AR requests,
requested burst bytes and completed R beats per AXI port. In this Fuxi adapter,
port 0 is instruction traffic, port 1 is data and port 2 is uncached. These
traffic totals span the **entire run**, including startup, printing, and
validation; they do not share the benchmark CSR measurement interval. They
measure off-core traffic, not cache hit counts or frontend empty cycles.
`SIM_STATS active_cycles=N` reports full-run active cycles. With `--stats`,
SIGTERM is handled like SIGINT so interactive harnesses can collect final
statistics while stopping a guest; the process exits with `128 + signal`.

For a separate layout-controlled comparison, configure with
`AXI_TB_FUXI_BENCHMARK_FIXED_LAYOUT=ON`. This adds four `*-fixed-rodata.elf`
variants, leaving the natural-layout images unchanged. Both IMA and IMAC
place `.rodata` at ROM address `0x5000`; linker assertions reject text that
reaches that address or a changed placement. Mutable data and stack addresses
remain identical. This prevents code compression from moving constants into
different direct-mapped D-cache sets. It does not eliminate changed branch
predictor aliasing or the intended I-cache/code-layout effects. Keep these
results separate from the natural-layout primary comparison.
