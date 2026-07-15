#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "axi.hpp"
#include "packed.hpp"

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

template <typename Function>
void check_protocol_error(Function &&function, std::string_view message) {
  try {
    function();
    check(false, message);
  } catch (const axi_tb::ProtocolError &) {
  }
}

void test_ring_buffer() {
  axi_tb::RingBuffer<2, int> queue;
  check(queue.empty(), "new queue is empty");
  check(queue.push(4), "first push succeeds");
  check(queue.push(5), "second push succeeds");
  check(!queue.push(6), "full queue rejects push");
  check(queue.front() == 4, "queue preserves order");
  queue.pop();
  check(queue.front() == 5, "queue advances head");
  check(queue.push(7), "wrapped push succeeds");
  queue.pop();
  check(queue.front() == 7, "wrapped value is visible");
}

void test_bursts() {
  axi_tb::AddressPayload address{};
  address.address = 0x102;
  address.length = 3;
  address.size = 2;
  address.burst = axi_tb::Burst::increment;
  axi_tb::BurstCursor<8> increment(address);
  check(increment.beat_address(0) == 0x102, "unaligned INCR first address");
  check(increment.beat_address(1) == 0x104,
        "unaligned INCR aligns second beat");
  check(increment.beat_address(3) == 0x10c, "unaligned INCR last address");
  const auto first_mask = increment.lane_mask(0);
  check(first_mask[2] && first_mask[3] && !first_mask[1] && !first_mask[4],
        "unaligned first beat uses only remaining size window lanes");

  address.address = 0x11c;
  address.length = 3;
  address.size = 2;
  address.burst = axi_tb::Burst::wrap;
  axi_tb::BurstCursor<8> wrap(address);
  check(wrap.beat_address(0) == 0x11c, "WRAP first address");
  check(wrap.beat_address(1) == 0x110, "WRAP wraps at boundary");
  check(wrap.beat_address(2) == 0x114, "WRAP advances after wrap");

  address.address = 0x200;
  address.length = 15;
  address.burst = axi_tb::Burst::fixed;
  axi_tb::BurstCursor<8> fixed(address);
  check(fixed.beat_address(15) == 0x200, "FIXED address does not advance");

  address.length = 16;
  check_protocol_error([&] { axi_tb::BurstCursor<8> invalid(address); },
                       "FIXED burst longer than 16 is rejected");

  address.address = 0xffc;
  address.length = 1;
  address.size = 2;
  address.burst = axi_tb::Burst::increment;
  check_protocol_error(
      [&] { axi_tb::BurstCursor<8>(address).validate_4k_boundary(); },
      "4 KiB crossing is rejected");

  address.address = 0;
  address.length = 0;
  address.size = 7;
  check_protocol_error([&] { axi_tb::BurstCursor<8> invalid(address); },
                       "oversized AxSIZE is rejected");

  address.address = 0x3000;
  address.length = 255;
  address.size = 0;
  address.burst = axi_tb::Burst::increment;
  axi_tb::BurstCursor<8> maximum(address);
  maximum.validate_4k_boundary();
  check(maximum.beats() == 256 && maximum.beat_address(255) == 0x30ff,
        "full 256-beat AXI4 burst is supported");
}

void test_packed_access() {
  std::uint64_t scalar = 0;
  axi_tb::packed::write_u64(scalar, 12, 40, 0xabcde12345ULL);
  check(axi_tb::packed::read_u64(scalar, 12, 40) == 0xabcde12345ULL,
        "packed scalar cross-word access");

  std::array<std::uint32_t, 4> wide{};
  axi_tb::packed::write_u64(wide, 61, 64, 0xfedcba9876543210ULL);
  check(axi_tb::packed::read_u64(wide, 61, 64) == 0xfedcba9876543210ULL,
        "packed wide unaligned access");

  const std::array<std::uint8_t, 8> bytes{0, 1, 2, 3, 4, 5, 6, 7};
  axi_tb::packed::write_bytes(wide, 17, bytes);
  check(axi_tb::packed::read_bytes<8>(wide, 17) == bytes,
        "packed byte array round trip");
}

}  // namespace

int main() {
  test_ring_buffer();
  test_bursts();
  test_packed_access();
  if (failures != 0) {
    std::cerr << failures << " host AXI test(s) failed\n";
    return 1;
  }
  std::cout << "host AXI tests passed\n";
  return 0;
}
