#include <array>
#include <cassert>

#include "devices.hpp"

// Keep this stateful register sequence together; exceptions fail the test.
// NOLINTNEXTLINE(bugprone-exception-escape,readability-function-cognitive-complexity)
int main() {
  axi_tb::BufferUartBackend backend;
  axi_tb::UartDevice uart(backend);
  uart.set_character_cycles(2);
  constexpr std::array<std::uint8_t, 1> enable{1};
  const auto write = [&](std::uint64_t offset, std::uint8_t value) {
    const std::array<std::byte, 1> bytes{static_cast<std::byte>(value)};
    assert(uart.write(offset, bytes, enable) == axi_tb::Response::Okay);
  };
  const auto read = [&](std::uint64_t offset) {
    std::array<std::byte, 1> bytes{};
    assert(uart.read(offset, bytes, enable) == axi_tb::Response::Okay);
    return std::to_integer<std::uint8_t>(bytes[0]);
  };
  const auto irq = [&] { return uart.output("irq")->value; };
  assert(irq() == 0);
  write(1, 2);  // Enabling THRE on an empty transmitter asserts the IRQ.
  assert(irq() == 1);
  assert(read(2) == 2);
  assert(irq() ==
         0);  // IIR acknowledges THRE, empty alone cannot retrigger it.
  uart.tick();
  assert(irq() == 0);
  write(0, 'X');
  assert(irq() == 0);
  uart.tick();
  assert(irq() == 1 && backend.output()[0] == std::byte{'X'});
  write(1, 0);
  assert(irq() == 0);

  uart.reset();
  write(2, 0x41);  // FIFO enabled, RX trigger 4.
  write(1, 3);
  backend.push_input('a');
  uart.tick();                            // RX must work with no guest reads.
  assert(irq() == 1 && read(2) == 0xc2);  // Below trigger, THRE has priority.
  assert(irq() == 0);
  for (unsigned i = 0; i < 7; ++i) {
    uart.tick();
  }
  assert(irq() == 0);
  uart.tick();
  assert(irq() == 1 && read(2) == 0xcc);  // Four simulated characters of idle.
  assert(irq() == 1);  // IIR does not acknowledge RX timeout.
  assert(read(0) == 'a' && irq() == 0);
  for (unsigned i = 0; i < 4; ++i) {
    backend.push_input('b' + i);
  }
  uart.tick();
  assert(irq() == 1 && read(2) == 0xc4);
  assert(read(0) == 'b' && irq() == 0);
  write(1, 0);
  for (unsigned i = 0; i < 10; ++i) {
    uart.tick();
  }
  assert(irq() == 0);
  write(1, 1);
  assert(irq() == 1);
  write(2, 0x43);  // RX FIFO reset deasserts its IRQ.
  assert(irq() == 0);

  // DLAB accesses cannot enable IRQ or transmit bytes.
  write(3, 0x80);
  write(0, 0xaa);
  write(1, 0xff);
  assert(irq() == 0 && backend.output().size() == 1);
  assert(read(0) == 0xaa && read(1) == 0xff);
  write(3, 3);

  // Linux-style loopback and modem-status probes, plus line-status priority.
  uart.reset();
  write(4, 0x1f);
  assert((read(6) & 0xf0U) == 0xf0);
  write(1, 5);  // RX and receiver-line-status IRQs.
  write(0, 'p');
  write(0, 'q');  // Non-FIFO receiver overrun.
  assert(read(2) == 6 && irq() == 1);
  assert((read(5) & 2U) == 2);
  assert(read(2) == 4 && read(0) == 'p');
  assert(irq() == 0 && backend.output().size() == 1);
  uart.reset();
  assert(irq() == 0 && read(1) == 0 && read(5) == 0x60);
}
