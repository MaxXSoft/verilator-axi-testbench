#include "devices.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#define AXI_TB_HAS_POSIX_MMAP 1
#else
#define AXI_TB_HAS_POSIX_MMAP 0
#endif

namespace axi_tb {
namespace {

[[nodiscard]] bool in_bounds(std::uint64_t offset, std::size_t length,
                             std::size_t capacity) noexcept {
  return offset <= capacity && length <= capacity - offset;
}

[[nodiscard]] bool range_in_bounds(std::uint64_t offset, std::uint64_t length,
                                   std::uint64_t capacity) noexcept {
  return offset <= capacity && length <= capacity - offset;
}

[[nodiscard]] std::uint8_t as_u8(std::byte value) noexcept {
  return std::to_integer<std::uint8_t>(value);
}

[[nodiscard]] bool all_lanes_enabled(
    std::span<const std::uint8_t> lanes) noexcept {
  return std::ranges::all_of(lanes,
                             [](std::uint8_t value) { return value != 0; });
}

}  // namespace

void AddressSpace::map(std::uint64_t base, std::uint64_t size, Device &device,
                       std::string name) {
  if (size == 0) {
    throw std::invalid_argument("address-space mapping must not be empty");
  }
  if (size > std::numeric_limits<std::uint64_t>::max() - base) {
    throw std::invalid_argument("address-space mapping overflows uint64_t");
  }

  const std::uint64_t end = base + size;
  auto position = std::ranges::lower_bound(mappings_, base, {}, &Mapping::base);
  if (position != mappings_.begin()) {
    const auto &previous = *std::prev(position);
    if (previous.end() > base) {
      throw std::invalid_argument("address-space mappings overlap");
    }
  }
  if (position != mappings_.end() && end > position->base) {
    throw std::invalid_argument("address-space mappings overlap");
  }

  mappings_.insert(position, Mapping{.base = base,
                                     .size = size,
                                     .device = &device,
                                     .name = std::move(name)});
}

const AddressSpace::Mapping *AddressSpace::resolve(
    std::uint64_t address, std::uint64_t length) const noexcept {
  if (length == 0 || mappings_.empty()) {
    return nullptr;
  }
  auto position =
      std::ranges::upper_bound(mappings_, address, {}, &Mapping::base);
  if (position == mappings_.begin()) {
    return nullptr;
  }
  --position;
  if (address < position->base || address >= position->end()) {
    return nullptr;
  }
  if (length > position->end() - address) {
    return nullptr;
  }
  return &*position;
}

AddressSpace::Mapping *AddressSpace::resolve(std::uint64_t address,
                                             std::uint64_t length) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  return const_cast<Mapping *>(std::as_const(*this).resolve(address, length));
}

Response AddressSpace::resolution_error(std::uint64_t address,
                                        std::uint64_t length) const noexcept {
  (void)length;
  auto position =
      std::ranges::upper_bound(mappings_, address, {}, &Mapping::base);
  if (position != mappings_.begin()) {
    const auto &candidate = *std::prev(position);
    if (address >= candidate.base && address < candidate.end()) {
      // The first byte was decoded, but the complete access did not fit.
      return Response::SlaveError;
    }
  }
  return Response::DecodeError;
}

Response AddressSpace::read(std::uint64_t address, std::span<std::byte> data,
                            std::span<const std::uint8_t> enable) {
  if (data.size() != enable.size()) {
    throw std::invalid_argument("address-space read data/enable size mismatch");
  }
  std::ranges::fill(data, std::byte{0});
  if (data.empty()) {
    return Response::Okay;
  }
  Mapping *mapping = resolve(address, data.size());
  if (mapping == nullptr) {
    return resolution_error(address, data.size());
  }
  return mapping->device->read(address - mapping->base, data, enable);
}

Response AddressSpace::write(std::uint64_t address,
                             std::span<const std::byte> data,
                             std::span<const std::uint8_t> strobe) {
  if (data.size() != strobe.size()) {
    throw std::invalid_argument(
        "address-space write data/strobe size mismatch");
  }
  if (data.empty()) {
    return Response::Okay;
  }
  Mapping *mapping = resolve(address, data.size());
  if (mapping == nullptr) {
    return resolution_error(address, data.size());
  }
  return mapping->device->write(address - mapping->base, data, strobe);
}

Response AddressSpace::load(std::uint64_t address,
                            std::span<const std::byte> data) {
  if (data.empty()) {
    return Response::Okay;
  }
  Mapping *mapping = resolve(address, data.size());
  if (mapping == nullptr) {
    return resolution_error(address, data.size());
  }
  if (!mapping->device->loadable()) {
    return Response::SlaveError;
  }
  const std::uint64_t offset = address - mapping->base;
  if (!mapping->device->can_load(offset, data.size())) {
    return Response::SlaveError;
  }
  return mapping->device->load(offset, data);
}

void AddressSpace::reset() noexcept {
  // Avoid allocation in this noexcept reset path.  Mapping counts are small,
  // and a device may legally appear in more than one window.
  for (std::size_t index = 0; index < mappings_.size(); ++index) {
    bool already_reset = false;
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (mappings_[previous].device == mappings_[index].device) {
        already_reset = true;
        break;
      }
    }
    if (!already_reset) {
      mappings_[index].device->reset();
    }
  }
}

RomDevice::RomDevice(std::size_t size) : bytes_(size, std::byte{0}) {
  if (size == 0) {
    throw std::invalid_argument("ROM size must not be zero");
  }
}

RomDevice::RomDevice(std::span<const std::byte> image)
    : RomDevice(image, image.size()) {}

RomDevice::RomDevice(std::span<const std::byte> image, std::size_t size)
    : bytes_(size, std::byte{0}) {
  if (size == 0) {
    throw std::invalid_argument("ROM size must not be zero");
  }
  if (image.size() > size) {
    throw std::invalid_argument("ROM image is larger than ROM");
  }
  std::ranges::copy(image, bytes_.begin());
}

Response RomDevice::read_impl(std::uint64_t offset, std::span<std::byte> data,
                              std::span<const std::uint8_t> enable) {
  if (!in_bounds(offset, data.size(), bytes_.size())) {
    return Response::SlaveError;
  }
  const auto start = static_cast<std::size_t>(offset);
  if (all_lanes_enabled(enable)) {
    std::memcpy(data.data(), bytes_.data() + start, data.size());
    return Response::Okay;
  }
  for (std::size_t lane = 0; lane < data.size(); ++lane) {
    if (enable[lane] != 0) {
      data[lane] = bytes_[start + lane];
    }
  }
  return Response::Okay;
}

Response RomDevice::write_impl(std::uint64_t offset,
                               std::span<const std::byte> data,
                               std::span<const std::uint8_t> strobe) {
  (void)strobe;
  if (!in_bounds(offset, data.size(), bytes_.size())) {
    return Response::SlaveError;
  }
  // An AXI write transaction targeting ROM is an error, even if all WSTRB
  // lanes happen to be zero.
  return Response::SlaveError;
}

Response RomDevice::load(std::uint64_t offset,
                         std::span<const std::byte> data) {
  if (!in_bounds(offset, data.size(), bytes_.size())) {
    return Response::SlaveError;
  }
  std::ranges::copy(data, bytes_.data() + static_cast<std::size_t>(offset));
  return Response::Okay;
}

bool RomDevice::can_load(std::uint64_t offset,
                         std::uint64_t size) const noexcept {
  return range_in_bounds(offset, size, bytes_.size());
}

RamDevice::RamDevice(std::size_t size) : size_(size) {
  if (size == 0) {
    throw std::invalid_argument("RAM size must not be zero");
  }
#if AXI_TB_HAS_POSIX_MMAP
  void *mapping = ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
  if (mapping != MAP_FAILED) {
    data_ = static_cast<std::byte *>(mapping);
    mapped_ = true;
    return;
  }
#endif
  fallback_.resize(size, std::byte{0});
  data_ = fallback_.data();
}

RamDevice::~RamDevice() { release(); }

RamDevice::RamDevice(RamDevice &&other) noexcept
    : size_(std::exchange(other.size_, 0)),
      data_(std::exchange(other.data_, nullptr)),
      mapped_(std::exchange(other.mapped_, false)),
      fallback_(std::move(other.fallback_)) {
  if (!mapped_ && !fallback_.empty()) {
    data_ = fallback_.data();
  }
}

RamDevice &RamDevice::operator=(RamDevice &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  release();
  size_ = std::exchange(other.size_, 0);
  data_ = std::exchange(other.data_, nullptr);
  mapped_ = std::exchange(other.mapped_, false);
  fallback_ = std::move(other.fallback_);
  if (!mapped_ && !fallback_.empty()) {
    data_ = fallback_.data();
  }
  return *this;
}

void RamDevice::release() noexcept {
#if AXI_TB_HAS_POSIX_MMAP
  if (mapped_ && data_ != nullptr) {
    ::munmap(data_, size_);
  }
#endif
  mapped_ = false;
  data_ = nullptr;
  size_ = 0;
  fallback_.clear();
}

Response RamDevice::read_impl(std::uint64_t offset, std::span<std::byte> data,
                              std::span<const std::uint8_t> enable) {
  if (!in_bounds(offset, data.size(), size_)) {
    return Response::SlaveError;
  }
  const auto start = static_cast<std::size_t>(offset);
  if (all_lanes_enabled(enable)) {
    std::memcpy(data.data(), data_ + start, data.size());
    return Response::Okay;
  }
  for (std::size_t lane = 0; lane < data.size(); ++lane) {
    if (enable[lane] != 0) {
      data[lane] = data_[start + lane];
    }
  }
  return Response::Okay;
}

Response RamDevice::write_impl(std::uint64_t offset,
                               std::span<const std::byte> data,
                               std::span<const std::uint8_t> strobe) {
  if (!in_bounds(offset, data.size(), size_)) {
    return Response::SlaveError;
  }
  const auto start = static_cast<std::size_t>(offset);
  if (all_lanes_enabled(strobe)) {
    std::memcpy(data_ + start, data.data(), data.size());
    return Response::Okay;
  }
  for (std::size_t lane = 0; lane < data.size(); ++lane) {
    if (strobe[lane] != 0) {
      data_[start + lane] = data[lane];
    }
  }
  return Response::Okay;
}

Response RamDevice::load(std::uint64_t offset,
                         std::span<const std::byte> data) {
  if (!in_bounds(offset, data.size(), size_)) {
    return Response::SlaveError;
  }
  std::ranges::copy(data, data_ + offset);
  return Response::Okay;
}

bool RamDevice::can_load(std::uint64_t offset,
                         std::uint64_t size) const noexcept {
  return range_in_bounds(offset, size, size_);
}

FileUartBackend::FileUartBackend(std::FILE *input, std::FILE *output) noexcept
    : input_(input), output_(output) {}

bool FileUartBackend::try_read(std::uint8_t &byte) {
  if (input_ == nullptr) {
    return false;
  }
#if defined(__unix__) || defined(__APPLE__)
  const int descriptor = ::fileno(input_);
  if (descriptor < 0) {
    return false;
  }
  pollfd descriptor_state{
      .fd = descriptor, .events = POLLIN | POLLHUP, .revents = 0};
  const int result = ::poll(&descriptor_state, 1, 0);
  if (result <= 0 || (descriptor_state.revents & (POLLIN | POLLHUP)) == 0) {
    return false;
  }
#else
  // There is no portable way to query stdin without blocking.  Non-stdin
  // FILE handles are expected to be regular files on this fallback path.
  if (input_ == stdin) {
    return false;
  }
#endif
  const int value = std::fgetc(input_);
  if (value == EOF) {
    return false;
  }
  byte = static_cast<std::uint8_t>(value);
  return true;
}

void FileUartBackend::write(std::uint8_t byte) {
  if (output_ != nullptr) {
    std::fputc(static_cast<int>(byte), output_);
  }
}

void FileUartBackend::flush() {
  if (output_ != nullptr) {
    std::fflush(output_);
  }
}

BufferUartBackend::BufferUartBackend(std::span<const std::byte> input) {
  push_input(input);
}

void BufferUartBackend::push_input(std::span<const std::byte> input) {
  for (const std::byte value : input) {
    input_.push_back(as_u8(value));
  }
}

void BufferUartBackend::push_input(std::uint8_t byte) {
  input_.push_back(byte);
}

bool BufferUartBackend::try_read(std::uint8_t &byte) {
  if (input_.empty()) {
    return false;
  }
  byte = input_.front();
  input_.pop_front();
  return true;
}

void BufferUartBackend::write(std::uint8_t byte) {
  output_.push_back(static_cast<std::byte>(byte));
}

UartDevice::UartDevice() : owned_backend_(std::in_place) {
  bind_backend(*owned_backend_);
  reset();
}

void UartDevice::reset() noexcept {
  receive_fifo_.clear();
  divisor_low_ = 0;
  divisor_high_ = 0;
  interrupt_enable_ = 0;
  line_control_ = 0;
  modem_control_ = 0;
  scratch_ = 0;
  fifo_control_ = 0;
}

void UartDevice::poll_input() {
  const std::size_t capacity = (fifo_control_ & 1U) != 0 ? 16U : 1U;
  while (receive_fifo_.size() < capacity) {
    std::uint8_t value = 0;
    if (!backend_try_read_(backend_context_, value)) {
      break;
    }
    if (!receive_fifo_.push(value)) {
      break;
    }
  }
}

std::uint8_t UartDevice::interrupt_identification() const noexcept {
  const std::uint8_t fifo_bits =
      (fifo_control_ & 1U) != 0 ? static_cast<std::uint8_t>(0xc0) : 0;
  if ((interrupt_enable_ & 1U) != 0 && !receive_fifo_.empty()) {
    return static_cast<std::uint8_t>(fifo_bits | 0x04U);
  }
  if ((interrupt_enable_ & 2U) != 0) {
    return static_cast<std::uint8_t>(fifo_bits | 0x02U);
  }
  return static_cast<std::uint8_t>(fifo_bits | 0x01U);
}

std::uint8_t UartDevice::read_register(std::uint64_t index) {
  switch (index) {
    case 0: {
      if ((line_control_ & LCR_DLAB) != 0) {
        return divisor_low_;
      }
      if (receive_fifo_.empty()) {
        return 0;
      }
      const std::uint8_t value = receive_fifo_.front();
      receive_fifo_.pop();
      return value;
    }
    case 1:
      return (line_control_ & LCR_DLAB) != 0 ? divisor_high_
                                             : interrupt_enable_;
    case 2:
      return interrupt_identification();
    case 3:
      return line_control_;
    case 4:
      return modem_control_;
    case 5:
      return static_cast<std::uint8_t>(
          LSR_THR_EMPTY | LSR_TRANSMITTER_EMPTY |
          (receive_fifo_.empty() ? 0U : LSR_DATA_READY));
    case 6:
      return 0;
    case 7:
      return scratch_;
    default:
      return 0;
  }
}

void UartDevice::write_register(std::uint64_t index, std::uint8_t value) {
  switch (index) {
    case 0:
      if ((line_control_ & LCR_DLAB) != 0) {
        divisor_low_ = value;
      } else {
        backend_write_(backend_context_, value);
        backend_flush_(backend_context_);
      }
      break;
    case 1:
      if ((line_control_ & LCR_DLAB) != 0) {
        divisor_high_ = value;
      } else {
        interrupt_enable_ = static_cast<std::uint8_t>(value & 0x0fU);
      }
      break;
    case 2:
      fifo_control_ = value;
      if ((value & 0x02U) != 0) {
        receive_fifo_.clear();
      }
      break;
    case 3:
      line_control_ = value;
      break;
    case 4:
      modem_control_ = value;
      break;
    case 5:
    case 6:
      break;  // Read-only registers ignore writes, like a 16550.
    case 7:
      scratch_ = value;
      break;
    default:
      break;
  }
}

Response UartDevice::read_impl(std::uint64_t offset, std::span<std::byte> data,
                               std::span<const std::uint8_t> enable) {
  if (!in_bounds(offset, data.size(), REGISTER_SPAN)) {
    return Response::SlaveError;
  }
  poll_input();
  for (std::size_t lane = 0; lane < data.size(); ++lane) {
    if (enable[lane] != 0) {
      data[lane] = static_cast<std::byte>(read_register(offset + lane));
    }
  }
  return Response::Okay;
}

Response UartDevice::write_impl(std::uint64_t offset,
                                std::span<const std::byte> data,
                                std::span<const std::uint8_t> strobe) {
  if (!in_bounds(offset, data.size(), REGISTER_SPAN)) {
    return Response::SlaveError;
  }
  for (std::size_t lane = 0; lane < data.size(); ++lane) {
    if (strobe[lane] != 0) {
      write_register(offset + lane, as_u8(data[lane]));
    }
  }
  return Response::Okay;
}

ExitDevice::ExitDevice() noexcept = default;

void ExitDevice::clear() noexcept {
  requested_ = false;
  code_ = 0;
}

Response ExitDevice::read_impl(std::uint64_t offset, std::span<std::byte> data,
                               std::span<const std::uint8_t> enable) {
  if (!in_bounds(offset, data.size(), REGISTER_SPAN)) {
    return Response::SlaveError;
  }
  for (std::size_t lane = 0; lane < data.size(); ++lane) {
    if (enable[lane] != 0) {
      const auto shift = static_cast<unsigned>((offset + lane) * CHAR_BIT);
      data[lane] = static_cast<std::byte>((code_ >> shift) & 0xffU);
    }
  }
  return Response::Okay;
}

Response ExitDevice::write_impl(std::uint64_t offset,
                                std::span<const std::byte> data,
                                std::span<const std::uint8_t> strobe) {
  if (offset != 0 || data.size() != REGISTER_SPAN ||
      !std::ranges::all_of(strobe,
                           [](std::uint8_t value) { return value != 0; })) {
    return Response::SlaveError;
  }

  code_ = static_cast<std::uint32_t>(as_u8(data[0])) |
          (static_cast<std::uint32_t>(as_u8(data[1])) << 8U) |
          (static_cast<std::uint32_t>(as_u8(data[2])) << 16U) |
          (static_cast<std::uint32_t>(as_u8(data[3])) << 24U);
  requested_ = true;
  ++generation_;
  return Response::Okay;
}

}  // namespace axi_tb
