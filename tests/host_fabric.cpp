#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

#include "devices.hpp"
#include "fabric.hpp"

namespace {

using Fabric = axi_tb::AxiFabric<2, 32, 32, 4>;
using Master = Fabric::Master;
using Inputs = std::array<Master, 2>;

int failures = 0;

void check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::array<Fabric::Slave, 2> tick(Fabric &fabric, const Inputs &inputs,
                                  bool reset = false) {
  const auto outputs = fabric.drive(reset);
  fabric.commit(inputs, reset);
  return outputs;
}

axi_tb::AddressPayload address(std::uint32_t value, std::uint8_t id = 0,
                               bool exclusive = false) {
  axi_tb::AddressPayload result;
  result.id = id;
  result.address = value;
  result.size = 2;
  result.burst = axi_tb::Burst::Increment;
  result.lock = exclusive;
  return result;
}

Fabric::WriteBeat word(std::uint32_t value, bool last = true) {
  Fabric::WriteBeat result;
  for (std::size_t index = 0; index < 4; ++index) {
    result.data[index] = static_cast<std::uint8_t>(value >> (index * 8U));
    result.strobe[index] = 1;
  }
  result.last = last;
  return result;
}

std::uint32_t word_value(const Fabric::ReadBeat &beat) {
  std::uint32_t result = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    result |= static_cast<std::uint32_t>(beat.data[index]) << (index * 8U);
  }
  return result;
}

std::uint32_t ram_word(const axi_tb::RamDevice &ram, std::size_t offset) {
  std::uint32_t result = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    result |= static_cast<std::uint32_t>(
                  std::to_integer<std::uint8_t>(ram.bytes()[offset + index]))
              << (index * 8U);
  }
  return result;
}

void consume_b(Fabric &fabric, Inputs &inputs, std::size_t port,
               axi_tb::Response expected) {
  auto outputs = tick(fabric, inputs);
  check(outputs[port].b_valid, "write response becomes valid");
  if (outputs[port].b_valid) {
    check(outputs[port].b.response == expected, "write response code");
  }
  inputs[port].b_ready = true;
  tick(fabric, inputs);
  inputs[port].b_ready = false;
}

void write_single(Fabric &fabric, Inputs &inputs, std::size_t port,
                  std::uint32_t location, std::uint32_t value,
                  std::uint8_t id = 0, bool exclusive = false) {
  inputs[port].aw_valid = true;
  inputs[port].aw = address(location, id, exclusive);
  inputs[port].w_valid = true;
  inputs[port].w = word(value);
  const auto outputs = tick(fabric, inputs);
  check(outputs[port].aw_ready && outputs[port].w_ready,
        "single write address and data accepted");
  inputs[port].aw_valid = false;
  inputs[port].w_valid = false;
}

template <std::size_t BeatCount>
void write_burst(Fabric &fabric, Inputs &inputs, std::size_t port,
                 axi_tb::AddressPayload payload,
                 const std::array<std::uint32_t, BeatCount> &values) {
  static_assert(BeatCount > 0 && BeatCount <= 256);
  payload.length = static_cast<std::uint8_t>(BeatCount - 1);
  inputs[port].aw_valid = true;
  inputs[port].aw = payload;
  inputs[port].w_valid = true;
  for (std::size_t beat = 0; beat < BeatCount; ++beat) {
    inputs[port].w = word(values[beat], beat + 1 == BeatCount);
    const auto outputs = tick(fabric, inputs);
    check(outputs[port].w_ready, "burst W beat accepted");
    if (beat == 0) {
      check(outputs[port].aw_ready, "burst AW accepted");
      inputs[port].aw_valid = false;
    }
  }
  inputs[port].w_valid = false;

  std::array<Fabric::Slave, 2> outputs{};
  std::size_t wait_cycles = 0;
  do {
    outputs = tick(fabric, inputs);
    ++wait_cycles;
  } while (!outputs[port].b_valid && wait_cycles <= BeatCount + 2);
  check(outputs[port].b_valid &&
            outputs[port].b.response == axi_tb::Response::Okay,
        "burst write returns OKAY");
  inputs[port].b_ready = true;
  tick(fabric, inputs);
  inputs[port].b_ready = false;
}

Fabric::ReadBeat read_single(Fabric &fabric, Inputs &inputs, std::size_t port,
                             std::uint32_t location, std::uint8_t id = 0,
                             bool exclusive = false) {
  inputs[port].ar_valid = true;
  inputs[port].ar = address(location, id, exclusive);
  const auto accepted = tick(fabric, inputs);
  check(accepted[port].ar_ready, "single read address accepted");
  inputs[port].ar_valid = false;
  const auto outputs = tick(fabric, inputs);
  check(outputs[port].r_valid, "single read response becomes valid");
  auto result = outputs[port].r;
  inputs[port].r_ready = true;
  tick(fabric, inputs);
  inputs[port].r_ready = false;
  return result;
}

void test_late_wlast_error_is_atomic() {
  axi_tb::RamDevice ram(0x1000);
  axi_tb::AddressSpace space;
  space.map(0x1000, ram.size(), ram, "ram");
  Fabric fabric(space);
  Inputs inputs{};
  tick(fabric, inputs, true);

  inputs[0].aw_valid = true;
  inputs[0].aw = address(0x1000, 0x0a);
  inputs[0].aw.length = 1;
  inputs[0].w_valid = true;
  inputs[0].w = word(0x44332211, false);
  const auto accepted = tick(fabric, inputs);
  check(accepted[0].aw_ready && accepted[0].w_ready,
        "first beat of malformed burst is accepted");
  inputs[0].aw_valid = false;
  check(ram.bytes()[0] == std::byte{0},
        "staged first beat is not visible before the complete burst");

  inputs[0].w = word(0x88776655, false);  // Final beat must assert WLAST.
  bool saw_error = false;
  std::string error_message;
  try {
    tick(fabric, inputs);
  } catch (const axi_tb::ProtocolError &error) {
    saw_error = true;
    error_message = error.what();
  }
  check(saw_error, "late missing WLAST is rejected");
  check(error_message.find("ID 0xa") != std::string::npos &&
            error_message.find("address 0x1000") != std::string::npos,
        "WLAST diagnostic contains transaction ID and address");
  bool unchanged = true;
  for (std::size_t index = 0; index < 8; ++index) {
    unchanged &= ram.bytes()[index] == std::byte{0};
  }
  check(unchanged, "late WLAST error leaves no earlier RAM side effect");
}

void test_late_wstrb_error_is_atomic() {
  axi_tb::RamDevice ram(0x1000);
  axi_tb::AddressSpace space;
  space.map(0x1000, ram.size(), ram, "ram");
  Fabric fabric(space);
  Inputs inputs{};
  tick(fabric, inputs, true);

  inputs[0].aw_valid = true;
  inputs[0].aw = address(0x1000, 0x0b);
  inputs[0].aw.length = 1;
  inputs[0].aw.size = 0;
  inputs[0].w_valid = true;
  inputs[0].w = {};
  inputs[0].w.data[0] = 0x11;
  inputs[0].w.strobe[0] = 1;
  inputs[0].w.last = false;
  tick(fabric, inputs);
  inputs[0].aw_valid = false;
  check(ram.bytes()[0] == std::byte{0},
        "narrow first beat remains staged before final W");

  inputs[0].w = {};
  inputs[0].w.data[0] = 0x22;
  inputs[0].w.strobe[0] = 1;  // Beat 1 at 0x1001 requires lane 1.
  inputs[0].w.last = true;
  bool saw_error = false;
  try {
    tick(fabric, inputs);
  } catch (const axi_tb::ProtocolError &) {
    saw_error = true;
  }
  check(saw_error, "late illegal WSTRB is rejected");
  check(ram.bytes()[0] == std::byte{0} && ram.bytes()[1] == std::byte{0},
        "late WSTRB error leaves no earlier RAM side effect");
}

void test_regular_burst_commits_one_beat_per_cycle() {
  axi_tb::RamDevice ram(0x1000);
  axi_tb::AddressSpace space;
  space.map(0x1000, ram.size(), ram, "ram");
  Fabric fabric(space);
  Inputs inputs{};
  tick(fabric, inputs, true);

  inputs[0].aw_valid = true;
  inputs[0].aw = address(0x1000, 0x0c);
  inputs[0].aw.length = 2;
  inputs[0].w_valid = true;
  inputs[0].w = word(0x11111111, false);
  tick(fabric, inputs);
  inputs[0].aw_valid = false;
  inputs[0].w = word(0x22222222, false);
  tick(fabric, inputs);
  check(ram_word(ram, 0) == 0 && ram_word(ram, 4) == 0 && ram_word(ram, 8) == 0,
        "incomplete regular burst has no side effect");

  inputs[0].w = word(0x33333333, true);
  tick(fabric, inputs);
  inputs[0].w_valid = false;
  check(ram_word(ram, 0) == 0x11111111 && ram_word(ram, 4) == 0 &&
            ram_word(ram, 8) == 0,
        "final W cycle commits only the first staged beat");

  auto outputs = tick(fabric, inputs);
  check(!outputs[0].b_valid && ram_word(ram, 4) == 0x22222222 &&
            ram_word(ram, 8) == 0,
        "next cycle commits only the second staged beat");
  outputs = tick(fabric, inputs);
  check(!outputs[0].b_valid && ram_word(ram, 8) == 0x33333333,
        "last device beat commits before the response");
  outputs = tick(fabric, inputs);
  check(outputs[0].b_valid && outputs[0].b.id == 0x0c,
        "regular burst response follows the final committed beat");
}

void test_fixed_wrap_and_reset() {
  axi_tb::RamDevice ram(0x1000);
  axi_tb::AddressSpace space;
  space.map(0x1000, ram.size(), ram, "ram");
  Fabric fabric(space);
  Inputs inputs{};
  tick(fabric, inputs, true);

  auto fixed = address(0x1000, 1);
  fixed.burst = axi_tb::Burst::Fixed;
  write_burst(fabric, inputs, 0, fixed,
              std::array<std::uint32_t, 3>{0x11111111, 0x22222222, 0x33333333});
  check(ram_word(ram, 0) == 0x33333333,
        "FIXED burst repeatedly updates one transfer address");

  auto wrap = address(0x101c, 2);
  wrap.burst = axi_tb::Burst::Wrap;
  write_burst(fabric, inputs, 0, wrap,
              std::array<std::uint32_t, 4>{0xaaaaaaaa, 0xbbbbbbbb, 0xcccccccc,
                                           0xdddddddd});
  check(ram_word(ram, 0x1c) == 0xaaaaaaaa &&
            ram_word(ram, 0x10) == 0xbbbbbbbb &&
            ram_word(ram, 0x14) == 0xcccccccc &&
            ram_word(ram, 0x18) == 0xdddddddd,
        "WRAP burst commits in wrapped address order");

  inputs[0].aw_valid = true;
  inputs[0].aw = address(0x1040, 3);
  inputs[0].aw.length = 1;
  inputs[0].w_valid = true;
  inputs[0].w = word(0xfeedface, false);
  tick(fabric, inputs);
  inputs = {};
  tick(fabric, inputs, true);
  check(fabric.idle() && ram_word(ram, 0x40) == 0,
        "reset discards an incomplete staged write");
  check(ram_word(ram, 0) == 0x33333333,
        "reset preserves already committed RAM contents");
}

}  // namespace

// Test assertions and helpers intentionally report failures by throwing.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
  test_late_wlast_error_is_atomic();
  test_late_wstrb_error_is_atomic();
  test_regular_burst_commits_one_beat_per_cycle();
  test_fixed_wrap_and_reset();

  axi_tb::RomDevice rom(0x1000);
  axi_tb::RamDevice ram(0x1000);
  axi_tb::BufferUartBackend uart_backend;
  axi_tb::UartDevice uart(uart_backend);
  axi_tb::ExitDevice exit;
  axi_tb::AddressSpace space;
  space.map(0x0000, 0x1000, rom, "rom");
  space.map(0x1000, 0x1000, ram, "ram");
  space.map(0x2000, 8, uart, "uart");
  space.map(0x3000, 4, exit, "exit");

  Fabric fabric(space);
  Inputs inputs{};
  tick(fabric, inputs, true);
  tick(fabric, inputs, false);

  // W may be accepted before its AW and must remain associated in port order.
  inputs[0].w_valid = true;
  inputs[0].w = word(0x11223344);
  check(tick(fabric, inputs)[0].w_ready, "W-before-AW is buffered");
  inputs[0].w_valid = false;
  inputs[0].aw_valid = true;
  inputs[0].aw = address(0x1000, 3);
  check(tick(fabric, inputs)[0].aw_ready, "late AW is accepted");
  inputs[0].aw_valid = false;
  consume_b(fabric, inputs, 0, axi_tb::Response::Okay);

  const auto first_read = read_single(fabric, inputs, 0, 0x1000, 3);
  check(first_read.id == 3 && first_read.last,
        "read response preserves ID and LAST");
  check(word_value(first_read) == 0x11223344, "RAM write/read round trip");

  // ROM writes and unmapped reads return protocol responses without mutation.
  write_single(fabric, inputs, 0, 0x0000, 0xaabbccdd);
  consume_b(fabric, inputs, 0, axi_tb::Response::SlaveError);
  const auto missing = read_single(fabric, inputs, 1, 0x4000, 7);
  check(missing.response == axi_tb::Response::DecodeError &&
            word_value(missing) == 0,
        "unmapped read returns DECERR and zero data");

  // Same numeric ID on different ports is routed using {port,id}.
  inputs[0].ar_valid = true;
  inputs[1].ar_valid = true;
  inputs[0].ar = address(0x1000, 5);
  inputs[1].ar = address(0x1000, 5);
  auto outputs = tick(fabric, inputs);
  check(outputs[0].ar_ready && outputs[1].ar_ready,
        "both ingress AR FIFOs accept in one cycle");
  inputs[0].ar_valid = false;
  inputs[1].ar_valid = false;
  outputs = tick(fabric, inputs);
  check(outputs[0].r_valid && outputs[0].r.id == 5,
        "first colliding ID returns to port 0");
  inputs[0].r_ready = true;
  tick(fabric, inputs);
  inputs[0].r_ready = false;
  outputs = tick(fabric, inputs);
  check(outputs[1].r_valid && outputs[1].r.id == 5,
        "second colliding ID returns to port 1");
  inputs[1].r_ready = true;
  tick(fabric, inputs);
  inputs[1].r_ready = false;

  // A matching exclusive read/write succeeds.
  const auto exclusive_read = read_single(fabric, inputs, 0, 0x1004, 9, true);
  check(exclusive_read.response == axi_tb::Response::ExclusiveOkay,
        "exclusive read returns EXOKAY");
  write_single(fabric, inputs, 0, 0x1004, 0x55667788, 9, true);
  consume_b(fabric, inputs, 0, axi_tb::Response::ExclusiveOkay);
  check(word_value(read_single(fabric, inputs, 0, 0x1004)) == 0x55667788,
        "successful exclusive write commits");

  // A conflicting port invalidates the reservation; the failed exclusive
  // write returns OKAY and does not update memory.
  read_single(fabric, inputs, 0, 0x1008, 10, true);
  write_single(fabric, inputs, 1, 0x1008, 0x01020304, 2, false);
  consume_b(fabric, inputs, 1, axi_tb::Response::Okay);
  write_single(fabric, inputs, 0, 0x1008, 0xdeadbeef, 10, true);
  consume_b(fabric, inputs, 0, axi_tb::Response::Okay);
  check(word_value(read_single(fabric, inputs, 0, 0x1008)) == 0x01020304,
        "failed exclusive write has no side effect");

  // Each outstanding Exit response snapshots its own code.  A later accepted
  // Exit write must not change the code observed when the first B handshakes.
  write_single(fabric, inputs, 0, 0x3000, 0x11111111, 11);
  outputs = tick(fabric, inputs);
  check(outputs[0].b_valid && outputs[0].b.id == 11 &&
            outputs[0].b.exit_code == 0x11111111 && !fabric.exit_completed() &&
            exit.requested(),
        "first exit response waits with its code snapshot");
  write_single(fabric, inputs, 0, 0x3000, 0x22222222, 12);
  outputs = tick(fabric, inputs);
  check(outputs[0].b_valid && outputs[0].b.id == 11 &&
            outputs[0].b.exit_code == 0x11111111 && exit.code() == 0x22222222 &&
            !fabric.exit_completed(),
        "later exit write does not overwrite the first B snapshot");
  inputs[0].b_ready = true;
  tick(fabric, inputs);
  inputs[0].b_ready = false;
  check(fabric.exit_completed() && fabric.exit_code() == 0x11111111,
        "exit completion uses the code of the B response that handshook");

  // A malformed WSTRB is a DUT protocol error with useful context.
  bool saw_protocol_error = false;
  try {
    inputs[0].aw_valid = true;
    inputs[0].aw = address(0x1000);
    inputs[0].aw.size = 0;
    inputs[0].w_valid = true;
    inputs[0].w = word(0);
    tick(fabric, inputs);
  } catch (const axi_tb::ProtocolError &error) {
    saw_protocol_error =
        std::string_view(error.what()).find("WSTRB") != std::string_view::npos;
  }
  check(saw_protocol_error, "illegal WSTRB raises contextual protocol error");

  if (failures != 0) {
    std::cerr << failures << " fabric test(s) failed\n";
    return 1;
  }
  std::cout << "host fabric tests passed\n";
  return 0;
}
