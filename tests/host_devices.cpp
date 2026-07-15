#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "devices.hpp"

namespace {

using axi_tb::AddressSpace;
using axi_tb::BufferUartBackend;
using axi_tb::ExitDevice;
using axi_tb::FileUartBackend;
using axi_tb::RamDevice;
using axi_tb::Response;
using axi_tb::RomDevice;
using axi_tb::UartDevice;

static_assert(!std::is_copy_constructible_v<axi_tb::Device>);
static_assert(!std::is_move_constructible_v<axi_tb::Device>);
static_assert(std::is_abstract_v<axi_tb::Device>);
static_assert(!std::is_copy_constructible_v<UartDevice>);
static_assert(!std::is_move_constructible_v<UartDevice>);

template <std::size_t Size>
std::array<std::byte, Size> byte_array(
    const std::array<std::uint8_t, Size> &values) {
  std::array<std::byte, Size> result{};
  for (std::size_t index = 0; index < Size; ++index) {
    result[index] = static_cast<std::byte>(values[index]);
  }
  return result;
}

// This integration-style test intentionally exercises many device paths.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void test_address_space_and_memories() {
  RomDevice rom(16);
  RamDevice ram(std::size_t{128} * 1024U * 1024U);
  BufferUartBackend uart_backend;
  UartDevice uart(uart_backend);
  ExitDevice exit;

  AddressSpace space;
  space.map(0x0000, 16, rom, "rom");
  space.map(0x1000, ram.size(), ram, "ram");
  space.map(0x10000000, 0x100, uart, "uart");
  space.map(0x10001000, 4, exit, "exit");

  assert(space.mappings().size() == 4);
  assert(space.resolve(0, 16) != nullptr);
  assert(space.resolve(15, 1) != nullptr);
  assert(space.resolve(15, 2) == nullptr);
  assert(space.resolve(0x10000000, 0x100) != nullptr);
  assert(space.resolve(0x10000000, 0x101) == nullptr);
  assert(space.resolve(0xdeadbeef, 1) == nullptr);

  bool overlap_was_rejected = false;
  try {
    space.map(8, 16, ram, "overlap");
  } catch (const std::invalid_argument &) {
    overlap_was_rejected = true;
  }
  assert(overlap_was_rejected);

  const auto IMAGE = byte_array<4>({0x11, 0x22, 0x33, 0x44});
  assert(rom.load(4, IMAGE) == Response::Okay);
  std::array<std::byte, 4> read_data{std::byte{0xff}, std::byte{0xff},
                                     std::byte{0xff}, std::byte{0xff}};
  const std::array<std::uint8_t, 4> SPARSE_ENABLE{1, 0, 1, 0};
  assert(space.read(4, read_data, SPARSE_ENABLE) == Response::Okay);
  assert(read_data[0] == std::byte{0x11});
  assert(read_data[1] == std::byte{0});
  assert(read_data[2] == std::byte{0x33});
  assert(read_data[3] == std::byte{0});

  const std::array<std::uint8_t, 4> ALL_LANES{1, 1, 1, 1};
  assert(space.write(4, IMAGE, ALL_LANES) == Response::SlaveError);
  assert(space.read(4, read_data, ALL_LANES) == Response::Okay);
  assert(read_data == IMAGE);

  read_data.fill(std::byte{0xcc});
  assert(space.read(14, read_data, ALL_LANES) == Response::SlaveError);
  for (const std::byte value : read_data) {
    assert(value == std::byte{0});
  }
  assert(space.read(0x900, read_data, ALL_LANES) == Response::DecodeError);

  const auto RAM_WRITE = byte_array<4>({0xaa, 0xbb, 0xcc, 0xdd});
  assert(space.write(0x1000, RAM_WRITE, SPARSE_ENABLE) == Response::Okay);
  assert(space.read(0x1000, read_data, ALL_LANES) == Response::Okay);
  assert(read_data[0] == std::byte{0xaa});
  assert(read_data[1] == std::byte{0});
  assert(read_data[2] == std::byte{0xcc});
  assert(read_data[3] == std::byte{0});

  const std::array<std::byte, 1> LAST_BYTE{std::byte{0x5a}};
  const std::array<std::uint8_t, 1> ONE_LANE{1};
  const std::uint64_t last_address = 0x1000 + ram.size() - 1;
  assert(space.write(last_address, LAST_BYTE, ONE_LANE) == Response::Okay);
  std::array<std::byte, 1> last_read{};
  assert(space.read(last_address, last_read, ONE_LANE) == Response::Okay);
  assert(last_read[0] == std::byte{0x5a});

  assert(rom.supports_burst());
  assert(ram.supports_burst());
  assert(ram.supports_exclusive());
  assert(!rom.supports_exclusive());
  assert(!uart.supports_burst());
  assert(exit.is_exit());

  bool size_mismatch_was_rejected = false;
  try {
    (void)space.read(0, read_data, ONE_LANE);
  } catch (const std::invalid_argument &) {
    size_mismatch_was_rejected = true;
  }
  assert(size_mismatch_was_rejected);
}

// Keeping the complete UART register scenario together makes failures easier
// to localize than splitting it into state-dependent test fragments.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void test_uart() {
  BufferUartBackend backend;
  UartDevice uart(backend);
  const std::array<std::uint8_t, 1> ENABLED{1};
  const std::array<std::uint8_t, 1> DISABLED{0};
  std::array<std::byte, 1> value{};

  assert(uart.read(5, value, ENABLED) == Response::Okay);
  assert(std::to_integer<std::uint8_t>(value[0]) == 0x60);
  assert(uart.read(2, value, ENABLED) == Response::Okay);
  assert(std::to_integer<std::uint8_t>(value[0]) == 0x01);

  backend.push_input(static_cast<std::uint8_t>('A'));
  backend.push_input(static_cast<std::uint8_t>('B'));
  assert(uart.read(5, value, ENABLED) == Response::Okay);
  assert((std::to_integer<std::uint8_t>(value[0]) & 0x01U) != 0);
  assert(uart.read(0, value, ENABLED) == Response::Okay);
  assert(value[0] == std::byte{'A'});
  assert(uart.read(5, value, ENABLED) == Response::Okay);
  assert((std::to_integer<std::uint8_t>(value[0]) & 0x01U) != 0);

  const std::array<std::byte, 1> ENABLE_RX_INTERRUPT{std::byte{1}};
  assert(uart.write(1, ENABLE_RX_INTERRUPT, ENABLED) == Response::Okay);
  assert(uart.read(2, value, ENABLED) == Response::Okay);
  assert((std::to_integer<std::uint8_t>(value[0]) & 0x0fU) == 0x04U);
  assert(uart.read(0, value, ENABLED) == Response::Okay);
  assert(value[0] == std::byte{'B'});

  const std::array<std::byte, 1> SET_DLAB{std::byte{0x80}};
  const std::array<std::byte, 1> DIVISOR_LOW{std::byte{0x34}};
  const std::array<std::byte, 1> DIVISOR_HIGH{std::byte{0x12}};
  assert(uart.write(3, SET_DLAB, ENABLED) == Response::Okay);
  assert(uart.write(0, DIVISOR_LOW, ENABLED) == Response::Okay);
  assert(uart.write(1, DIVISOR_HIGH, ENABLED) == Response::Okay);
  assert(uart.read(0, value, ENABLED) == Response::Okay);
  assert(value[0] == std::byte{0x34});
  assert(uart.read(1, value, ENABLED) == Response::Okay);
  assert(value[0] == std::byte{0x12});

  const std::array<std::byte, 1> CLEAR_DLAB{std::byte{0x03}};
  const std::array<std::byte, 1> OUTPUT_X{std::byte{'X'}};
  assert(uart.write(3, CLEAR_DLAB, ENABLED) == Response::Okay);
  assert(uart.write(0, OUTPUT_X, DISABLED) == Response::Okay);
  assert(backend.output().empty());
  assert(uart.write(0, OUTPUT_X, ENABLED) == Response::Okay);
  assert(backend.output().size() == 1);
  assert(backend.output()[0] == std::byte{'X'});

  value[0] = std::byte{0xff};
  assert(uart.read(0, value, DISABLED) == Response::Okay);
  assert(value[0] == std::byte{0});
  assert(uart.read(8, value, ENABLED) == Response::SlaveError);

  uart.reset();
  assert(uart.read(3, value, ENABLED) == Response::Okay);
  assert(value[0] == std::byte{0});
}

void test_file_uart_backend() {
  // The raw handles are non-owning views closed explicitly at the end of this
  // focused C stdio interoperability test.
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
  std::FILE *input = std::tmpfile();
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
  std::FILE *output = std::tmpfile();
  assert(input != nullptr);
  assert(output != nullptr);
  std::fputc('Q', input);
  std::fflush(input);
  assert(std::fseek(input, 0, SEEK_SET) == 0);

  {
    FileUartBackend backend(input, output);
    std::uint8_t input_byte = 0;
    assert(backend.try_read(input_byte));
    assert(input_byte == static_cast<std::uint8_t>('Q'));
    assert(!backend.try_read(input_byte));
    backend.write(static_cast<std::uint8_t>('Z'));
    backend.flush();
  }

  assert(std::fseek(output, 0, SEEK_SET) == 0);
  assert(std::fgetc(output) == 'Z');
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
  std::fclose(input);
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
  std::fclose(output);
}

void test_exit_device() {
  ExitDevice exit;
  const auto CODE = byte_array<4>({0xef, 0xbe, 0xad, 0xde});
  const std::array<std::uint8_t, 4> PARTIAL{1, 1, 1, 0};
  const std::array<std::uint8_t, 4> FULL{1, 1, 1, 1};

  assert(exit.write(0, CODE, PARTIAL) == Response::SlaveError);
  assert(!exit.requested());
  assert(exit.write(0, std::span(CODE).first(1), std::span(FULL).first(1)) ==
         Response::SlaveError);
  assert(!exit.requested());
  assert(exit.write(0, CODE, FULL) == Response::Okay);
  assert(exit.requested());
  assert(exit.code() == 0xdeadbeefU);
  assert(exit.generation() == 1);

  std::array<std::byte, 4> readback{};
  assert(exit.read(0, readback, FULL) == Response::Okay);
  assert(readback == CODE);
  exit.clear();
  assert(!exit.requested());
  assert(exit.code() == 0);
  assert(exit.generation() == 1);

  AddressSpace space;
  space.map(0x2000, 4, exit, "exit");
  assert(space.write(0x2000, CODE, FULL) == Response::Okay);
  assert(exit.requested());
  space.reset();
  assert(!exit.requested());
}

}  // namespace

// Test assertions and helpers intentionally report failures by throwing.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
  test_address_space_and_memories();
  test_uart();
  test_file_uart_backend();
  test_exit_device();
  return 0;
}
