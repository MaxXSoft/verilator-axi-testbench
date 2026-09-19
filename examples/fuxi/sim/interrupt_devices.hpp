#pragma once

#include <array>
#include <vector>

#include "device.hpp"

namespace fuxi_sim {

// Single-hart SiFive-compatible CLINT register layout. mtime uses simulation
// cycles; no dependency on host speed. RV32 accesses both halves independently.
class Clint final : public axi_tb::Device {
 public:
  explicit Clint(std::uint64_t divider = 1);
  void reset() noexcept override;
  void tick() override;
  void settle() noexcept override;
  [[nodiscard]] const axi_tb::Signal *output(
      std::string_view name) const noexcept override;

 private:
  axi_tb::Response read_impl(std::uint64_t offset, std::span<std::byte> data,
                             std::span<const std::uint8_t> lanes) override;
  axi_tb::Response write_impl(std::uint64_t offset,
                              std::span<const std::byte> data,
                              std::span<const std::uint8_t> lanes) override;
  std::uint64_t divider_, phase_ = 0, compare_ = UINT64_MAX;
  axi_tb::Signal time_{.width = 64, .value = 0}, timer_, software_;
};

// Standard PLIC layout, level-sensitive gateways, priority 0..7.
// Context 0 is hart 0 M-mode; context 1 is hart 0 S-mode in the Fuxi platform.
class Plic final : public axi_tb::Device {
 public:
  explicit Plic(unsigned sources = 31, unsigned contexts = 2);
  void reset() noexcept override;
  void settle() noexcept override;
  [[nodiscard]] const axi_tb::Signal *output(
      std::string_view name) const noexcept override;
  axi_tb::InputSignal *input(std::string_view name) noexcept override;

 private:
  axi_tb::Response read_impl(std::uint64_t offset, std::span<std::byte> data,
                             std::span<const std::uint8_t> lanes) override;
  axi_tb::Response write_impl(std::uint64_t offset,
                              std::span<const std::byte> data,
                              std::span<const std::uint8_t> lanes) override;
  [[nodiscard]] unsigned best(unsigned context) const noexcept;
  [[nodiscard]] bool enabled(unsigned context, unsigned source) const noexcept;
  void notify() noexcept;
  std::uint32_t read_register(std::uint64_t offset);
  void write_register(std::uint64_t offset, std::uint32_t value);
  unsigned sources_, contexts_;
  std::vector<axi_tb::InputSignal> inputs_;
  std::vector<axi_tb::Signal> irqs_;
  std::vector<std::uint32_t> priority_, threshold_;
  std::vector<std::array<std::uint32_t, 32>> enables_;
  std::vector<bool> pending_, busy_;
};
}  // namespace fuxi_sim
