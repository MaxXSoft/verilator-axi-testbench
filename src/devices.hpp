#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "device.hpp"

namespace axi_tb {

class RomDevice final : public Device {
 public:
  explicit RomDevice(std::size_t size);
  explicit RomDevice(std::span<const std::byte> image);
  RomDevice(std::span<const std::byte> image, std::size_t size);

  [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
    return bytes_;
  }
  [[nodiscard]] Response load(std::uint64_t offset,
                              std::span<const std::byte> data) override;
  [[nodiscard]] bool loadable() const noexcept override { return true; }
  [[nodiscard]] bool can_load(std::uint64_t offset,
                              std::uint64_t size) const noexcept override;
  [[nodiscard]] bool supports_burst() const noexcept override { return true; }

 private:
  [[nodiscard]] Response read_impl(
      std::uint64_t offset, std::span<std::byte> data,
      std::span<const std::uint8_t> enable) override;
  [[nodiscard]] Response write_impl(
      std::uint64_t offset, std::span<const std::byte> data,
      std::span<const std::uint8_t> strobe) override;

  std::vector<std::byte> bytes_;
};

class RamDevice final : public Device {
 public:
  explicit RamDevice(std::size_t size);
  ~RamDevice() override;

  RamDevice(const RamDevice &) = delete;
  RamDevice &operator=(const RamDevice &) = delete;
  RamDevice(RamDevice &&other) noexcept;
  RamDevice &operator=(RamDevice &&other) noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::span<std::byte> bytes() noexcept { return {data_, size_}; }
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
    return {data_, size_};
  }
  [[nodiscard]] bool uses_lazy_mapping() const noexcept { return mapped_; }
  [[nodiscard]] Response load(std::uint64_t offset,
                              std::span<const std::byte> data) override;
  [[nodiscard]] bool loadable() const noexcept override { return true; }
  [[nodiscard]] bool can_load(std::uint64_t offset,
                              std::uint64_t size) const noexcept override;
  [[nodiscard]] bool supports_exclusive() const noexcept override {
    return true;
  }
  [[nodiscard]] bool supports_burst() const noexcept override { return true; }

 private:
  [[nodiscard]] Response read_impl(
      std::uint64_t offset, std::span<std::byte> data,
      std::span<const std::uint8_t> enable) override;
  [[nodiscard]] Response write_impl(
      std::uint64_t offset, std::span<const std::byte> data,
      std::span<const std::uint8_t> strobe) override;

  void release() noexcept;

  std::size_t size_ = 0;
  std::byte *data_ = nullptr;
  bool mapped_ = false;
  std::vector<std::byte> fallback_;
};

// Non-blocking FILE-backed UART.  stdin/stdout are the defaults, but arbitrary
// FILE handles can be supplied for deterministic tests and redirection.
class FileUartBackend final {
 public:
  explicit FileUartBackend(std::FILE *input = stdin,
                           std::FILE *output = stdout) noexcept;

  [[nodiscard]] bool try_read(std::uint8_t &byte);
  void write(std::uint8_t byte);
  void flush();

 private:
  std::FILE *input_;
  std::FILE *output_;
};

class BufferUartBackend final {
 public:
  BufferUartBackend() = default;
  explicit BufferUartBackend(std::span<const std::byte> input);

  void push_input(std::span<const std::byte> input);
  void push_input(std::uint8_t byte);
  [[nodiscard]] bool try_read(std::uint8_t &byte);
  void write(std::uint8_t byte);
  void flush() {}

  [[nodiscard]] const std::vector<std::byte> &output() const noexcept {
    return output_;
  }
  void clear_output() noexcept { output_.clear(); }

 private:
  std::deque<std::uint8_t> input_;
  std::vector<std::byte> output_;
};

class UartDevice final : public Device {
 public:
  static constexpr std::uint64_t REGISTER_SPAN = 8;
  static constexpr std::uint8_t LSR_DATA_READY = 1U << 0U;
  static constexpr std::uint8_t LSR_THR_EMPTY = 1U << 5U;
  static constexpr std::uint8_t LSR_TRANSMITTER_EMPTY = 1U << 6U;
  static constexpr std::uint8_t LCR_DLAB = 1U << 7U;

  UartDevice();
  template <typename Backend>
  explicit UartDevice(Backend &backend) noexcept {
    bind_backend(backend);
    reset();
  }

  void reset() noexcept override;

 private:
  using TryRead = bool (*)(void *, std::uint8_t &);
  using Write = void (*)(void *, std::uint8_t);
  using Flush = void (*)(void *);

  template <typename Backend>
  void bind_backend(Backend &backend) noexcept {
    backend_context_ = std::addressof(backend);
    backend_try_read_ = [](void *context, std::uint8_t &byte) {
      return static_cast<Backend *>(context)->try_read(byte);
    };
    backend_write_ = [](void *context, std::uint8_t byte) {
      static_cast<Backend *>(context)->write(byte);
    };
    backend_flush_ = [](void *context) {
      static_cast<Backend *>(context)->flush();
    };
  }

  [[nodiscard]] Response read_impl(
      std::uint64_t offset, std::span<std::byte> data,
      std::span<const std::uint8_t> enable) override;
  [[nodiscard]] Response write_impl(
      std::uint64_t offset, std::span<const std::byte> data,
      std::span<const std::uint8_t> strobe) override;

  void poll_input();
  [[nodiscard]] std::uint8_t read_register(std::uint64_t index);
  void write_register(std::uint64_t index, std::uint8_t value);
  [[nodiscard]] std::uint8_t interrupt_identification() const noexcept;

  std::optional<FileUartBackend> owned_backend_;
  void *backend_context_ = nullptr;
  TryRead backend_try_read_ = nullptr;
  Write backend_write_ = nullptr;
  Flush backend_flush_ = nullptr;
  RingBuffer<16, std::uint8_t> receive_fifo_;
  std::uint8_t divisor_low_ = 0;
  std::uint8_t divisor_high_ = 0;
  std::uint8_t interrupt_enable_ = 0;
  std::uint8_t line_control_ = 0;
  std::uint8_t modem_control_ = 0;
  std::uint8_t scratch_ = 0;
  std::uint8_t fifo_control_ = 0;
};

class ExitDevice final : public Device {
 public:
  static constexpr std::uint64_t REGISTER_SPAN = 4;

  ExitDevice() noexcept;

  [[nodiscard]] bool requested() const noexcept { return requested_; }
  [[nodiscard]] std::uint32_t code() const noexcept { return code_; }
  [[nodiscard]] std::uint64_t generation() const noexcept {
    return generation_;
  }
  void clear() noexcept;
  void reset() noexcept override { clear(); }
  [[nodiscard]] bool is_exit() const noexcept override { return true; }
  [[nodiscard]] std::uint32_t exit_code() const noexcept override {
    return code_;
  }

 private:
  [[nodiscard]] Response read_impl(
      std::uint64_t offset, std::span<std::byte> data,
      std::span<const std::uint8_t> enable) override;
  [[nodiscard]] Response write_impl(
      std::uint64_t offset, std::span<const std::byte> data,
      std::span<const std::uint8_t> strobe) override;

  bool requested_ = false;
  std::uint32_t code_ = 0;
  std::uint64_t generation_ = 0;
};

}  // namespace axi_tb
