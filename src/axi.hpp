#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace axi_tb {

enum class Response : std::uint8_t {
  okay = 0,
  exclusive_okay = 1,
  slave_error = 2,
  decode_error = 3,
};

enum class Burst : std::uint8_t {
  fixed = 0,
  increment = 1,
  wrap = 2,
  reserved = 3,
};

class ProtocolError final : public std::runtime_error {
 public:
  explicit ProtocolError(const std::string &message)
      : std::runtime_error(message) {}
};

struct AddressPayload {
  std::uint64_t id = 0;
  std::uint64_t address = 0;
  std::uint8_t length = 0;
  std::uint8_t size = 0;
  Burst burst = Burst::increment;
  bool lock = false;
  std::uint8_t cache = 0;
  std::uint8_t protection = 0;

  friend bool operator==(const AddressPayload &,
                         const AddressPayload &) = default;
};

template <std::size_t DataBytes>
struct WriteDataPayload {
  static_assert(DataBytes > 0);
  std::array<std::uint8_t, DataBytes> data{};
  std::array<std::uint8_t, DataBytes> strobe{};
  bool last = false;

  friend bool operator==(const WriteDataPayload &,
                         const WriteDataPayload &) = default;
};

template <std::size_t DataBytes>
struct ReadDataPayload {
  std::uint64_t id = 0;
  std::array<std::uint8_t, DataBytes> data{};
  Response response = Response::okay;
  bool last = false;
};

struct WriteResponsePayload {
  std::uint64_t id = 0;
  Response response = Response::okay;
  bool exit_response = false;
  std::uint32_t exit_code = 0;
};

template <std::size_t DataBytes>
struct MasterSignals {
  bool aw_valid = false;
  AddressPayload aw{};
  bool w_valid = false;
  WriteDataPayload<DataBytes> w{};
  bool b_ready = false;
  bool ar_valid = false;
  AddressPayload ar{};
  bool r_ready = false;
};

template <std::size_t DataBytes>
struct SlaveSignals {
  bool aw_ready = false;
  bool w_ready = false;
  bool b_valid = false;
  WriteResponsePayload b{};
  bool ar_ready = false;
  bool r_valid = false;
  ReadDataPayload<DataBytes> r{};
};

constexpr Response combine_response(Response lhs, Response rhs) noexcept {
  if (lhs == Response::decode_error || rhs == Response::decode_error) {
    return Response::decode_error;
  }
  if (lhs == Response::slave_error || rhs == Response::slave_error) {
    return Response::slave_error;
  }
  if (lhs == Response::exclusive_okay || rhs == Response::exclusive_okay) {
    return Response::exclusive_okay;
  }
  return Response::okay;
}

template <std::size_t Capacity, typename T>
class RingBuffer {
 public:
  static_assert(Capacity > 0);

  [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] constexpr bool full() const noexcept {
    return size_ == Capacity;
  }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
  [[nodiscard]] static constexpr std::size_t capacity() noexcept {
    return Capacity;
  }

  constexpr T &front() {
    if (empty()) {
      throw std::logic_error("front() on empty RingBuffer");
    }
    return values_[head_];
  }
  constexpr const T &front() const {
    if (empty()) {
      throw std::logic_error("front() on empty RingBuffer");
    }
    return values_[head_];
  }

  constexpr bool push(const T &value) {
    if (full()) {
      return false;
    }
    values_[(head_ + size_) % Capacity] = value;
    ++size_;
    return true;
  }

  constexpr bool push(T &&value) {
    if (full()) {
      return false;
    }
    values_[(head_ + size_) % Capacity] = std::move(value);
    ++size_;
    return true;
  }

  constexpr void pop() {
    if (empty()) {
      throw std::logic_error("pop() on empty RingBuffer");
    }
    head_ = (head_ + 1) % Capacity;
    --size_;
  }

  constexpr void clear() noexcept {
    head_ = 0;
    size_ = 0;
  }

 private:
  std::array<T, Capacity> values_{};
  std::size_t head_ = 0;
  std::size_t size_ = 0;
};

template <std::size_t DataBytes>
class BurstCursor {
 public:
  explicit BurstCursor(const AddressPayload &payload)
      : payload_(payload),
        beats_(static_cast<std::size_t>(payload.length) + 1U),
        beat_bytes_(checked_beat_bytes(payload.size)) {
    static_assert(DataBytes > 0 && (DataBytes & (DataBytes - 1)) == 0);
    validate_shape();
  }

  [[nodiscard]] std::size_t beats() const noexcept { return beats_; }
  [[nodiscard]] std::size_t beat_bytes() const noexcept { return beat_bytes_; }

  [[nodiscard]] std::uint64_t beat_address(std::size_t index) const {
    if (index >= beats_) {
      throw std::out_of_range("AXI beat index out of range");
    }
    using Wide = unsigned __int128;
    const Wide start = payload_.address;
    const Wide aligned = start & ~Wide(beat_bytes_ - 1U);
    Wide result = start;
    switch (payload_.burst) {
      case Burst::fixed:
        result = start;
        break;
      case Burst::increment:
        result = index == 0 ? start : aligned + Wide(index) * beat_bytes_;
        break;
      case Burst::wrap: {
        const Wide boundary = Wide(beats_) * beat_bytes_;
        const Wide base = start & ~(boundary - 1U);
        result = base + ((start - base + Wide(index) * beat_bytes_) % boundary);
        break;
      }
      case Burst::reserved:
        throw ProtocolError("reserved AXI burst type");
    }
    if (result > UINT64_MAX) {
      throw ProtocolError("AXI address arithmetic overflow");
    }
    return static_cast<std::uint64_t>(result);
  }

  [[nodiscard]] std::array<std::uint8_t, DataBytes> lane_mask(
      std::size_t index) const {
    const std::uint64_t address = beat_address(index);
    const std::uint64_t aligned_to_size = address & ~(beat_bytes_ - 1U);
    const std::uint64_t first = address;
    const std::uint64_t last = aligned_to_size + beat_bytes_;
    const std::uint64_t bus_base = address & ~(DataBytes - 1U);
    std::array<std::uint8_t, DataBytes> mask{};
    for (std::size_t lane = 0; lane < DataBytes; ++lane) {
      const std::uint64_t byte_address = bus_base + lane;
      mask[lane] = byte_address >= first && byte_address < last ? 1U : 0U;
    }
    return mask;
  }

  [[nodiscard]] std::uint64_t bus_word_address(std::size_t index) const {
    return beat_address(index) & ~(std::uint64_t(DataBytes) - 1U);
  }

  void validate_4k_boundary() const {
    const std::uint64_t page = payload_.address >> 12U;
    for (std::size_t beat = 0; beat < beats_; ++beat) {
      const auto base = bus_word_address(beat);
      const auto mask = lane_mask(beat);
      for (std::size_t lane = 0; lane < DataBytes; ++lane) {
        if (mask[lane] != 0 && ((base + lane) >> 12U) != page) {
          throw ProtocolError("AXI burst crosses a 4 KiB boundary");
        }
      }
    }
  }

 private:
  static std::size_t checked_beat_bytes(std::uint8_t size) {
    if (size >= sizeof(std::size_t) * 8U) {
      throw ProtocolError("AXI AxSIZE is not representable");
    }
    return std::size_t{1} << size;
  }

  void validate_shape() const {
    if (beat_bytes_ > DataBytes) {
      throw ProtocolError("AXI AxSIZE exceeds the data bus width");
    }
    if (payload_.burst == Burst::reserved) {
      throw ProtocolError("reserved AXI burst type");
    }
    if (payload_.burst == Burst::fixed && beats_ > 16) {
      throw ProtocolError("AXI FIXED burst contains more than 16 beats");
    }
    if (payload_.burst == Burst::wrap) {
      if (!(beats_ == 2 || beats_ == 4 || beats_ == 8 || beats_ == 16)) {
        throw ProtocolError("AXI WRAP burst length is not 2, 4, 8, or 16");
      }
      if ((payload_.address & (beat_bytes_ - 1U)) != 0) {
        throw ProtocolError("AXI WRAP burst start is not beat-aligned");
      }
    }
  }

  AddressPayload payload_{};
  std::size_t beats_ = 0;
  std::size_t beat_bytes_ = 0;
};

}  // namespace axi_tb
