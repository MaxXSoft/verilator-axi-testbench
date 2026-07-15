#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

#include "devices.hpp"
#include "fabric.hpp"

namespace {

constexpr std::uint64_t SEED = 0x51a7e5d3c9b28f04ULL;
constexpr std::uint64_t RAM_BASE = 0x80000000ULL;
constexpr std::size_t RAM_SIZE = static_cast<std::size_t>(128) * 1024;
constexpr std::size_t PORT_COUNT = 2;
constexpr std::size_t TRAFFIC_CYCLES = 100'000;
constexpr std::size_t MAX_BEATS = 16;

using Fabric = axi_tb::AxiFabric<PORT_COUNT, 64, 64, 4>;
using Inputs = std::array<Fabric::Master, PORT_COUNT>;

[[noreturn]] void fail(const std::string &message) {
  throw std::runtime_error(message);
}

void require(bool condition, const std::string &message) {
  if (!condition) {
    fail(message);
  }
}

class Random {
 public:
  explicit Random(std::uint64_t seed) : state_(seed) {}

  [[nodiscard]] std::uint64_t next() noexcept {
    state_ ^= state_ << 13U;
    state_ ^= state_ >> 7U;
    state_ ^= state_ << 17U;
    return state_;
  }

  [[nodiscard]] std::size_t below(std::size_t limit) {
    require(limit != 0, "random range must not be empty");
    return static_cast<std::size_t>(next() % limit);
  }

  [[nodiscard]] bool chance(std::size_t numerator, std::size_t denominator) {
    return below(denominator) < numerator;
  }

 private:
  std::uint64_t state_;
};

struct Statistics {
  std::array<std::size_t, PORT_COUNT> port_transactions{};
  std::array<std::size_t, 4> read_sizes{};
  std::array<std::size_t, 4> write_sizes{};
  std::size_t reads = 0;
  std::size_t writes = 0;
  std::size_t partial_write_beats = 0;
  std::size_t unaligned_transactions = 0;
  std::size_t aw_first = 0;
  std::size_t w_first = 0;
  std::size_t simultaneous_aw_w = 0;
  std::size_t aw_stalls = 0;
  std::size_t w_stalls = 0;
  std::size_t ar_stalls = 0;
  std::size_t b_stalls = 0;
  std::size_t r_stalls = 0;
};

struct TransactionShape {
  axi_tb::AddressPayload address{};
  axi_tb::BurstCursor<Fabric::DATA_BYTES> cursor;

  explicit TransactionShape(const axi_tb::AddressPayload &value)
      : address(value), cursor(address) {}
};

TransactionShape random_shape(std::size_t port, Random &random, std::uint8_t id,
                              Statistics &statistics) {
  constexpr std::size_t PARTITION_SIZE = RAM_SIZE / PORT_COUNT;
  constexpr std::size_t PAGES_PER_PARTITION = PARTITION_SIZE / 4096;

  axi_tb::AddressPayload address;
  address.id = id;
  address.size = static_cast<std::uint8_t>(random.below(4));
  const std::size_t beats = 1 + random.below(MAX_BEATS);
  address.length = static_cast<std::uint8_t>(beats - 1);
  address.burst = axi_tb::Burst::Increment;

  const std::size_t beat_bytes = std::size_t{1} << address.size;
  const std::size_t page = random.below(PAGES_PER_PARTITION);
  const std::size_t last_aligned_start = 4096 - beats * beat_bytes;
  const std::size_t aligned_slots = last_aligned_start / beat_bytes + 1;
  const std::size_t aligned_offset = random.below(aligned_slots) * beat_bytes;
  const std::size_t unalignment = random.below(beat_bytes);
  address.address = RAM_BASE + port * PARTITION_SIZE + page * 4096 +
                    aligned_offset + unalignment;
  if (unalignment != 0) {
    ++statistics.unaligned_transactions;
  }

  TransactionShape shape(address);
  shape.cursor.validate_4k_boundary();
  return shape;
}

struct WriteTransaction {
  axi_tb::AddressPayload address{};
  axi_tb::BurstCursor<Fabric::DATA_BYTES> cursor;
  std::array<Fabric::WriteBeat, MAX_BEATS> beats{};
  std::size_t beat_count = 0;
  std::size_t next_beat = 0;
  std::size_t aw_delay = 0;
  std::size_t w_delay = 0;
  bool aw_presented = false;
  bool w_presented = false;
  bool aw_done = false;
  bool saw_aw_handshake = false;
  bool saw_w_handshake = false;

  WriteTransaction(const TransactionShape &shape, Random &random,
                   Statistics &statistics)
      : address(shape.address),
        cursor(address),
        beat_count(cursor.beats()),
        aw_delay(random.below(5)),
        w_delay(random.below(5)) {
    // Independent launch delays deliberately exercise AW-before-W,
    // W-before-AW, and same-cycle acceptance.
    for (std::size_t beat_index = 0; beat_index < beat_count; ++beat_index) {
      auto &beat = beats[beat_index];
      const auto lanes = cursor.lane_mask(beat_index);
      bool all_legal_lanes_enabled = true;
      for (std::size_t lane = 0; lane < Fabric::DATA_BYTES; ++lane) {
        beat.data[lane] = static_cast<std::uint8_t>(random.next());
        if (lanes[lane] != 0) {
          beat.strobe[lane] = random.chance(3, 4) ? 1U : 0U;
          all_legal_lanes_enabled &= beat.strobe[lane] != 0;
        }
      }
      if (!all_legal_lanes_enabled) {
        ++statistics.partial_write_beats;
      }
      beat.last = beat_index + 1 == beat_count;
    }
  }
};

struct ReadTransaction {
  axi_tb::AddressPayload address{};
  axi_tb::BurstCursor<Fabric::DATA_BYTES> cursor;
  std::array<std::array<std::uint8_t, Fabric::DATA_BYTES>, MAX_BEATS>
      expected{};
  std::size_t next_beat = 0;
  std::size_t ar_delay = 0;
  bool ar_presented = false;
  bool ar_done = false;

  ReadTransaction(const TransactionShape &shape,
                  const std::array<std::uint8_t, RAM_SIZE> &golden,
                  Random &random)
      : address(shape.address), cursor(address), ar_delay(random.below(5)) {
    for (std::size_t beat_index = 0; beat_index < cursor.beats();
         ++beat_index) {
      const auto base = cursor.bus_word_address(beat_index);
      const auto lanes = cursor.lane_mask(beat_index);
      for (std::size_t lane = 0; lane < Fabric::DATA_BYTES; ++lane) {
        if (lanes[lane] != 0) {
          expected[beat_index][lane] =
              golden[static_cast<std::size_t>(base + lane - RAM_BASE)];
        }
      }
    }
  }
};

struct PortDriver {
  std::optional<WriteTransaction> write;
  std::optional<ReadTransaction> read;
  std::size_t cooldown = 0;
  std::uint8_t next_id = 0;

  [[nodiscard]] bool idle() const noexcept {
    return !write.has_value() && !read.has_value();
  }
};

class StressHarness {
 public:
  StressHarness() : ram_(RAM_SIZE), fabric_(space_), random_(SEED) {
    space_.map(RAM_BASE, RAM_SIZE, ram_, "stress-ram");
    fabric_.set_seed(SEED ^ 0xd1b54a32d192ed03ULL);
    fabric_.set_stall_probability(0.29);
  }

  void reset() {
    const Inputs inputs{};
    (void)fabric_.drive(true);
    fabric_.commit(inputs, true);
  }

  void step(bool draining) {
    Inputs inputs{};
    for (std::size_t port = 0; port < PORT_COUNT; ++port) {
      maybe_start(port, draining);
      drive_port(port, inputs[port], draining);
    }

    const auto outputs = fabric_.drive(false);
    fabric_.commit(inputs, false);

    for (std::size_t port = 0; port < PORT_COUNT; ++port) {
      finish_cycle(port, inputs[port], outputs[port]);
    }
  }

  [[nodiscard]] bool idle() const noexcept {
    for (const auto &port : ports_) {
      if (!port.idle()) {
        return false;
      }
    }
    return fabric_.idle();
  }

  void verify(std::size_t total_cycles) const {
    require(total_cycles >= TRAFFIC_CYCLES,
            "stress run did not reach 100000 traffic cycles");
    require(idle(), "fabric did not drain after randomized traffic");
    require(statistics_.reads != 0 && statistics_.writes != 0,
            "randomized run did not cover both reads and writes");
    for (std::size_t port = 0; port < PORT_COUNT; ++port) {
      require(statistics_.port_transactions[port] != 0,
              "one AXI port generated no traffic");
    }
    for (std::size_t size = 0; size < 4; ++size) {
      require(statistics_.read_sizes[size] != 0,
              "a read AxSIZE was not covered");
      require(statistics_.write_sizes[size] != 0,
              "a write AxSIZE was not covered");
    }
    require(statistics_.partial_write_beats != 0,
            "partial WSTRB was not covered");
    require(statistics_.unaligned_transactions != 0,
            "unaligned first beats were not covered");
    require(statistics_.aw_first != 0 && statistics_.w_first != 0 &&
                statistics_.simultaneous_aw_w != 0,
            "AW/W acceptance ordering coverage is incomplete");
    require(statistics_.aw_stalls != 0 && statistics_.w_stalls != 0 &&
                statistics_.ar_stalls != 0,
            "request-channel backpressure was not observed");
    require(statistics_.b_stalls != 0 && statistics_.r_stalls != 0,
            "response-channel backpressure was not observed");

    const auto actual = ram_.bytes();
    for (std::size_t index = 0; index < golden_.size(); ++index) {
      const auto byte = std::to_integer<std::uint8_t>(actual[index]);
      if (byte != golden_[index]) {
        std::ostringstream message;
        message << "RAM/golden mismatch at byte " << index << ": got "
                << static_cast<unsigned>(byte) << ", expected "
                << static_cast<unsigned>(golden_[index]);
        fail(message.str());
      }
    }
  }

  [[nodiscard]] const Statistics &statistics() const noexcept {
    return statistics_;
  }

 private:
  void maybe_start(std::size_t port, bool draining) {
    auto &driver = ports_[port];
    if (!driver.idle() || draining) {
      return;
    }
    if (driver.cooldown != 0) {
      --driver.cooldown;
      return;
    }
    if (!random_.chance(1, 3)) {
      return;
    }

    const auto id = driver.next_id++ & 0x0fU;
    auto shape = random_shape(port, random_, id, statistics_);
    ++statistics_.port_transactions[port];
    if (random_.chance(1, 2)) {
      ++statistics_.writes;
      ++statistics_.write_sizes[shape.address.size];
      driver.write.emplace(shape, random_, statistics_);
    } else {
      ++statistics_.reads;
      ++statistics_.read_sizes[shape.address.size];
      driver.read.emplace(shape, golden_, random_);
    }
  }

  void drive_port(std::size_t port, Fabric::Master &input, bool draining) {
    auto &driver = ports_[port];
    if (driver.write) {
      auto &transaction = *driver.write;
      if (!transaction.aw_done &&
          (transaction.aw_presented || transaction.aw_delay == 0)) {
        input.aw_valid = true;
        input.aw = transaction.address;
      }
      if (transaction.next_beat < transaction.beat_count &&
          (transaction.w_presented || transaction.w_delay == 0)) {
        input.w_valid = true;
        input.w = transaction.beats[transaction.next_beat];
      }
      input.b_ready = draining || random_.chance(2, 3);
    } else if (driver.read) {
      auto &transaction = *driver.read;
      if (!transaction.ar_done &&
          (transaction.ar_presented || transaction.ar_delay == 0)) {
        input.ar_valid = true;
        input.ar = transaction.address;
      }
      input.r_ready = draining || random_.chance(2, 3);
    }
  }

  void finish_cycle(std::size_t port, const Fabric::Master &input,
                    const Fabric::Slave &output) {
    auto &driver = ports_[port];
    if (driver.write) {
      finish_write_cycle(port, input, output, driver.write.value());
    } else if (driver.read) {
      finish_read_cycle(port, input, output, driver.read.value());
    }
  }

  void finish_write_cycle(std::size_t port, const Fabric::Master &input,
                          const Fabric::Slave &output,
                          WriteTransaction &transaction) {
    const bool aw_handshake = input.aw_valid && output.aw_ready;
    const bool w_handshake = input.w_valid && output.w_ready;

    if (!transaction.saw_aw_handshake && !transaction.saw_w_handshake) {
      if (aw_handshake && w_handshake) {
        ++statistics_.simultaneous_aw_w;
      } else if (aw_handshake) {
        ++statistics_.aw_first;
      } else if (w_handshake) {
        ++statistics_.w_first;
      }
    }
    transaction.saw_aw_handshake |= aw_handshake;
    transaction.saw_w_handshake |= w_handshake;

    if (input.aw_valid) {
      if (aw_handshake) {
        transaction.aw_done = true;
        transaction.aw_presented = false;
      } else {
        transaction.aw_presented = true;
        ++statistics_.aw_stalls;
      }
    } else if (!transaction.aw_done && transaction.aw_delay != 0) {
      --transaction.aw_delay;
    }

    if (input.w_valid) {
      if (w_handshake) {
        ++transaction.next_beat;
        transaction.w_presented = false;
        transaction.w_delay = random_.below(3);
      } else {
        transaction.w_presented = true;
        ++statistics_.w_stalls;
      }
    } else if (transaction.next_beat < transaction.beat_count &&
               transaction.w_delay != 0) {
      --transaction.w_delay;
    }

    if (!output.b_valid) {
      return;
    }
    require(output.b.id == transaction.address.id,
            "write response ID mismatch");
    require(output.b.response == axi_tb::Response::Okay,
            "RAM write returned a non-OKAY response");
    if (!input.b_ready) {
      ++statistics_.b_stalls;
      return;
    }

    apply_write(transaction);
    ports_[port].write.reset();
    ports_[port].cooldown = random_.below(4);
  }

  void finish_read_cycle(std::size_t port, const Fabric::Master &input,
                         const Fabric::Slave &output,
                         ReadTransaction &transaction) {
    if (input.ar_valid) {
      if (output.ar_ready) {
        transaction.ar_done = true;
        transaction.ar_presented = false;
      } else {
        transaction.ar_presented = true;
        ++statistics_.ar_stalls;
      }
    } else if (!transaction.ar_done && transaction.ar_delay != 0) {
      --transaction.ar_delay;
    }

    if (!output.r_valid) {
      return;
    }
    require(output.r.id == transaction.address.id, "read response ID mismatch");
    require(output.r.response == axi_tb::Response::Okay,
            "RAM read returned a non-OKAY response");
    require(output.r.last ==
                (transaction.next_beat + 1 == transaction.cursor.beats()),
            "read response RLAST mismatch");
    if (!input.r_ready) {
      ++statistics_.r_stalls;
      return;
    }

    const auto &expected = transaction.expected[transaction.next_beat];
    for (std::size_t lane = 0; lane < Fabric::DATA_BYTES; ++lane) {
      if (output.r.data[lane] != expected[lane]) {
        std::ostringstream message;
        message << "read data mismatch on port " << port << ", beat "
                << transaction.next_beat << ", lane " << lane;
        fail(message.str());
      }
    }
    ++transaction.next_beat;
    if (transaction.next_beat == transaction.cursor.beats()) {
      ports_[port].read.reset();
      ports_[port].cooldown = random_.below(4);
    }
  }

  void apply_write(const WriteTransaction &transaction) {
    for (std::size_t beat_index = 0; beat_index < transaction.beat_count;
         ++beat_index) {
      const auto base = transaction.cursor.bus_word_address(beat_index);
      const auto lanes = transaction.cursor.lane_mask(beat_index);
      const auto &beat = transaction.beats[beat_index];
      for (std::size_t lane = 0; lane < Fabric::DATA_BYTES; ++lane) {
        if (lanes[lane] != 0 && beat.strobe[lane] != 0) {
          golden_[static_cast<std::size_t>(base + lane - RAM_BASE)] =
              beat.data[lane];
        }
      }
    }
  }

  axi_tb::RamDevice ram_;
  axi_tb::AddressSpace space_;
  Fabric fabric_;
  Random random_;
  std::array<std::uint8_t, RAM_SIZE> golden_{};
  std::array<PortDriver, PORT_COUNT> ports_{};
  Statistics statistics_{};
};

}  // namespace

int main() {
  try {
    StressHarness harness;
    harness.reset();

    for (std::size_t cycle = 0; cycle < TRAFFIC_CYCLES; ++cycle) {
      harness.step(false);
    }

    constexpr std::size_t MAX_DRAIN_CYCLES = 10'000;
    std::size_t drain_cycles = 0;
    while (!harness.idle() && drain_cycles < MAX_DRAIN_CYCLES) {
      harness.step(true);
      ++drain_cycles;
    }
    harness.verify(TRAFFIC_CYCLES + drain_cycles);

    const auto &statistics = harness.statistics();
    std::cout << "fabric differential stress passed: seed=" << SEED
              << ", traffic_cycles=" << TRAFFIC_CYCLES
              << ", drain_cycles=" << drain_cycles
              << ", reads=" << statistics.reads
              << ", writes=" << statistics.writes << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "fabric differential stress failed: " << error.what() << '\n';
    return 1;
  }
}
