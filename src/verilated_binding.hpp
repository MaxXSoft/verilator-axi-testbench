#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "axi.hpp"
#include "packed.hpp"

namespace axi_tb {

template <typename Top, std::size_t NumPorts, std::size_t AddressBits,
          std::size_t DataBits, std::size_t IdBits>
class VerilatedAxiBinding {
 public:
  static_assert(NumPorts > 0);
  static_assert(AddressBits == 32 || AddressBits == 64);
  static_assert(DataBits == 32 || DataBits == 64 || DataBits == 128);
  static_assert(IdBits >= 1 && IdBits <= 32);
  static constexpr std::size_t data_bytes = DataBits / 8U;
  using Master = MasterSignals<data_bytes>;
  using Slave = SlaveSignals<data_bytes>;

  [[nodiscard]] static std::array<Master, NumPorts> sample(const Top &top) {
    std::array<Master, NumPorts> result{};
    for (std::size_t port = 0; port < NumPorts; ++port) {
      auto &value = result[port];
      value.aw_valid = packed::bit(top.axi_aw_valid, port);
      value.aw.id = packed::read_u64(top.axi_aw_id, port * IdBits, IdBits);
      value.aw.address =
          packed::read_u64(top.axi_aw_addr, port * AddressBits, AddressBits);
      value.aw.length = static_cast<std::uint8_t>(
          packed::read_u64(top.axi_aw_len, port * 8U, 8));
      value.aw.size = static_cast<std::uint8_t>(
          packed::read_u64(top.axi_aw_size, port * 3U, 3));
      value.aw.burst =
          static_cast<Burst>(packed::read_u64(top.axi_aw_burst, port * 2U, 2));
      value.aw.lock = packed::bit(top.axi_aw_lock, port);
      value.aw.cache = static_cast<std::uint8_t>(
          packed::read_u64(top.axi_aw_cache, port * 4U, 4));
      value.aw.protection = static_cast<std::uint8_t>(
          packed::read_u64(top.axi_aw_prot, port * 3U, 3));

      value.w_valid = packed::bit(top.axi_w_valid, port);
      value.w.data =
          packed::read_bytes<data_bytes>(top.axi_w_data, port * DataBits);
      const auto strobe =
          packed::read_u64(top.axi_w_strb, port * data_bytes, data_bytes);
      for (std::size_t lane = 0; lane < data_bytes; ++lane) {
        value.w.strobe[lane] = (strobe >> lane) & 1U;
      }
      value.w.last = packed::bit(top.axi_w_last, port);
      value.b_ready = packed::bit(top.axi_b_ready, port);

      value.ar_valid = packed::bit(top.axi_ar_valid, port);
      value.ar.id = packed::read_u64(top.axi_ar_id, port * IdBits, IdBits);
      value.ar.address =
          packed::read_u64(top.axi_ar_addr, port * AddressBits, AddressBits);
      value.ar.length = static_cast<std::uint8_t>(
          packed::read_u64(top.axi_ar_len, port * 8U, 8));
      value.ar.size = static_cast<std::uint8_t>(
          packed::read_u64(top.axi_ar_size, port * 3U, 3));
      value.ar.burst =
          static_cast<Burst>(packed::read_u64(top.axi_ar_burst, port * 2U, 2));
      value.ar.lock = packed::bit(top.axi_ar_lock, port);
      value.ar.cache = static_cast<std::uint8_t>(
          packed::read_u64(top.axi_ar_cache, port * 4U, 4));
      value.ar.protection = static_cast<std::uint8_t>(
          packed::read_u64(top.axi_ar_prot, port * 3U, 3));
      value.r_ready = packed::bit(top.axi_r_ready, port);
    }
    return result;
  }

  static void drive(Top &top, const std::array<Slave, NumPorts> &values) {
    for (std::size_t port = 0; port < NumPorts; ++port) {
      const auto &value = values[port];
      packed::set_bit(top.axi_aw_ready, port, value.aw_ready);
      packed::set_bit(top.axi_w_ready, port, value.w_ready);
      packed::set_bit(top.axi_b_valid, port, value.b_valid);
      packed::write_u64(top.axi_b_id, port * IdBits, IdBits, value.b.id);
      packed::write_u64(top.axi_b_resp, port * 2U, 2,
                        static_cast<std::uint8_t>(value.b.response));
      packed::set_bit(top.axi_ar_ready, port, value.ar_ready);
      packed::set_bit(top.axi_r_valid, port, value.r_valid);
      packed::write_u64(top.axi_r_id, port * IdBits, IdBits, value.r.id);
      packed::write_bytes(top.axi_r_data, port * DataBits, value.r.data);
      packed::write_u64(top.axi_r_resp, port * 2U, 2,
                        static_cast<std::uint8_t>(value.r.response));
      packed::set_bit(top.axi_r_last, port, value.r.last);
    }
  }
};

}  // namespace axi_tb
