#include <array>
#include <cassert>

#include "fuxi_platform.hpp"

namespace {
struct SidebandTop {
  std::uint64_t irq_msip = 0;
  std::uint64_t irq_mtip = 0;
  std::uint64_t irq_meip = 0;
  std::uint64_t irq_seip = 0;
  std::uint64_t rtc_time = 0;
};
void write32(axi_tb::AddressSpace &space, std::uint64_t address,
             std::uint32_t value) {
  std::array<std::byte, 4> data{};
  constexpr std::array<std::uint8_t, 4> lanes{1, 1, 1, 1};
  for (unsigned i = 0; i < 4; ++i) {
    data[i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
  }
  assert(space.write(address, data, lanes) == axi_tb::Response::Okay);
}
}  // namespace

// Uncaught exceptions intentionally fail this standalone test executable.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
  axi_tb::DeviceRegistry registry;
  axi_tb::register_builtin_devices(registry);
  fuxi_sim::PlatformDefinition::register_devices(registry);
  const auto spec = fuxi_sim::PlatformDefinition::defaults({});
  axi_tb::Platform platform(registry, spec, 32);
  const fuxi_sim::SidebandBinding binding(platform);
  auto &space = platform.address_space();
  assert(space.mappings().size() == 6);
  assert(space.resolve(0, 0x10000)->name == "rom");
  assert(space.resolve(0x80000000, 0x08000000)->name == "ram");
  assert(space.resolve(0x10000000, 0x100)->name == "uart");
  assert(space.resolve(0x10001000, 4)->name == "exit");
  assert(space.resolve(FUXI_CLINT_BASE, FUXI_CLINT_SIZE)->name == "clint");
  assert(space.resolve(FUXI_PLIC_BASE, FUXI_PLIC_SIZE)->name == "plic");
  write32(space, FUXI_CLINT_BASE, 1);
  write32(space, FUXI_PLIC_BASE + FUXI_UART_IRQ * 4U, 1);
  write32(space, FUXI_PLIC_BASE + 0x2000,
          1U << static_cast<unsigned>(FUXI_UART_IRQ));
  constexpr std::array<std::byte, 1> enable_tx{std::byte{2}};
  constexpr std::array<std::uint8_t, 1> byte_lane{1};
  assert(space.write(0x10000001, enable_tx, byte_lane) ==
         axi_tb::Response::Okay);
  platform.end_cycle(false);
  SidebandTop top;
  binding.drive(top);
  assert(top.irq_msip == 1 && top.irq_mtip == 0);
  assert(top.irq_meip == 1 && top.irq_seip == 0 && top.rtc_time == 1);
  platform.begin_cycle(true);
  binding.drive(top);
  assert(top.irq_msip == 0 && top.irq_meip == 0 && top.rtc_time == 0);
}
