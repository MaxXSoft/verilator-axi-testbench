#include <array>
#include <cassert>
#include <sstream>
#include <stdexcept>

#include "devices.hpp"
#include "platform.hpp"

namespace {
using namespace axi_tb;
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
  const Signal *output(std::string_view name) const noexcept override {
    return name == "irq" ? &out : nullptr;
  }
  InputSignal *input(std::string_view name) noexcept override {
    return name == "in" ? &in : nullptr;
  }
  Response read_impl(std::uint64_t, std::span<std::byte>,
                     std::span<const std::uint8_t>) override {
    return Response::SlaveError;
  }
  Response write_impl(std::uint64_t, std::span<const std::byte>,
                      std::span<const std::uint8_t>) override {
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
int main() {
  DeviceRegistry registry;
  register_builtin_devices(registry);
  registry.add("counter", {{},
                           [](const DeviceConfig &, HostServices &) {
                             return std::make_unique<Counter>();
                           },
                           {}});
  auto spec = default_platform();
  spec.remove("uart");
  spec.remove("ram");
  spec.map("exit", 0x1000, 4);
  spec.map("rom", 0x2000, 0x100);
  spec.set("rom.size=256");
  // Deliberately add the sink first; it is unmapped and still ticks/resets.
  spec.devices.push_back({"sink", "counter", {}});
  spec.devices.push_back({"source", "counter", {}});
  spec.connections.push_back({"source.irq", "sink.in"});
  Platform platform(registry, spec, 32);
  assert(platform.address_space().mappings().size() == 2);
  auto &source = static_cast<Counter &>(platform.device("source"));
  auto &sink = static_cast<Counter &>(platform.device("sink"));
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
  assert(static_cast<RomDevice &>(platform.device("rom")).bytes()[0] ==
         image[0]);
  rejects([&] { platform.address_space().map(0x3000, 4, source); });
  rejects([&] { (void)platform.output("source.irq", 64); });
  rejects([&] { (void)platform.output("source.missing"); });
  auto bad = spec;
  bad.set("rom.size=-1");
  rejects([&] { Platform p(registry, bad); });
  bad = spec;
  bad.set("rom.unknown=42");
  rejects([&] { Platform p(registry, bad); });
  bad = spec;
  bad.connections.push_back({"sink.irq", "source.in"});
  rejects([&] { Platform p(registry, bad); });
  bad = spec;
  bad.connections.push_back({"source.irq", "sink.in"});
  rejects([&] { Platform p(registry, bad); });
  bad = spec;
  bad.mappings.push_back({"rom", 0x1000, 4});
  rejects([&] { Platform p(registry, bad); });
  bad = spec;
  bad.map("rom", 0xfffffff0, 0x100);
  rejects([&] { Platform p(registry, bad, 32); });
  bad = spec;
  bad.devices.push_back(bad.devices.front());
  rejects([&] { Platform p(registry, bad); });
  rejects([&] { register_builtin_devices(registry); });
  std::ostringstream help;
  registry.print_help(help);
  assert(help.str().find("image") != std::string::npos);
}
