#include "interrupt_devices.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <stdexcept>

namespace fuxi_sim {
namespace {
using axi_tb::Response;
struct Access {
  std::uint64_t address;
  std::size_t lane;
  std::size_t size;
};
// Accept one aligned 32-bit word, or a CLINT 64-bit timer register. The AXI
// fabric passes an entire bus beat, so ignore inactive outer lanes.
std::optional<Access> access(std::uint64_t offset,
                             std::span<const std::uint8_t> lanes) {
  const auto first = std::ranges::find_if(lanes, [](auto v) { return v != 0; });
  if (first == lanes.end()) {
    return Access{.address = offset, .lane = 0, .size = 0};
  }
  const auto start = static_cast<std::size_t>(first - lanes.begin());
  std::size_t end = lanes.size();
  while (lanes[end - 1] == 0) {
    --end;
  }
  const auto size = end - start;
  if ((size != 4 && size != 8) || offset > UINT64_MAX - start ||
      (offset + start) % size != 0) {
    return {};
  }
  for (auto i = start; i < end; ++i) {
    if (!lanes[i]) {
      return {};
    }
  }
  return Access{.address = offset + start, .lane = start, .size = size};
}
std::uint64_t get(std::span<const std::byte> data, const Access &a) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < a.size; ++i) {
    value |= static_cast<std::uint64_t>(
                 std::to_integer<std::uint8_t>(data[a.lane + i]))
             << (i * 8);
  }
  return value;
}
void put(std::span<std::byte> data, const Access &a, std::uint64_t value) {
  for (std::size_t i = 0; i < a.size; ++i) {
    data[a.lane + i] = std::byte((value >> (i * 8)) & 0xffU);
  }
}
std::optional<unsigned> index(std::string_view name, std::string_view prefix,
                              unsigned limit) noexcept {
  if (!name.starts_with(prefix)) {
    return {};
  }
  name.remove_prefix(prefix.size());
  unsigned value = 0;
  const auto parsed =
      std::from_chars(name.data(), name.data() + name.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size() ||
      value >= limit) {
    return {};
  }
  return value;
}
bool timer_register(std::uint64_t address, std::size_t size) {
  return address == 0x4000 || address == 0xbff8 ||
         (size == 4 && (address == 0x4004 || address == 0xbffc));
}
}  // namespace

Clint::Clint(std::uint64_t divider) : divider_(divider) {
  if (!divider) {
    throw std::invalid_argument("CLINT divider must be positive");
  }
  reset();
}
void Clint::reset() noexcept {
  phase_ = 0;
  compare_ = UINT64_MAX;
  time_.value = timer_.value = software_.value = 0;
}
void Clint::tick() {
  if (++phase_ == divider_) {
    phase_ = 0;
    ++time_.value;
  }
  settle();
}
void Clint::settle() noexcept { timer_.value = time_.value >= compare_; }
const axi_tb::Signal *Clint::output(std::string_view name) const noexcept {
  if (name == "mtime") {
    return &time_;
  }
  if (name == "mtip") {
    return &timer_;
  }
  if (name == "msip") {
    return &software_;
  }
  return nullptr;
}
Response Clint::read_impl(std::uint64_t offset, std::span<std::byte> data,
                          std::span<const std::uint8_t> lanes) {
  const auto a = access(offset, lanes);
  if (!a) {
    return Response::SlaveError;
  }
  if (!a->size) {
    return Response::Okay;
  }
  if (a->address == 0 && a->size == 4) {
    put(data, *a, software_.value);
  } else if (timer_register(a->address, a->size)) {
    const auto value = a->address < 0x8000 ? compare_ : time_.value;
    put(data, *a, value >> ((a->address & 4U) ? 32U : 0U));
  } else {
    return Response::SlaveError;
  }
  return Response::Okay;
}
Response Clint::write_impl(std::uint64_t offset,
                           std::span<const std::byte> data,
                           std::span<const std::uint8_t> lanes) {
  const auto a = access(offset, lanes);
  if (!a) {
    return Response::SlaveError;
  }
  if (!a->size) {
    return Response::Okay;
  }
  const auto value = get(data, *a);
  if (a->address == 0 && a->size == 4) {
    software_.value = value & 1U;
  } else if (timer_register(a->address, a->size)) {
    auto &target = a->address < 0x8000 ? compare_ : time_.value;
    if (a->size == 8) {
      target = value;
    } else {
      const unsigned shift = (a->address & 4U) ? 32U : 0U;
      target = (target & ~(UINT64_C(0xffffffff) << shift)) | (value << shift);
    }
  } else {
    return Response::SlaveError;
  }
  settle();
  return Response::Okay;
}

Plic::Plic(unsigned sources, unsigned contexts)
    : sources_(sources), contexts_(contexts) {
  if (!sources || sources > 1023 || !contexts || contexts > 64) {
    throw std::invalid_argument(
        "PLIC requires 1..1023 sources and 1..64 contexts");
  }
  inputs_.resize(sources + 1);
  priority_.resize(sources + 1);
  pending_.resize(sources + 1);
  busy_.resize(sources + 1);
  irqs_.resize(contexts);
  threshold_.resize(contexts);
  enables_.resize(contexts);
  reset();
}
void Plic::reset() noexcept {
  std::ranges::fill(priority_, 0);
  std::ranges::fill(threshold_, 0);
  // vector<bool> iterators are not writable ranges under the C++20 contract.
  // NOLINTNEXTLINE(modernize-use-ranges)
  std::fill(pending_.begin(), pending_.end(), false);
  // NOLINTNEXTLINE(modernize-use-ranges)
  std::fill(busy_.begin(), busy_.end(), false);
  for (auto &enable : enables_) {
    enable.fill(0);
  }
  for (auto &irq : irqs_) {
    irq.value = 0;
  }
}
axi_tb::InputSignal *Plic::input(std::string_view name) noexcept {
  const auto i = index(name, "source", sources_ + 1);
  return i && *i != 0 ? &inputs_[*i] : nullptr;
}
const axi_tb::Signal *Plic::output(std::string_view name) const noexcept {
  const auto i = index(name, "context", contexts_);
  return i ? &irqs_[*i] : nullptr;
}
bool Plic::enabled(unsigned context, unsigned source) const noexcept {
  return (enables_[context][source / 32] & (1U << (source % 32))) != 0;
}
unsigned Plic::best(unsigned context) const noexcept {
  unsigned selected = 0;
  for (unsigned i = 1; i <= sources_; ++i) {
    if (pending_[i] && enabled(context, i) &&
        priority_[i] > priority_[selected]) {
      selected = i;
    }
  }
  return selected;  // Priority 0 is masked; ascending scan breaks ties by ID.
}
void Plic::notify() noexcept {
  for (unsigned c = 0; c < contexts_; ++c) {
    irqs_[c].value = priority_[best(c)] > threshold_[c];
  }
}
void Plic::settle() noexcept {
  for (unsigned i = 1; i <= sources_; ++i) {
    if (inputs_[i].value() && !busy_[i]) {
      pending_[i] = busy_[i] = true;
    }
  }
  notify();
}
std::uint32_t Plic::read_register(std::uint64_t offset) {
  if (offset < 0x1000) {
    return offset / 4 <= sources_ ? priority_[offset / 4] : 0;
  }
  if (offset < 0x1080) {
    const auto base = static_cast<unsigned>((offset - 0x1000) / 4) * 32;
    std::uint32_t value = 0;
    for (unsigned bit = 0; bit < 32 && base + bit <= sources_; ++bit) {
      if (pending_[base + bit]) {
        value |= 1U << bit;
      }
    }
    return value;
  }
  if (offset >= 0x2000 && offset < 0x2000 + contexts_ * 0x80) {
    return enables_[(offset - 0x2000) / 0x80][(offset % 0x80) / 4];
  }
  if (offset >= 0x200000 && offset < 0x200000 + contexts_ * 0x1000) {
    const auto c = static_cast<unsigned>((offset - 0x200000) / 0x1000);
    if (offset % 0x1000 == 0) {
      return threshold_[c];
    }
    if (offset % 0x1000 == 4) {
      const auto id =
          best(c);  // Claim is intentionally independent of threshold.
      if (id) {
        pending_[id] = false;
      }
      notify();
      return id;
    }
  }
  return 0;
}
void Plic::write_register(std::uint64_t offset, std::uint32_t value) {
  if (offset > 0 && offset < 0x1000 && offset / 4 <= sources_) {
    priority_[offset / 4] = value & 7U;
  } else if (offset >= 0x2000 && offset < 0x2000 + contexts_ * 0x80) {
    const auto c = static_cast<unsigned>((offset - 0x2000) / 0x80);
    const auto word = static_cast<unsigned>((offset % 0x80) / 4);
    std::uint32_t mask = 0;
    for (unsigned bit = 0; bit < 32; ++bit) {
      const auto id = word * 32 + bit;
      if (id && id <= sources_) {
        mask |= 1U << bit;
      }
    }
    enables_[c][word] = value & mask;
  } else if (offset >= 0x200000 && offset < 0x200000 + contexts_ * 0x1000) {
    const auto c = static_cast<unsigned>((offset - 0x200000) / 0x1000);
    if (offset % 0x1000 == 0) {
      threshold_[c] = value & 7U;
    } else if (offset % 0x1000 == 4 && value && value <= sources_ &&
               enabled(c, value)) {
      busy_[value] = false;  // No requirement to match the claiming context.
    }
  }
  settle();
}
Response Plic::read_impl(std::uint64_t offset, std::span<std::byte> data,
                         std::span<const std::uint8_t> lanes) {
  const auto a = access(offset, lanes);
  if (!a || a->size == 8 || a->address >= 0x4000000) {
    return Response::SlaveError;
  }
  if (a->size) {
    put(data, *a, read_register(a->address));
  }
  return Response::Okay;
}
Response Plic::write_impl(std::uint64_t offset, std::span<const std::byte> data,
                          std::span<const std::uint8_t> lanes) {
  const auto a = access(offset, lanes);
  if (!a || a->size == 8 || a->address >= 0x4000000) {
    return Response::SlaveError;
  }
  if (a->size) {
    write_register(a->address, static_cast<std::uint32_t>(get(data, *a)));
  }
  return Response::Okay;
}
}  // namespace fuxi_sim
