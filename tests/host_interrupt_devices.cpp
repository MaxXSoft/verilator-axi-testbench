#include <array>
#include <cassert>

#include "devices.hpp"
#include "interrupt_devices.hpp"
#include "platform.hpp"

namespace {
using axi_tb::Device;
using axi_tb::Response;
std::uint32_t read(Device &d, std::uint64_t offset) {
  std::array<std::byte, 4> data{};
  constexpr std::array<std::uint8_t, 4> lanes{1, 1, 1, 1};
  assert(d.read(offset, data, lanes) == Response::Okay);
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) {
    value |= std::to_integer<std::uint32_t>(data[i]) << (i * 8);
  }
  return value;
}
void write(Device &d, std::uint64_t offset, std::uint32_t value) {
  std::array<std::byte, 4> data{};
  constexpr std::array<std::uint8_t, 4> lanes{1, 1, 1, 1};
  for (unsigned i = 0; i < 4; ++i) {
    data[i] = std::byte((value >> (i * 8)) & 0xffU);
  }
  assert(d.write(offset, data, lanes) == Response::Okay);
}
void test_clint() {
  fuxi_sim::Clint c(4);
  assert(c.output("mtime")->width == 64 && c.output("mtip")->value == 0);
  write(c, 0, 3);
  assert(read(c, 0) == 1 && c.output("msip")->value == 1);
  write(c, 0, 2);
  assert(read(c, 0) == 0);
  for (unsigned i = 0; i < 7; ++i) {
    c.tick();
  }
  assert(read(c, 0xbff8) == 1);
  c.tick();
  assert(read(c, 0xbff8) == 2);
  write(c, 0x4000, 2);
  write(c, 0x4004, 0);
  assert(c.output("mtip")->value == 1);  // unsigned >= comparison
  write(c, 0x4000, 3);
  assert(c.output("mtip")->value == 0);
  write(c, 0xbffc, 1);
  write(c, 0xbff8, 0xffffffff);
  for (unsigned i = 0; i < 4; ++i) {
    c.tick();
  }
  assert(read(c, 0xbffc) == 2 && read(c, 0xbff8) == 0);
  std::array<std::byte, 8> wide{};
  constexpr std::array<std::uint8_t, 8> all{1, 1, 1, 1, 1, 1, 1, 1};
  assert(c.read(0xbff8, wide, all) == Response::Okay);
  assert(wide[4] == std::byte{2});
  wide.fill(std::byte{0xff});
  assert(c.write(0x4000, wide, all) == Response::Okay);
  assert(c.output("mtip")->value == 0);
  constexpr std::array<std::uint8_t, 8> sparse{1, 1, 0, 1, 0, 0, 0, 0};
  assert(c.write(0x4000, wide, sparse) == Response::SlaveError);
  assert(read(c, 0x4000) == 0xffffffff);
  c.reset();
  assert(c.output("mtime")->value == 0 && c.output("msip")->value == 0 &&
         c.output("mtip")->value == 0);
}
// This stateful sequence follows a request through arbitration and completion.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void test_plic() {
  fuxi_sim::Plic p(63, 2);
  axi_tb::Signal a;
  axi_tb::Signal b;
  axi_tb::Signal high;
  p.input("source2")->source = &a;
  p.input("source3")->source = &b;
  p.input("source40")->source = &high;
  assert(p.input("source0") == nullptr);
  const auto m = [&] { return p.output("context0")->value; };
  const auto s = [&] { return p.output("context1")->value; };
  write(p, 0, 7);
  assert(read(p, 0) == 0);
  write(p, 8, 3);
  write(p, 12, 3);
  write(p, 0x2000, 0xd);  // bit 0 must remain zero
  assert(read(p, 0x2000) == 0xc);
  write(p, 0x2080, 0xc);
  a.value = b.value = 1;
  p.settle();
  assert(m() && s() && read(p, 0x1000) == 0xc);
  a.value = 0;
  p.settle();
  assert(read(p, 0x1000) == 0xc);  // Falling input cannot retract a request.
  assert(read(p, 0x200004) == 2);  // Lower ID wins equal priorities.
  assert(read(p, 0x201004) ==
         3);  // Global pending is cleared for all contexts.
  assert(!m() && !s());
  p.settle();
  assert(read(p, 0x200004) == 0);  // Gateway waits for completion.
  write(p, 0x201004, 3);           // Still high: one new pending request.
  assert(m() && s());
  write(p, 0x200000, 7);
  assert(!m() && s());
  assert(read(p, 0x200004) == 3);  // Claim ignores notification threshold.
  write(p, 0x2000, 0);  // Completion with source disabled must be ignored.
  write(p, 0x200004, 3);
  p.settle();
  assert(!s());
  write(p, 0x201004, 3);
  assert(s());
  b.value = 0;
  assert(read(p, 0x201004) == 3);
  write(p, 0x201004, 3);
  assert(!s());
  write(p, 0x200004, 9999);  // Invalid completion ignored.

  // Source numbers above 31, priority zero masking and high lanes on a 64-bit
  // bus.
  high.value = 1;
  write(p, 0x2084, 1U << 8U);
  p.settle();
  assert(!s() && read(p, 0x1004) == (1U << 8U));
  write(p, UINT64_C(40) * 4, 0xffffffff);
  assert(read(p, UINT64_C(40) * 4) == 7 && s());
  std::array<std::byte, 8> wide{};
  constexpr std::array<std::uint8_t, 8> partial{0, 0, 0, 0, 1, 1, 0, 1};
  assert(p.read(0x201000, wide, partial) == Response::SlaveError);
  assert(s());  // Failed read cannot claim the interrupt.
  constexpr std::array<std::uint8_t, 8> upper{0, 0, 0, 0, 1, 1, 1, 1};
  assert(p.read(0x201000, wide, upper) == Response::Okay);
  assert(wide[4] == std::byte{40} && !s());
  high.value = 0;
  write(p, 0x201004, 40);
  p.reset();
  assert(!m() && !s() && read(p, 0x1000) == 0 && read(p, 0x2084) == 0);
}
void test_connected_uart() {
  axi_tb::BufferUartBackend backend;
  axi_tb::DeviceRegistry registry;
  registry.add("test-uart",
               {
                   .options = {},
                   .create =
                       [&](const auto &, auto &) {
                         return std::make_unique<axi_tb::UartDevice>(backend);
                       },
                   .image_option = {},
               });
  registry.add("plic", {
                           .options = {},
                           .create =
                               [](const auto &, auto &) {
                                 return std::make_unique<fuxi_sim::Plic>();
                               },
                           .image_option = {},
                       });
  axi_tb::PlatformSpec spec;
  spec.devices = {
      {.id = "plic", .type = "plic", .properties = {}},
      {.id = "uart", .type = "test-uart", .properties = {}},
  };
  spec.connections = {{.source = "uart.irq", .destination = "plic.source10"}};
  axi_tb::Platform platform(registry, spec);
  auto &p = platform.device("plic");
  auto &u = platform.device("uart");
  write(p, 40, 1);
  write(p, 0x2000, 1U << 10U);
  const std::array<std::byte, 1> rx_enable{std::byte{1}};
  constexpr std::array<std::uint8_t, 1> lane{1};
  assert(u.write(1, rx_enable, lane) == Response::Okay);
  backend.push_input('Z');
  platform.end_cycle(false);
  assert(platform.output("plic.context0").value == 1);
  assert(read(p, 0x200004) == 10);
  std::array<std::byte, 1> received{};
  assert(u.read(0, received, lane) == Response::Okay);
  assert(received[0] == std::byte{'Z'});
  write(p, 0x200004, 10);
  platform.end_cycle(false);
  assert(platform.output("plic.context0").value == 0);
  platform.begin_cycle(true);
  assert(platform.output("uart.irq").value == 0);
}
}  // namespace
// Uncaught exceptions intentionally fail this standalone test executable.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
  test_clint();
  test_plic();
  test_connected_uart();
}
