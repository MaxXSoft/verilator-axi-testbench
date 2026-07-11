# Verilator 通用 AXI4 Testbench

这是一个面向“AXI initiator DUT”的可复用仿真环境。DUT 和少量信号改名留在
SystemVerilog；AXI fabric、协议检查、ROM/RAM/UART/Exit 设备、ELF 加载和仿真
循环均使用 C++20 实现。构建系统要求 Verilator 5 或更新版本。

当前实现以 little-endian、单时钟域、各端口同宽为边界。支持 32/64-bit 地址、
32/64/128-bit 数据、1--32-bit ID，以及 AXI4 AW/AR 的
`ID/ADDR/LEN/SIZE/BURST/LOCK/CACHE/PROT` 和完整 W/R/B 通道。不包含 AXI3
write-data interleaving、宽度转换、`USER/QOS/REGION`、ACE 或 AXI5 ATOP。

## 架构

```text
initiator DUT
    │
    ├─ thin SystemVerilog adapter（packed canonical ports）
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

`AxiFabric` 的端口数和宽度都是编译期参数。每个入口分别缓冲五个通道，W 可以
先于 AW 到达；多端口的 AW 和 AR 独立 round-robin 仲裁，内部路由同时保留端口号
和 AXI ID。fabric 每周期至多处理一个 read beat 和一个 write beat，允许多个
outstanding transaction，但不会主动乱序完成或交织两个 read burst 的数据。

burst 地址计算覆盖 FIXED、INCR、WRAP、窄传输、非对齐首拍、WSTRB、WLAST、
最多 256 beats 和 4 KiB 边界。违反 AXI 形状或 VALID 背压稳定性的事务会抛出
带周期、端口、通道以及可用的 ID/地址信息的协议错误。write burst 会在完整
校验 WLAST/WSTRB 后再提交，因此后拍错误或中途 reset 不会留下部分写入。未映射
访问返回 DECERR；ROM 写和设备不支持的合法访问返回 SLVERR。RAM 支持以
`{port,id}` 为键的 exclusive reservation。

ROM 在镜像加载后只读；RAM 在 macOS/Linux 优先使用 lazy anonymous mapping，
其他平台回退到标准容器。所有 AXI 端口共享同一个 RAM 对象。UART 实现
RBR/THR、LSR 的 DR/THRE/TEMT，以及常见的 DLAB、DLL/DLM、IER、LCR、MCR、
SCR 初始化寄存器；它不模拟精确波特率、中断或完整 modem/FIFO 时序。reset
清空 fabric、仲裁、响应和 exclusive 状态，但不清除 ROM/RAM 内容。
每拍设备访问使用固定函数表而非 C++ virtual dispatch，UART RX 使用固定 16-byte
ring；默认仿真热路径不分配内存。

## Canonical SystemVerilog adapter

[`rtl/axi_tb_ports.svh`](rtl/axi_tb_ports.svh) 中的
`AXI_TB_INITIATOR_PORTS` 是 C++ binding 依赖的稳定边界。adapter 顶层必须具有
参数 `NUM_AXI`、`ADDR_WIDTH`、`DATA_WIDTH`、`ID_WIDTH`，并直接展开该宏：

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
  // 在这里实例化 DUT，并将它的 initiator 端口逐项连接到 axi_* packed arrays。
  // axi_*[0] 是第 0 个入口，依此类推；clk 和低有效 aresetn 也来自该边界。
endmodule
```

宏声明了 initiator 方向的所有 `axi_aw_*`、`axi_w_*`、`axi_b_*`、`axi_ar_*`、
`axi_r_*` 信号。不要在 adapter 外再展平或重命名这些 Verilator-visible 信号。
[`rtl/axi_tb_canonical_top.sv`](rtl/axi_tb_canonical_top.sv) 是保持空闲的最小结构
示例；[`examples/fuxi/fuxi_axi_adapter.sv`](examples/fuxi/fuxi_axi_adapter.sv) 展示了
三组具名接口到 canonical arrays 的实际映射。

## CMake 接入

最简单的接入方式是把本仓库作为子目录加入工程；它会创建 `axi_tb_core` 并载入
`cmake/AxiTestbench.cmake`：

```cmake
set(AXI_TB_BUILD_TESTS OFF CACHE BOOL "")
add_subdirectory(path/to/verilator-axi-testbench axi-testbench)
```

随后可用 `add_axi_testbench()` 为每个 DUT 生成独立的配置、Verilated model 和
仿真可执行文件。下面列出全部常用配置项：

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

  # 可选：仅对这个目标编译 VCD tracing。
  TRACE
  # 可选的多值参数：
  # CXX_SOURCES     ${CMAKE_CURRENT_SOURCE_DIR}/extra_support.cpp
  # INCLUDE_DIRS    ${CMAKE_CURRENT_SOURCE_DIR}/rtl/include
  # VERILATOR_ARGS  --timescale 1ns/1ps
)
```

`TARGET`、`TOP`、`RTL_SOURCES` 必填，其余值均有默认值。`EXIT_ADDRESS` 可作为
`EXIT_BASE` 的别名。每次调用会在当前 binary directory 下生成：

- `axi_tb_generated/<target>/config.hpp`：C++ 配置；
- `axi_tb_generated/<target>/axi_tb_env.h`：guest 软件头；
- `axi_tb_generated/<target>/link.ld`：使用同一 ROM/RAM 地址的 linker script；
- `verilated/<target>/`：该目标独立的 Verilator 输出。

这些文件来自同一组 CMake 参数，guest 和 host 不需要各维护一份地址常量。
配置阶段会以完整 `uint64_t` 范围检查每个窗口的溢出、32-bit 地址宽度上限和任意
两个设备窗口的重叠；错误地址图不会进入 Verilation。

### 默认地址图

| 设备 | Base | Size |
|---|---:|---:|
| ROM | `0x00000000` | `0x00010000`（64 KiB） |
| RAM | `0x80000000` | `0x08000000`（128 MiB） |
| UART | `0x10000000` | `0x00000100` |
| Exit | `0x10001000` | `0x00000004` |

Exit 只接受对齐、完整 strobe 的 32-bit 单拍写。guest 写 `0` 时 host 返回 0；
写非零值时会完整打印 guest code，而 host 返回 1。

## 仿真命令行

```text
my_core_sim [options]

  --elf FILE                 加载 little-endian ELF32/ELF64 的 PT_LOAD
  --rom-image FILE           将 raw image 加载到 ROM base
  --ram-image FILE           将 raw image 加载到 RAM base
  --max-cycles N             reset 后的最大 active cycles；默认 10000000
  --reset-cycles N           reset rising edges；默认 5
  --uart-in FILE|-           UART 输入；默认 stdin，- 也表示 stdin
  --uart-out FILE|-          UART 输出；默认 stdout，- 也表示 stdout
  --seed N                   fabric 随机 stall seed；默认 1
  --stall-probability P      AW/W/AR READY stall 概率 [0,1]；默认 0
  --trace FILE               写 VCD；目标必须以 TRACE 编译
```

`--elf` 不能和 raw image 选项同时使用。ELF loader 先验证全部 PT_LOAD，再写入
ROM/RAM，并将 `p_memsz - p_filesz` 区域清零。返回码为：guest PASS `0`、guest
非零退出 `1`、配置或镜像错误 `2`、AXI 协议错误（或 DUT 提前 `$finish`）`3`、
超时 `124`、SIGINT `130`。

## 构建与测试

使用 preset 时需要 CMake 3.25 或更新版本、C++20 编译器和 Verilator 5+：

```sh
cmake --preset default
cmake --build --preset default -j
ctest --preset default
```

只运行自包含 core 或 protocol 分组：

```sh
ctest --preset core
ctest --preset protocol
```

固定 seed 的双端口 differential stress 在 100,000 个随机流量周期后 drain，覆盖
不同 AxSIZE、INCR burst、非对齐首拍、partial WSTRB、AW/W 任意先后以及请求和
响应背压，并逐字节比较 RAM golden model：

```sh
cmake --build build/default --target axi_tb_host_fabric_stress -j
ctest --test-dir build/default -R '^core\.host_fabric_stress$' --output-on-failure
```

上述 preset、host test、Verilator 和 Fuxi staging 的产物都位于仓库内的
`build/`；正常构建和测试不需要从 `/private/tmp` 运行可执行文件。

## 可选 Fuxi preset

默认构建不读取 Fuxi 或 riscv-tests。`fuxi` preset 预期相邻目录布局如下：

```text
parent/
├── verilator-axi-testbench/
├── Fuxi/
└── riscv-tests/
```

它使用 `../Fuxi` 和 `../riscv-tests`，启用 Fuxi 及 RV32I/M/A 软件集成：

```sh
cmake --preset fuxi
cmake --build --preset fuxi -j2
ctest --preset fuxi
```

该配置还需要 OpenJDK 11、sbt、支持
`rv32ima_zicsr_zifencei/ilp32` 的 Clang，以及 `ld.lld`。可用以下 cache 变量
覆盖自动发现结果或相邻 checkout：

- `AXI_TB_FUXI_SOURCE_DIR`
- `AXI_TB_FUXI_JAVA_HOME`
- `AXI_TB_FUXI_SBT_EXECUTABLE`
- `AXI_TB_RISCV_TESTS_SOURCE_DIR`
- `AXI_TB_RISCV_CLANG`
- `AXI_TB_RISCV_LLD`

配置阶段只把 Fuxi 所需文件复制到 build staging，并在 staging 中用 Java 11、
Chisel 3.2.8 和 iotesters 1.3.8 生成 `Fuxi.v`；不会修改外部 checkout。staging
还会锁存 AMO 的旧读值，以保持原 SoC 所依赖的 FPGA read-first BRAM 语义，避免
当前 Verilator 对旧 `SyncReadMem` 模型呈现 write-first。补丁会检查预期源码形状，
不匹配的 Fuxi revision 会在配置阶段明确失败。Fuxi 的三个端口固定映射为
instruction/data/uncached，IRQ 绑 0，并断言遗留 AXI3 `WID` 始终为 0。

可选测试包含 UART/Exit smoke 和直接从 upstream assembly 编译的 59 个
RV32I/M/A guest。`ma_data` 作为单独的 misaligned-access capability 测试，默认
不属于 59 项门槛；它用于记录 DUT 选择 trap 还是硬件完成非对齐访问。是否通过
这些外部测试取决于本机 checkout、工具链与 DUT，本 README 不把它们声明为已在
所有环境验证。

配置成功后可按 CTest label 分组运行：

```sh
ctest --preset fuxi -L fuxi
ctest --preset fuxi -L rv32i
ctest --preset fuxi -L rv32m
ctest --preset fuxi -L rv32a
```

如需显式加入 Fuxi 预期以 trap 处理的 `ma_data` capability 项，重新配置时设置
`-DAXI_TB_FUXI_RUN_MISALIGNED_CAPABILITY=ON`，随后可用 `-L capability` 单独选择；该
选项默认关闭。
