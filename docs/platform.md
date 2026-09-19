# Static C++ devices and platforms

`axi_tb::Device` in `src/device.hpp` remains the bus interface. Implement its
beat-wide `read_impl`/`write_impl` methods, preserving byte enables and strobes.
Return `SlaveError` for unsupported accesses. Burst/exclusive/load support is
opt-in. An in-range `load()` must succeed when `can_load()` returned true;
image preflight relies on this contract (there is no rollback for a broken
custom device). Host loading bypasses bus write permissions.

The public headers are exported by `axi_tb::core`. No binary ABI is promised:
build external device sources with the same C++20 toolchain as the simulator.

## Register and assemble

A device type supplies an option schema and a factory. Types use explicit
registration; no static constructors or linker discovery are involved.

```cpp
struct MyPlatform {
  static void register_devices(axi_tb::DeviceRegistry &registry) {
    registry.add("my-timer", {
        {{"divider", axi_tb::OptionKind::Unsigned, "10",
          "Simulated cycles per timer tick", 1, 1000000}},
        [](const axi_tb::DeviceConfig &config, axi_tb::HostServices &) {
          return std::make_unique<MyTimer>(config.number("divider"));
        }, {}});
  }

  static axi_tb::PlatformSpec defaults(const axi_tb::DefaultMap &map) {
    auto spec = axi_tb::default_platform(map);
    spec.remove("uart");                  // mappings and connections removed too
    spec.map("ram", 0x80000000, 0x100000); // map size and capacity are independent
    spec.set("ram.size=0x100000");
    spec.devices.push_back({"timer", "my-timer", {{"divider", "20"}}});
    spec.map("timer", 0x11000000, 0x10000);
    return spec;
  }
};
```

Alternatively start with `PlatformSpec{}`. `devices`, `mappings`, and
`connections` are ordinary editable vectors. Multiple instances and aliases
are supported; `spec.map()` replaces an instance's mappings, while appending
to `mappings` creates an alias. Devices can have no MMIO mapping. To replace a
device while retaining its wiring, change `spec.device(id).type` and its
`properties`. Names consist of letters, digits, underscores, and hyphens.

```cmake
add_axi_testbench(
  TARGET my_sim TOP my_adapter RTL_SOURCES my_adapter.sv
  PLATFORM_HEADER my_platform.hpp PLATFORM_TYPE MyPlatform
  CXX_SOURCES my_platform.cpp my_timer.cpp
  INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/sim"
)
```

The default without these arguments is `axi_tb::DefaultPlatform`. Built-ins
(`rom`, `ram`, `uart`, `exit`) are always registered, but instantiated only when
listed in the chosen specification. The existing ROM/RAM/UART/Exit CMake
parameters describe the default map and still generate the guest header and
linker script. A custom platform must keep its guest configuration consistent
with any overrides. Custom mappings receive runtime overflow, overlap, and
address-width checks before simulation, and the map then becomes immutable.

## Properties and loading

`--set INSTANCE.PROPERTY=VALUE` overrides platform defaults. Schemas declare
string, unsigned decimal/hexadecimal, or boolean (`true`/`false`) values, plus
help/defaults and numeric bounds. Unknown properties, duplicate types or
instances, and invalid values fail startup. `--help` lists registered types
and properties without constructing devices or opening files. Devices receive
validated values and never parse argv themselves.

ROM and RAM declare `size` and `image`; UART declares `input`, `output`, and
`character-cycles` (used for deterministic receive timeout timing).
Factories may use `HostServices` for owned files and terminal input, or own
other resources in the returned device. `DeviceType::image_option` optionally
identifies a string property for a raw image loaded at the instance's sole
mapping. More complex loading uses the generic `--load` interface:

```sh
my_sim --set rom.image=boot.bin --elf kernel.elf --load 0x81000000=initrd.bin
my_sim --rom-image boot.bin --elf kernel.elf --load ram=data.bin
my_sim --set uart.input=input.bin --set uart.output=console.log
```

The second example works only if `kernel.elf` does not overlap `data.bin`.
`--elf` and `--load` are repeatable; all raw files, ELF segments and BSS ranges
are preflighted together before any image bytes are applied. The old
`--rom-image`, `--ram-image`, `--uart-in`, and `--uart-out` flags are aliases
for properties of the preset instances. `--load NAME=FILE` requires one mapping
with that name, or use a numeric address. Loading never changes the reset PC.

## Sidebands and lifecycle

Devices expose stable `Signal` outputs and `InputSignal` inputs through
`output(name)`/`input(name)`. Widths are 1..64 bits, input values default to
zero. Use `spec.connections.push_back({"uart.irq", "plic.source10"})` to connect
them. Each input accepts one driver; mismatched widths, missing endpoints and
cycles are rejected. Unused outputs are allowed.

A binding class connects device outputs to additional ports of a particular
Verilated top. This is independent of AXI, whose canonical macro stays intact:

```cpp
struct MyBinding {
  const axi_tb::Signal &irq;
  explicit MyBinding(axi_tb::Platform &p) : irq(p.output("timer.irq")) {}
  template<class Top> void drive(Top &top) const { top.timer_irq = irq.value; }
};
```

Select it using `SIDEBAND_HEADER my_platform.hpp SIDEBAND_TYPE MyBinding`.
The default `NoSideband` ignores every output and needs no additional RTL
ports. Bind once at startup; no string lookups occur in the simulation loop.

The platform owns all devices. It resets them at construction and once when
entering reset, including unmapped devices. ROM/RAM preserve loaded bytes.
For each active cycle the runner drives sidebands, evaluates both clock
phases, commits AXI accesses, calls each device's `tick()` once, then calls
`settle()` in topological connection order. `settle()` may propagate levels
and accept interrupt requests but must not advance simulated time. A change
caused by MMIO or a tick reaches the DUT on the next cycle. Reset cycles do
not advance devices. Tick must not block on host I/O or wall-clock time.

Standalone `AxiFabric(space)` retains its original reset behavior. The shared
runner uses `AxiFabric(space, false)` because the platform owns device reset.
Exit still terminates only after the corresponding AXI B response handshake.

Device-tree output is deferred. Keep platform addresses, interrupt topology
and guest constants explicit so a later description API can reuse them.
