#pragma once
#include "fuxi_platform.h"
#include "platform.hpp"

namespace fuxi_sim {
struct PlatformDefinition {
  static void register_devices(axi_tb::DeviceRegistry &);
  static axi_tb::PlatformSpec defaults(const axi_tb::DefaultMap &);
};
struct SidebandBinding {
  const axi_tb::Signal &msip, &mtip, &meip, &seip, &time;
  explicit SidebandBinding(axi_tb::Platform &p)
      : msip(p.output("clint.msip")),
        mtip(p.output("clint.mtip")),
        meip(p.output("plic.context0")),
        seip(p.output("plic.context1")),
        time(p.output("clint.mtime", 64)) {}
  template <class Top>
  void drive(Top &top) const noexcept {
    top.irq_msip = msip.value;
    top.irq_mtip = mtip.value;
    top.irq_meip = meip.value;
    top.irq_seip = seip.value;
    top.rtc_time = time.value;
  }
};
}  // namespace fuxi_sim
