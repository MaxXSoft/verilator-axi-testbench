#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "devices.hpp"

namespace {
struct CountingBackend {
  axi_tb::BufferUartBackend buffer;
  unsigned probes = 0;

  bool try_read(std::uint8_t &byte) {
    ++probes;
    return buffer.try_read(byte);
  }
  void write(std::uint8_t byte) { buffer.write(byte); }
  void flush() {}
};
}  // namespace

// Register transactions and exceptions intentionally fail this host test.
// NOLINTNEXTLINE(bugprone-exception-escape,readability-function-cognitive-complexity)
int main() {
  CountingBackend backend;
  axi_tb::UartDevice uart(backend);
  uart.set_input_poll_cycles(8);
  uart.set_character_cycles(1);
  constexpr std::array<std::uint8_t, 1> ENABLE{1};
  const auto write = [&](std::uint64_t offset, std::uint8_t value) {
    const std::array<std::byte, 1> bytes{static_cast<std::byte>(value)};
    assert(uart.write(offset, bytes, ENABLE) == axi_tb::Response::Okay);
  };
  const auto read = [&](std::uint64_t offset) {
    std::array<std::byte, 1> bytes{};
    assert(uart.read(offset, bytes, ENABLE) == axi_tb::Response::Okay);
    return std::to_integer<std::uint8_t>(bytes[0]);
  };
  const auto irq = [&] { return uart.output("irq")->value; };

  write(2, 0x41);  // FIFO enabled, trigger four bytes.
  write(1, 1);
  uart.tick();
  assert(backend.probes == 1);
  backend.buffer.push_input('a');
  for (unsigned i = 0; i < 7; ++i) {
    uart.tick();
    // Guest polling must not bypass the host-input backoff.
    assert((read(5) & 1U) == 0);
    assert(backend.probes == 1 && irq() == 0);
  }
  uart.tick();
  assert(backend.probes == 3);  // One byte followed by an empty read.
  assert((read(5) & 1U) == 1 && irq() == 0);
  for (unsigned i = 0; i < 3; ++i) {
    uart.tick();
    assert(irq() == 0);
  }
  uart.tick();
  // RX timeout remains four character cycles, even during host backoff.
  assert(irq() == 1 && read(2) == 0xcc && backend.probes == 3);
  assert(read(0) == 'a' && irq() == 0);

  // Reset cancels the pending delay but preserves the polling configuration.
  uart.reset();
  write(2, 1);
  write(1, 1);
  for (std::uint8_t i = 0; i < 32; ++i) {
    backend.buffer.push_input(i);
  }
  uart.tick();
  assert(irq() == 1);
  const auto full_probes = backend.probes;
  for (unsigned i = 0; i < 10; ++i) {
    uart.tick();
  }
  assert(backend.probes == full_probes);  // Full FIFO backpressures the host.
  for (unsigned i = 0; i < 32; ++i) {
    assert(read(0) == i);  // Pending bytes refill without a batch-sized gap.
  }
  assert((read(5) & 1U) == 0 && irq() == 0);
  const auto empty_probes = backend.probes;
  for (unsigned i = 0; i < 7; ++i) {
    uart.tick();
  }
  assert(backend.probes == empty_probes);
  uart.tick();
  assert(backend.probes == empty_probes + 1);

  // Opting out restores immediate input, including reads between ticks.
  uart.set_input_poll_cycles(1);
  assert((read(5) & 1U) == 0);
  backend.buffer.push_input('z');
  assert((read(5) & 1U) == 1 && irq() == 1);
  assert(read(0) == 'z');
  bool rejected = false;
  try {
    uart.set_input_poll_cycles(0);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  assert(rejected);
}
