#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>

#include "axi.hpp"
#include "device.hpp"

namespace axi_tb {

template <std::size_t NumPorts, std::size_t AddressBits, std::size_t DataBits,
          std::size_t IdBits, std::size_t AddressDepth = 16,
          std::size_t WriteDataDepth = 512, std::size_t ResponseDepth = 4,
          std::size_t RouteDepth = 64>
class AxiFabric {
 public:
  static_assert(NumPorts > 0);
  static_assert(AddressBits == 32 || AddressBits == 64);
  static_assert(DataBits == 32 || DataBits == 64 || DataBits == 128);
  static_assert(IdBits >= 1 && IdBits <= 32);
  static constexpr std::size_t data_bytes = DataBits / 8U;
  static constexpr std::size_t max_burst_beats = 256;
  static constexpr std::size_t max_exclusive_beats = 16;
  using Master = MasterSignals<data_bytes>;
  using Slave = SlaveSignals<data_bytes>;
  using WriteBeat = WriteDataPayload<data_bytes>;
  using ReadBeat = ReadDataPayload<data_bytes>;

  explicit AxiFabric(AddressSpace &address_space)
      : address_space_(address_space) {}

  void set_seed(std::uint64_t seed) noexcept {
    random_state_ = seed == 0 ? 0x9e3779b97f4a7c15ULL : seed;
  }

  void set_stall_probability(double probability) {
    if (!std::isfinite(probability) || probability < 0.0 || probability > 1.0) {
      throw std::invalid_argument("stall probability must be in [0, 1]");
    }
    stall_probability_ = probability;
  }

  [[nodiscard]] const std::array<Slave, NumPorts> &drive(bool reset_active) {
    outputs_ = {};
    if (reset_active) {
      return outputs_;
    }
    for (std::size_t port = 0; port < NumPorts; ++port) {
      auto &output = outputs_[port];
      const auto &state = ports_[port];
      output.aw_ready = !state.aw.full() && allow_ready();
      output.w_ready = !state.w.full() && allow_ready();
      output.ar_ready = !state.ar.full() && allow_ready();
      if (!state.b.empty()) {
        output.b_valid = true;
        output.b = state.b.front();
      }
      if (!state.r.empty()) {
        output.r_valid = true;
        output.r = state.r.front();
      }
    }
    return outputs_;
  }

  void commit(const std::array<Master, NumPorts> &inputs, bool reset_active) {
    if (reset_active) {
      if (!was_reset_) {
        reset_state();
        address_space_.reset();
      }
      was_reset_ = true;
      ++cycle_;
      return;
    }
    was_reset_ = false;

    check_stability(inputs);
    retire_responses(inputs);
    accept_ingress(inputs);
    arbitrate_addresses();
    suppress_read_this_cycle_ = false;
    process_write_beat();
    if (!suppress_read_this_cycle_) {
      process_read_beat();
    }
    ++cycle_;
  }

  [[nodiscard]] std::uint64_t cycle() const noexcept { return cycle_; }
  [[nodiscard]] bool exit_completed() const noexcept { return exit_completed_; }
  [[nodiscard]] std::uint32_t exit_code() const noexcept { return exit_code_; }
  void clear_exit_completed() noexcept {
    exit_completed_ = false;
    exit_code_ = 0;
  }

  [[nodiscard]] bool idle() const noexcept {
    if (!read_routes_.empty() || !write_routes_.empty()) {
      return false;
    }
    return std::ranges::all_of(ports_, [](const PortState &port) {
      return port.aw.empty() && port.w.empty() && port.ar.empty() &&
             port.b.empty() && port.r.empty();
    });
  }

 private:
  struct AddressWatch {
    bool blocked = false;
    AddressPayload payload{};
  };

  struct WriteWatch {
    bool blocked = false;
    WriteBeat payload{};
  };

  struct PortState {
    RingBuffer<AddressDepth, AddressPayload> aw;
    RingBuffer<WriteDataDepth, WriteBeat> w;
    RingBuffer<AddressDepth, AddressPayload> ar;
    RingBuffer<ResponseDepth, WriteResponsePayload> b;
    RingBuffer<ResponseDepth, ReadBeat> r;
    AddressWatch aw_watch;
    WriteWatch w_watch;
    AddressWatch ar_watch;
  };

  struct RouteBase {
    std::size_t port = 0;
    AddressPayload payload{};
    std::size_t beat = 0;
    AddressSpace::Mapping *mapping = nullptr;
    Response forced_response = Response::okay;
    bool exclusive = false;
  };

  struct ReadRoute : RouteBase {};

  struct WriteRoute : RouteBase {
    Response response = Response::okay;
    bool exit_response = false;
    std::uint32_t exit_code = 0;
    std::size_t commit_beat = 0;
    bool data_complete = false;
    std::array<std::array<std::byte, data_bytes>, max_burst_beats>
        staged_data{};
    std::array<std::array<std::uint8_t, data_bytes>, max_burst_beats>
        staged_strobe{};
  };

  struct ExclusiveMonitor {
    bool valid = false;
    std::size_t port = 0;
    std::uint64_t id = 0;
    AddressPayload payload{};
    std::uint64_t first = 0;
    std::uint64_t last = 0;
  };

  [[nodiscard]] bool allow_ready() noexcept {
    if (stall_probability_ <= 0.0) {
      return true;
    }
    if (stall_probability_ >= 1.0) {
      return false;
    }
    random_state_ ^= random_state_ << 13U;
    random_state_ ^= random_state_ >> 7U;
    random_state_ ^= random_state_ << 17U;
    const double normalized =
        static_cast<double>(random_state_) /
        static_cast<double>(std::numeric_limits<std::uint64_t>::max());
    return normalized >= stall_probability_;
  }

  [[noreturn]] void protocol_error(
      std::size_t port, std::string_view channel, const std::string &detail,
      const AddressPayload *payload = nullptr) const {
    std::ostringstream stream;
    stream << "cycle " << cycle_ << ", AXI port " << port << ", " << channel;
    if (payload != nullptr) {
      stream << ", ID 0x" << std::hex << payload->id << ", address 0x"
             << payload->address << std::dec;
    }
    stream << ": " << detail;
    throw ProtocolError(stream.str());
  }

  void check_stability(const std::array<Master, NumPorts> &inputs) {
    for (std::size_t port = 0; port < NumPorts; ++port) {
      auto &state = ports_[port];
      const auto &input = inputs[port];
      check_address_watch(port, "AW", state.aw_watch, input.aw_valid,
                          outputs_[port].aw_ready, input.aw);
      if (state.w_watch.blocked &&
          (!input.w_valid || !(input.w == state.w_watch.payload))) {
        protocol_error(port, "W", "VALID or payload changed while stalled");
      }
      state.w_watch.blocked = input.w_valid && !outputs_[port].w_ready;
      if (state.w_watch.blocked) {
        state.w_watch.payload = input.w;
      }
      check_address_watch(port, "AR", state.ar_watch, input.ar_valid,
                          outputs_[port].ar_ready, input.ar);
    }
  }

  void check_address_watch(std::size_t port, std::string_view channel,
                           AddressWatch &watch, bool valid, bool ready,
                           const AddressPayload &payload) {
    if (watch.blocked && (!valid || !(payload == watch.payload))) {
      protocol_error(port, channel, "VALID or payload changed while stalled",
                     &watch.payload);
    }
    watch.blocked = valid && !ready;
    if (watch.blocked) {
      watch.payload = payload;
    }
  }

  void retire_responses(const std::array<Master, NumPorts> &inputs) {
    for (std::size_t port = 0; port < NumPorts; ++port) {
      auto &state = ports_[port];
      if (outputs_[port].b_valid && inputs[port].b_ready) {
        if (state.b.front().exit_response && !exit_completed_) {
          exit_completed_ = true;
          exit_code_ = state.b.front().exit_code;
        }
        state.b.pop();
      }
      if (outputs_[port].r_valid && inputs[port].r_ready) {
        state.r.pop();
      }
    }
  }

  void accept_ingress(const std::array<Master, NumPorts> &inputs) {
    for (std::size_t port = 0; port < NumPorts; ++port) {
      auto &state = ports_[port];
      const auto &input = inputs[port];
      if (input.aw_valid && outputs_[port].aw_ready &&
          !state.aw.push(input.aw)) {
        protocol_error(port, "AW", "internal address FIFO overflow", &input.aw);
      }
      if (input.w_valid && outputs_[port].w_ready && !state.w.push(input.w)) {
        protocol_error(port, "W", "internal write-data FIFO overflow");
      }
      if (input.ar_valid && outputs_[port].ar_ready &&
          !state.ar.push(input.ar)) {
        protocol_error(port, "AR", "internal address FIFO overflow", &input.ar);
      }
    }
  }

  void arbitrate_addresses() {
    if (!write_routes_.full()) {
      for (std::size_t offset = 0; offset < NumPorts; ++offset) {
        const auto port = (next_aw_port_ + offset) % NumPorts;
        if (!ports_[port].aw.empty()) {
          WriteRoute route;
          initialize_route(route, port, ports_[port].aw.front(), "AW");
          ports_[port].aw.pop();
          write_routes_.push(std::move(route));
          next_aw_port_ = (port + 1) % NumPorts;
          break;
        }
      }
    }
    if (!read_routes_.full()) {
      for (std::size_t offset = 0; offset < NumPorts; ++offset) {
        const auto port = (next_ar_port_ + offset) % NumPorts;
        if (!ports_[port].ar.empty()) {
          ReadRoute route;
          initialize_route(route, port, ports_[port].ar.front(), "AR");
          ports_[port].ar.pop();
          read_routes_.push(std::move(route));
          next_ar_port_ = (port + 1) % NumPorts;
          break;
        }
      }
    }
  }

  template <typename Route>
  void initialize_route(Route &route, std::size_t port,
                        const AddressPayload &payload,
                        std::string_view channel) {
    route.port = port;
    route.payload = payload;
    route.exclusive = payload.lock;
    const auto cursor = make_cursor(port, channel, payload);
    AddressSpace::Mapping *mapping = nullptr;
    bool decode_error = false;
    std::uint64_t address_limit = UINT64_MAX;
    if constexpr (AddressBits < 64) {
      address_limit = (std::uint64_t{1} << AddressBits) - 1U;
    }
    for (std::size_t beat = 0; beat < cursor.beats(); ++beat) {
      const auto base = cursor.bus_word_address(beat);
      const auto mask = cursor.lane_mask(beat);
      for (std::size_t lane = 0; lane < data_bytes; ++lane) {
        if (mask[lane] == 0) {
          continue;
        }
        if (base > address_limit || lane > address_limit - base) {
          protocol_error(port, channel, "address overflows configured width",
                         &payload);
        }
        auto *current = address_space_.resolve(base + lane, 1);
        if (current == nullptr || (mapping != nullptr && current != mapping)) {
          decode_error = true;
        } else if (mapping == nullptr) {
          mapping = current;
        }
      }
    }
    route.mapping = mapping;
    if (decode_error || mapping == nullptr) {
      route.mapping = nullptr;
      route.forced_response = Response::decode_error;
    } else if (cursor.beats() > 1 && !mapping->device->supports_burst()) {
      route.forced_response = Response::slave_error;
    }
    if (payload.lock) {
      validate_exclusive(port, channel, payload, cursor);
      if (route.mapping == nullptr ||
          !route.mapping->device->supports_exclusive()) {
        route.forced_response = Response::slave_error;
      }
    }
  }

  [[nodiscard]] BurstCursor<data_bytes> make_cursor(
      std::size_t port, std::string_view channel,
      const AddressPayload &payload) const {
    try {
      BurstCursor<data_bytes> cursor(payload);
      cursor.validate_4k_boundary();
      return cursor;
    } catch (const ProtocolError &error) {
      protocol_error(port, channel, error.what(), &payload);
    }
  }

  void validate_exclusive(std::size_t port, std::string_view channel,
                          const AddressPayload &payload,
                          const BurstCursor<data_bytes> &cursor) const {
    const auto total = cursor.beats() * cursor.beat_bytes();
    if (payload.burst != Burst::increment ||
        cursor.beats() > max_exclusive_beats || total == 0 || total > 128 ||
        (total & (total - 1U)) != 0 || (payload.address & (total - 1U)) != 0) {
      protocol_error(port, channel, "illegal AXI exclusive transfer shape",
                     &payload);
    }
  }

  void process_read_beat() {
    if (read_routes_.empty()) {
      return;
    }
    auto &route = read_routes_.front();
    auto &destination = ports_[route.port].r;
    if (destination.full()) {
      return;
    }
    const BurstCursor<data_bytes> cursor(route.payload);
    if (route.exclusive && route.beat == 0 &&
        route.forced_response == Response::okay) {
      establish_monitor(route);
    }

    ReadBeat response;
    response.id = route.payload.id;
    response.last = route.beat + 1U == cursor.beats();
    response.response = route.forced_response;
    if (route.forced_response == Response::okay && route.mapping != nullptr) {
      const auto mask = cursor.lane_mask(route.beat);
      const auto base = cursor.bus_word_address(route.beat);
      std::array<std::byte, data_bytes> data{};
      const auto [first, count] = enabled_extent(mask);
      if (count != 0) {
        auto data_span = std::span<std::byte>(data).subspan(first, count);
        auto enable_span =
            std::span<const std::uint8_t>(mask).subspan(first, count);
        const auto device_offset = base + first - route.mapping->base;
        response.response =
            route.mapping->device->read(device_offset, data_span, enable_span);
      }
      for (std::size_t lane = 0; lane < data_bytes; ++lane) {
        response.data[lane] = static_cast<std::uint8_t>(data[lane]);
      }
      if (route.exclusive && response.response == Response::okay) {
        response.response = Response::exclusive_okay;
      }
    }
    destination.push(response);
    ++route.beat;
    if (route.beat == cursor.beats()) {
      read_routes_.pop();
    }
  }

  void process_write_beat() {
    if (write_routes_.empty()) {
      return;
    }
    auto &route = write_routes_.front();
    auto &source = ports_[route.port].w;
    const BurstCursor<data_bytes> cursor(route.payload);
    if (route.data_complete) {
      process_write_commit(route, cursor);
      return;
    }
    const bool final = route.beat + 1U == cursor.beats();
    if (source.empty()) {
      return;
    }
    const auto beat = source.front();
    if (beat.last != final) {
      protocol_error(route.port, "W",
                     final ? "WLAST missing on final beat"
                           : "WLAST asserted before final beat",
                     &route.payload);
    }
    const auto lanes = cursor.lane_mask(route.beat);
    for (std::size_t lane = 0; lane < data_bytes; ++lane) {
      if (beat.strobe[lane] != 0 && lanes[lane] == 0) {
        protocol_error(route.port, "W", "WSTRB selects an illegal byte lane",
                       &route.payload);
      }
    }

    std::array<std::byte, data_bytes> data{};
    for (std::size_t lane = 0; lane < data_bytes; ++lane) {
      data[lane] = static_cast<std::byte>(beat.data[lane]);
    }
    // Do not expose any beat to a device until WLAST and every WSTRB in the
    // complete burst have been validated.  This also makes reset-before-WLAST
    // discard a write without rolling memory back.
    route.staged_data[route.beat] = data;
    route.staged_strobe[route.beat] = beat.strobe;
    source.pop();
    ++route.beat;

    if (!final) {
      return;
    }
    route.data_complete = true;
    process_write_commit(route, cursor);
  }

  void process_write_commit(WriteRoute &route,
                            const BurstCursor<data_bytes> &cursor) {
    auto &destination = ports_[route.port].b;
    if (destination.full()) {
      return;
    }
    if (route.exclusive) {
      suppress_read_this_cycle_ = true;
      finish_exclusive_write(route, cursor);
      complete_write_route(route);
      return;
    }
    if (route.forced_response != Response::okay || route.mapping == nullptr) {
      route.response = combine_response(route.response, route.forced_response);
      complete_write_route(route);
      return;
    }

    suppress_read_this_cycle_ = true;
    const auto response = write_device_beat(
        route, cursor, route.commit_beat, route.staged_data[route.commit_beat],
        route.staged_strobe[route.commit_beat]);
    route.response = combine_response(route.response, response);
    ++route.commit_beat;
    if (!response_is_success(response)) {
      route.commit_beat = cursor.beats();
    }
    if (route.commit_beat == cursor.beats()) {
      complete_write_route(route);
    }
  }

  void complete_write_route(const WriteRoute &route) {
    WriteResponsePayload response;
    response.id = route.payload.id;
    response.response = route.response;
    response.exit_response = route.exit_response;
    response.exit_code = route.exit_code;
    ports_[route.port].b.push(response);
    write_routes_.pop();
  }

  [[nodiscard]] Response write_device_beat(
      WriteRoute &route, const BurstCursor<data_bytes> &cursor,
      std::size_t beat_index, const std::array<std::byte, data_bytes> &data,
      const std::array<std::uint8_t, data_bytes> &strobe) {
    const auto base = cursor.bus_word_address(beat_index);
    const auto lanes = cursor.lane_mask(beat_index);
    std::array<std::uint8_t, data_bytes> effective{};
    for (std::size_t lane = 0; lane < data_bytes; ++lane) {
      effective[lane] = lanes[lane] && strobe[lane] ? 1U : 0U;
    }
    const auto [first, count] = enabled_extent(lanes);
    if (count == 0) {
      return Response::okay;
    }
    const auto device_offset = base + first - route.mapping->base;
    const auto response = route.mapping->device->write(
        device_offset, std::span<const std::byte>(data).subspan(first, count),
        std::span<const std::uint8_t>(effective).subspan(first, count));
    if (response_is_success(response)) {
      for (std::size_t lane = first; lane < first + count; ++lane) {
        if (effective[lane] != 0) {
          invalidate_monitors(base + lane);
        }
      }
      if (route.mapping->device->is_exit()) {
        route.exit_response = true;
        route.exit_code = route.mapping->device->exit_code();
      }
    }
    return response;
  }

  void finish_exclusive_write(WriteRoute &route,
                              const BurstCursor<data_bytes> &cursor) {
    if (route.forced_response != Response::okay || route.mapping == nullptr) {
      route.response = route.forced_response;
      clear_monitor(route.port, route.payload.id);
      return;
    }
    auto *monitor = find_monitor(route.port, route.payload.id);
    if (monitor == nullptr || !(monitor->payload == route.payload)) {
      route.response = Response::okay;
      clear_monitor(route.port, route.payload.id);
      return;
    }
    constexpr std::size_t max_exclusive_bytes = 128;
    std::array<std::byte, max_exclusive_bytes> atomic_data{};
    std::array<std::uint8_t, max_exclusive_bytes> atomic_strobe{};
    const std::size_t total = cursor.beats() * cursor.beat_bytes();
    for (std::size_t beat = 0; beat < cursor.beats(); ++beat) {
      const auto base = cursor.bus_word_address(beat);
      const auto lanes = cursor.lane_mask(beat);
      for (std::size_t lane = 0; lane < data_bytes; ++lane) {
        if (lanes[lane] == 0) {
          continue;
        }
        const auto index =
            static_cast<std::size_t>(base + lane - route.payload.address);
        atomic_data[index] = route.staged_data[beat][lane];
        atomic_strobe[index] = route.staged_strobe[beat][lane] != 0 ? 1U : 0U;
      }
    }
    const auto response = route.mapping->device->write(
        route.payload.address - route.mapping->base,
        std::span<const std::byte>(atomic_data).first(total),
        std::span<const std::uint8_t>(atomic_strobe).first(total));
    route.response =
        response_is_success(response) ? Response::exclusive_okay : response;
    if (response_is_success(response)) {
      for (std::size_t index = 0; index < total; ++index) {
        if (atomic_strobe[index] != 0) {
          invalidate_monitors(route.payload.address + index);
        }
      }
    }
    clear_monitor(route.port, route.payload.id);
  }

  template <std::size_t Size>
  [[nodiscard]] static std::pair<std::size_t, std::size_t> enabled_extent(
      const std::array<std::uint8_t, Size> &mask) {
    std::size_t first = Size;
    std::size_t last = 0;
    for (std::size_t index = 0; index < Size; ++index) {
      if (mask[index] != 0) {
        first = std::min(first, index);
        last = index + 1;
      }
    }
    return first == Size
               ? std::pair<std::size_t, std::size_t>{0, 0}
               : std::pair<std::size_t, std::size_t>{first, last - first};
  }

  void establish_monitor(const ReadRoute &route) {
    const BurstCursor<data_bytes> cursor(route.payload);
    const auto total = cursor.beats() * cursor.beat_bytes();
    ExclusiveMonitor *slot = find_monitor(route.port, route.payload.id);
    if (slot == nullptr) {
      for (auto &monitor : monitors_) {
        if (!monitor.valid) {
          slot = &monitor;
          break;
        }
      }
    }
    if (slot == nullptr) {
      protocol_error(route.port, "AR", "exclusive monitor table exhausted",
                     &route.payload);
    }
    *slot = ExclusiveMonitor{true,
                             route.port,
                             route.payload.id,
                             route.payload,
                             route.payload.address,
                             route.payload.address + total};
  }

  [[nodiscard]] ExclusiveMonitor *find_monitor(std::size_t port,
                                               std::uint64_t id) {
    for (auto &monitor : monitors_) {
      if (monitor.valid && monitor.port == port && monitor.id == id) {
        return &monitor;
      }
    }
    return nullptr;
  }

  void clear_monitor(std::size_t port, std::uint64_t id) {
    if (auto *monitor = find_monitor(port, id); monitor != nullptr) {
      monitor->valid = false;
    }
  }

  void invalidate_monitors(std::uint64_t byte_address) noexcept {
    for (auto &monitor : monitors_) {
      if (monitor.valid && byte_address >= monitor.first &&
          byte_address < monitor.last) {
        monitor.valid = false;
      }
    }
  }

  void reset_state() noexcept {
    for (auto &port : ports_) {
      port.aw.clear();
      port.w.clear();
      port.ar.clear();
      port.b.clear();
      port.r.clear();
      port.aw_watch = {};
      port.w_watch = {};
      port.ar_watch = {};
    }
    read_routes_.clear();
    write_routes_.clear();
    for (auto &monitor : monitors_) {
      monitor = {};
    }
    next_aw_port_ = 0;
    next_ar_port_ = 0;
    suppress_read_this_cycle_ = false;
    exit_completed_ = false;
    exit_code_ = 0;
  }

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
  AddressSpace &address_space_;
  std::array<PortState, NumPorts> ports_{};
  std::array<Slave, NumPorts> outputs_{};
  RingBuffer<RouteDepth, ReadRoute> read_routes_;
  RingBuffer<RouteDepth, WriteRoute> write_routes_;
  std::array<ExclusiveMonitor, RouteDepth> monitors_{};
  std::size_t next_aw_port_ = 0;
  std::size_t next_ar_port_ = 0;
  std::uint64_t cycle_ = 0;
  std::uint64_t random_state_ = 0x9e3779b97f4a7c15ULL;
  double stall_probability_ = 0.0;
  bool was_reset_ = false;
  bool suppress_read_this_cycle_ = false;
  bool exit_completed_ = false;
  std::uint32_t exit_code_ = 0;
};

}  // namespace axi_tb
