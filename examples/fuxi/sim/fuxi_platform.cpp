#include "fuxi_platform.hpp"

#include "interrupt_devices.hpp"

namespace fuxi_sim {
void PlatformDefinition::register_devices(axi_tb::DeviceRegistry &registry) {
  using axi_tb::DeviceConfig;
  using axi_tb::HostServices;
  using axi_tb::OptionKind;
  registry.add("riscv-clint",
               {
                   .options =
                       {
                           {
                               .name = "divider",
                               .kind = OptionKind::Unsigned,
                               .default_value = "1",
                               .help = "CPU cycles per mtime tick",
                               .minimum = 1,
                           },
                       },
                   .create =
                       [](const DeviceConfig &c, HostServices & /*host*/) {
                         return std::make_unique<Clint>(c.number("divider"));
                       },
                   .image_option = {},
               });
  registry.add("riscv-plic",
               {
                   .options =
                       {
                           {
                               .name = "sources",
                               .kind = OptionKind::Unsigned,
                               .default_value = "31",
                               .help = "External interrupt sources",
                               .minimum = 1,
                               .maximum = 1023,
                           },
                           {
                               .name = "contexts",
                               .kind = OptionKind::Unsigned,
                               .default_value = "2",
                               .help = "Interrupt target contexts",
                               .minimum = 1,
                               .maximum = 64,
                           },
                       },
                   .create =
                       [](const DeviceConfig &c, HostServices & /*host*/) {
                         return std::make_unique<Plic>(
                             static_cast<unsigned>(c.number("sources")),
                             static_cast<unsigned>(c.number("contexts")));
                       },
                   .image_option = {},
               });
}
axi_tb::PlatformSpec PlatformDefinition::defaults(
    const axi_tb::DefaultMap &map) {
  auto spec = axi_tb::default_platform(map);
  spec.devices.push_back(
      {.id = "clint", .type = "riscv-clint", .properties = {}});
  spec.devices.push_back(
      {.id = "plic", .type = "riscv-plic", .properties = {}});
  spec.map("clint", FUXI_CLINT_BASE, FUXI_CLINT_SIZE);
  spec.map("plic", FUXI_PLIC_BASE, FUXI_PLIC_SIZE);
  spec.connections.push_back({
      .source = "uart.irq",
      .destination = "plic.source" + std::to_string(FUXI_UART_IRQ),
  });
  return spec;
}
}  // namespace fuxi_sim
