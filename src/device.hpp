#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "axi.hpp"

namespace axi_tb {

class Device;

// Fixed, allocation-free type erasure for the per-beat device path.  Device
// setup and image loading are cold paths and may remain virtual, but AXI reads
// and writes dispatch through these plain function pointers.
struct DeviceOperations {
  using Read = Response (*)(Device &, std::uint64_t, std::span<std::byte>,
                            std::span<const std::uint8_t>);
  using Write = Response (*)(Device &, std::uint64_t,
                             std::span<const std::byte>,
                             std::span<const std::uint8_t>);
  using ExitCode = std::uint32_t (*)(const Device &) noexcept;

  Read read = nullptr;
  Write write = nullptr;
  ExitCode exit_code = nullptr;
  bool supports_exclusive = false;
  bool supports_burst = false;
  bool is_exit = false;
};

// Device accesses are beat-wide.  The enable/strobe span has one entry per
// byte in data; a zero entry suppresses that lane.  Keeping a whole beat in a
// single call is important for MMIO devices, where splitting a write into byte
// calls could repeat a side effect.
class Device {
 public:
  Device(const Device &) = delete;
  Device &operator=(const Device &) = delete;
  Device(Device &&) = delete;
  Device &operator=(Device &&) = delete;
  virtual ~Device() = default;

  [[nodiscard]] Response read(std::uint64_t offset, std::span<std::byte> data,
                              std::span<const std::uint8_t> enable) {
    if (data.size() != enable.size()) {
      throw std::invalid_argument("device read data/enable size mismatch");
    }
    std::ranges::fill(data, std::byte{0});
    if (data.empty()) {
      return Response::okay;
    }
    return operations_->read(*this, offset, data, enable);
  }

  [[nodiscard]] Response write(std::uint64_t offset,
                               std::span<const std::byte> data,
                               std::span<const std::uint8_t> strobe) {
    if (data.size() != strobe.size()) {
      throw std::invalid_argument("device write data/strobe size mismatch");
    }
    if (data.empty()) {
      return Response::okay;
    }
    return operations_->write(*this, offset, data, strobe);
  }

  // Host-side image initialization bypasses runtime access permissions.  It
  // is deliberately separate from write(), so a ROM stays read-only to AXI.
  [[nodiscard]] virtual Response load(std::uint64_t offset,
                                      std::span<const std::byte> data) {
    (void)offset;
    (void)data;
    return Response::slave_error;
  }

  [[nodiscard]] virtual bool loadable() const noexcept { return false; }
  // A loadable device must report whether the complete host initialization
  // range is valid.  ELF validation uses this before applying any segment, so
  // an out-of-range later segment cannot leave an earlier one partially loaded.
  [[nodiscard]] virtual bool can_load(std::uint64_t offset,
                                      std::uint64_t size) const noexcept {
    (void)offset;
    (void)size;
    return false;
  }
  [[nodiscard]] bool supports_exclusive() const noexcept {
    return operations_->supports_exclusive;
  }
  [[nodiscard]] bool supports_burst() const noexcept {
    return operations_->supports_burst;
  }
  [[nodiscard]] bool is_exit() const noexcept { return operations_->is_exit; }
  [[nodiscard]] std::uint32_t exit_code() const noexcept {
    return operations_->exit_code == nullptr ? 0
                                             : operations_->exit_code(*this);
  }

  virtual void reset() noexcept {}

 protected:
  explicit Device(const DeviceOperations &operations) noexcept
      : operations_(&operations) {}

 private:
  const DeviceOperations *operations_;
};

class AddressSpace {
 public:
  struct Mapping {
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    Device *device = nullptr;
    std::string name;

    [[nodiscard]] std::uint64_t end() const noexcept { return base + size; }
  };

  // Mappings are non-owning.  Devices must outlive the AddressSpace.
  void map(std::uint64_t base, std::uint64_t size, Device &device,
           std::string name = {});

  // Returns a mapping only if [address, address + length) lies wholly inside
  // that one mapping.  Mapping pointers remain valid until the next map().
  [[nodiscard]] const Mapping *resolve(std::uint64_t address,
                                       std::uint64_t length) const noexcept;
  [[nodiscard]] Mapping *resolve(std::uint64_t address,
                                 std::uint64_t length) noexcept;

  [[nodiscard]] Response read(std::uint64_t address, std::span<std::byte> data,
                              std::span<const std::uint8_t> enable);
  [[nodiscard]] Response write(std::uint64_t address,
                               std::span<const std::byte> data,
                               std::span<const std::uint8_t> strobe);

  // Host image initialization.  The complete range must resolve to one
  // loadable device; unlike write(), this may initialize a ROM.
  [[nodiscard]] Response load(std::uint64_t address,
                              std::span<const std::byte> data);

  void reset() noexcept;

  [[nodiscard]] const std::vector<Mapping> &mappings() const noexcept {
    return mappings_;
  }

 private:
  [[nodiscard]] Response resolution_error(std::uint64_t address,
                                          std::uint64_t length) const noexcept;

  std::vector<Mapping> mappings_;
};

[[nodiscard]] constexpr bool response_is_success(Response response) noexcept {
  return response == Response::okay || response == Response::exclusive_okay;
}

}  // namespace axi_tb
