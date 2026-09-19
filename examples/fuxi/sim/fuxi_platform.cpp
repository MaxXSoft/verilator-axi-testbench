#include "fuxi_platform.hpp"

#include "interrupt_devices.hpp"

namespace fuxi_sim {
void PlatformDefinition::register_devices(axi_tb::DeviceRegistry &registry) {
  using axi_tb::DeviceConfig;
  using axi_tb::HostServices;
  using axi_tb::OptionKind;
  registry.add(
      "riscv-clint",
      {{{"divider", OptionKind::Unsigned, "1", "CPU cycles per mtime tick", 1}},
       [](const DeviceConfig &c, HostServices &) {
         return std::make_unique<Clint>(c.number("divider"));
       },
       {}});
  registry.add("riscv-plic",
               {{{"sources", OptionKind::Unsigned, "31",
                  "External interrupt sources", 1, 1023},
                 {"contexts", OptionKind::Unsigned, "2",
                  "Interrupt target contexts", 1, 64}},
                [](const DeviceConfig &c, HostServices &) {
                  return std::make_unique<Plic>(
                      static_cast<unsigned>(c.number("sources")),
                      static_cast<unsigned>(c.number("contexts")));
                },
                {}});
}
axi_tb::PlatformSpec PlatformDefinition::defaults(
    const axi_tb::DefaultMap &map) {
  auto spec = axi_tb::default_platform(map);
  spec.devices.push_back({"clint", "riscv-clint", {}});
  spec.devices.push_back({"plic", "riscv-plic", {}});
  spec.map("clint", FUXI_CLINT_BASE, FUXI_CLINT_SIZE);
  spec.map("plic", FUXI_PLIC_BASE, FUXI_PLIC_SIZE);
  spec.connections.push_back(
      {"uart.irq", "plic.source" + std::to_string(FUXI_UART_IRQ)});
  return spec;
}
}  // namespace fuxi_sim
