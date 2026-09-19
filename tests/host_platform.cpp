#include <array>
#include <cassert>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "devices.hpp"
#include "platform.hpp"

namespace {
using axi_tb::AddressSpace;
using axi_tb::default_platform;
using axi_tb::Device;
using axi_tb::DeviceConfig;
using axi_tb::DeviceRegistry;
using axi_tb::HostServices;
using axi_tb::InputSignal;
using axi_tb::Platform;
using axi_tb::Response;
using axi_tb::RomDevice;
using axi_tb::Signal;
static_assert(
    std::is_same_v<decltype(std::declval<AddressSpace &>().resolve(0, 1)),
                   const AddressSpace::Mapping *>);
struct Counter final : Device {
  Signal out;
  InputSignal in;
  unsigned resets = 0;
  std::uint64_t count = 0;
  void tick() override { ++count; }
  void settle() noexcept override { out.value = (count != 0) || in.value(); }
  void reset() noexcept override {
    count = 0;
    out.value = 0;
    ++resets;
  }
  [[nodiscard]] const Signal *output(
      std::string_view name) const noexcept override {
    return name == "irq" ? &out : nullptr;
  }
  InputSignal *input(std::string_view name) noexcept override {
    return name == "in" ? &in : nullptr;
  }

 private:
  Response read_impl(std::uint64_t /*offset*/, std::span<std::byte> /*data*/,
                     std::span<const std::uint8_t> /*enable*/) override {
    return Response::SlaveError;
  }
  Response write_impl(std::uint64_t /*offset*/,
                      std::span<const std::byte> /*data*/,
                      std::span<const std::uint8_t> /*strobe*/) override {
    return Response::SlaveError;
  }
};
template <class Fn>
void rejects(Fn fn) {
  bool rejected = false;
  try {
    fn();
  } catch (const std::logic_error &) {
    rejected = true;
  }
  assert(rejected);
}
}  // namespace
// Uncaught exceptions intentionally fail this standalone test executable.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
  DeviceRegistry registry;
  register_builtin_devices(registry);
  registry.add("counter", {
                              .options = {},
                              .create =
                                  [](const DeviceConfig & /*config*/,
                                     HostServices & /*host*/) {
                                    return std::make_unique<Counter>();
                                  },
                              .image_option = {},
                          });
  auto spec = default_platform();
  spec.remove("uart");
  spec.remove("ram");
  spec.map("exit", 0x1000, 4);
  spec.map("rom", 0x2000, 0x100);
  spec.set("rom.size=256");
  // Deliberately add the sink first; it is unmapped and still ticks/resets.
  spec.devices.push_back({.id = "sink", .type = "counter", .properties = {}});
  spec.devices.push_back({.id = "source", .type = "counter", .properties = {}});
  spec.connections.push_back(
      {.source = "source.irq", .destination = "sink.in"});
  Platform platform(registry, spec, 32);
  assert(platform.address_space().mappings().size() == 2);
  auto &source = dynamic_cast<Counter &>(platform.device("source"));
  const auto &sink = dynamic_cast<Counter &>(platform.device("sink"));
  assert(source.resets == 1 && sink.resets == 1);
  platform.begin_cycle(true);
  platform.end_cycle(true);
  platform.begin_cycle(true);
  assert(source.resets == 2 && source.count == 0);
  platform.begin_cycle(false);
  platform.end_cycle(false);
  assert(source.count == 1 && sink.in.value() == 1 &&
         platform.output("sink.irq").value == 1);
  const std::array<std::byte, 1> image{std::byte{0xaa}};
  assert(platform.address_space().load(0x2000, image) == Response::Okay);
  platform.reset();
  assert(dynamic_cast<RomDevice &>(platform.device("rom")).bytes()[0] ==
         image[0]);
  rejects([&] { platform.address_space().map(0x3000, 4, source); });
  rejects([&] { (void)platform.output("source.irq", 64); });
  rejects([&] { (void)platform.output("source.missing"); });
  auto bad = spec;
  bad.set("rom.size=-1");
  rejects([&] { const Platform p(registry, bad); });
  bad = spec;
  bad.set("rom.unknown=42");
  rejects([&] { const Platform p(registry, bad); });
  bad = spec;
  bad.connections.push_back({.source = "sink.irq", .destination = "source.in"});
  rejects([&] { const Platform p(registry, bad); });
  bad = spec;
  bad.connections.push_back({.source = "source.irq", .destination = "sink.in"});
  rejects([&] { const Platform p(registry, bad); });
  bad = spec;
  bad.mappings.push_back({.device = "rom", .base = 0x1000, .size = 4});
  rejects([&] { const Platform p(registry, bad); });
  bad = spec;
  bad.map("rom", 0xfffffff0, 0x100);
  rejects([&] { const Platform p(registry, bad, 32); });
  bad = std::move(spec);
  bad.devices.push_back(bad.devices.front());
  rejects([&] { const Platform p(registry, bad); });
  rejects([&] { register_builtin_devices(registry); });
  std::ostringstream help;
  registry.print_help(help);
  assert(help.str().find("image") != std::string::npos);
}
