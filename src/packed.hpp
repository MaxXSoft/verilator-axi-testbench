#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

namespace axi_tb::packed {

template <typename Signal>
[[nodiscard]] std::uint32_t word(const Signal &signal, std::size_t index) {
  using Value = std::remove_cvref_t<Signal>;
  if constexpr (std::is_integral_v<Value>) {
    constexpr std::size_t bits = sizeof(Value) * 8U;
    if (index * 32U >= bits) {
      return 0;
    }
    using Unsigned = std::make_unsigned_t<Value>;
    return static_cast<std::uint32_t>(static_cast<Unsigned>(signal) >>
                                      (index * 32U));
  } else {
    return static_cast<std::uint32_t>(signal[index]);
  }
}

template <typename Signal>
void set_word(Signal &signal, std::size_t index, std::uint32_t value) {
  using Value = std::remove_cvref_t<Signal>;
  if constexpr (std::is_integral_v<Value>) {
    constexpr std::size_t bits = sizeof(Value) * 8U;
    if (index * 32U >= bits) {
      throw std::out_of_range("packed signal word index");
    }
    using Unsigned = std::make_unsigned_t<Value>;
    Unsigned current = static_cast<Unsigned>(signal);
    const std::size_t shift = index * 32U;
    const Unsigned mask = static_cast<Unsigned>(UINT32_MAX) << shift;
    current = (current & ~mask) | (static_cast<Unsigned>(value) << shift);
    signal = static_cast<Value>(current);
  } else {
    signal[index] = value;
  }
}

template <typename Signal>
[[nodiscard]] bool bit(const Signal &signal, std::size_t index) {
  return ((word(signal, index / 32U) >> (index % 32U)) & 1U) != 0U;
}

template <typename Signal>
void set_bit(Signal &signal, std::size_t index, bool value) {
  const auto word_index = index / 32U;
  auto current = word(signal, word_index);
  const auto mask = std::uint32_t{1} << (index % 32U);
  current = value ? current | mask : current & ~mask;
  set_word(signal, word_index, current);
}

template <typename Signal>
[[nodiscard]] std::uint64_t read_u64(const Signal &signal, std::size_t lsb,
                                     std::size_t width) {
  if (width > 64U) {
    throw std::invalid_argument("read_u64 width exceeds 64 bits");
  }
  std::uint64_t result = 0;
  std::size_t copied = 0;
  while (copied < width) {
    const std::size_t source_bit = lsb + copied;
    const std::size_t offset = source_bit % 32U;
    const std::size_t count =
        std::min<std::size_t>(width - copied, 32U - offset);
    const std::uint64_t mask =
        count == 32U ? UINT32_MAX : ((std::uint64_t{1} << count) - 1U);
    const std::uint64_t part =
        (word(signal, source_bit / 32U) >> offset) & mask;
    result |= part << copied;
    copied += count;
  }
  return result;
}

template <typename Signal>
void write_u64(Signal &signal, std::size_t lsb, std::size_t width,
               std::uint64_t value) {
  if (width > 64U) {
    throw std::invalid_argument("write_u64 width exceeds 64 bits");
  }
  std::size_t copied = 0;
  while (copied < width) {
    const std::size_t destination_bit = lsb + copied;
    const std::size_t offset = destination_bit % 32U;
    const std::size_t count =
        std::min<std::size_t>(width - copied, 32U - offset);
    const std::uint32_t low_mask =
        count == 32U
            ? UINT32_MAX
            : static_cast<std::uint32_t>((std::uint64_t{1} << count) - 1U);
    const std::uint32_t mask = low_mask << offset;
    auto current = word(signal, destination_bit / 32U);
    current = (current & ~mask) |
              ((static_cast<std::uint32_t>(value >> copied) << offset) & mask);
    set_word(signal, destination_bit / 32U, current);
    copied += count;
  }
}

template <std::size_t Size, typename Signal>
[[nodiscard]] std::array<std::uint8_t, Size> read_bytes(const Signal &signal,
                                                        std::size_t lsb) {
  std::array<std::uint8_t, Size> result{};
  for (std::size_t i = 0; i < Size; ++i) {
    result[i] = static_cast<std::uint8_t>(read_u64(signal, lsb + i * 8U, 8));
  }
  return result;
}

template <std::size_t Size, typename Signal>
void write_bytes(Signal &signal, std::size_t lsb,
                 const std::array<std::uint8_t, Size> &value) {
  for (std::size_t i = 0; i < Size; ++i) {
    write_u64(signal, lsb + i * 8U, 8, value[i]);
  }
}

}  // namespace axi_tb::packed
