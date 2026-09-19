#pragma once

#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "device.hpp"

namespace axi_tb {

using Properties = std::map<std::string, std::string, std::less<>>;
enum class OptionKind { String, Unsigned, Boolean };
struct DeviceOption {
  std::string name;
  OptionKind kind;
  std::string default_value;
  std::string help;
  std::uint64_t minimum = 0;
  std::uint64_t maximum = UINT64_MAX;
};

[[nodiscard]] std::uint64_t parse_unsigned(std::string_view text);

class DeviceConfig {
 public:
  explicit DeviceConfig(Properties values) : values_(std::move(values)) {}
  [[nodiscard]] const std::string &text(std::string_view name) const;
  [[nodiscard]] std::uint64_t number(std::string_view name) const;
  [[nodiscard]] bool boolean(std::string_view name) const;

 private:
  Properties values_;
};

// Resources opened by factories outlive all devices. Standard streams are
// borrowed; other files and terminal mode changes are restored on destruction.
class HostServices {
 public:
  HostServices();
  ~HostServices();
  HostServices(const HostServices &) = delete;
  HostServices &operator=(const HostServices &) = delete;
  std::FILE *open_input(const std::string &path);
  std::FILE *open_output(const std::string &path);

 private:
  struct State;
  std::unique_ptr<State> state_;
};

struct DeviceType {
  std::vector<DeviceOption> options;
  std::function<std::unique_ptr<Device>(const DeviceConfig &, HostServices &)>
      create;
  // Optional string property naming a raw image loaded at this device's
  // mapping.
  std::string image_option;
};

class DeviceRegistry {
 public:
  void add(std::string name, DeviceType type);
  [[nodiscard]] const DeviceType &type(std::string_view name) const;
  [[nodiscard]] DeviceConfig configure(std::string_view type,
                                       const Properties &properties) const;
  void print_help(std::ostream &stream) const;

 private:
  std::map<std::string, DeviceType, std::less<>> types_;
};

struct DeviceSpec {
  std::string id;
  std::string type;
  Properties properties;
};
struct MappingSpec {
  std::string device;
  std::uint64_t base;
  std::uint64_t size;
};
struct ConnectionSpec {
  std::string source;  // instance.port
  std::string destination;
};
struct PlatformSpec {
  std::vector<DeviceSpec> devices;
  std::vector<MappingSpec> mappings;
  std::vector<ConnectionSpec> connections;
  DeviceSpec &device(std::string_view id);
  void remove(std::string_view id);  // also removes mappings and connections
  void map(std::string id, std::uint64_t base, std::uint64_t size);
  void set(std::string_view assignment);  // instance.option=value
};

struct DefaultMap {
  std::uint64_t rom_base = 0, rom_size = 0x10000;
  std::uint64_t ram_base = 0x80000000, ram_size = 0x08000000;
  std::uint64_t uart_base = 0x10000000, uart_size = 0x100;
  std::uint64_t exit_base = 0x10001000, exit_size = 4;
};

void register_builtin_devices(DeviceRegistry &registry);
[[nodiscard]] PlatformSpec default_platform(const DefaultMap &map = {});

class Platform {
 public:
  Platform(const DeviceRegistry &registry, const PlatformSpec &spec,
           unsigned address_bits = 64);
  Platform(const Platform &) = delete;
  Platform &operator=(const Platform &) = delete;
  [[nodiscard]] AddressSpace &address_space() noexcept {
    return address_space_;
  }
  [[nodiscard]] Device &device(std::string_view id) const;
  [[nodiscard]] const Signal &output(std::string_view endpoint,
                                     unsigned width = 1) const;
  void begin_cycle(bool reset);
  void end_cycle(bool reset);
  void reset() noexcept;
  struct RawImage {
    std::string path;
    std::uint64_t address;
  };
  [[nodiscard]] const std::vector<RawImage> &images() const { return images_; }

 private:
  void settle() noexcept;
  HostServices host_;
  std::map<std::string, std::unique_ptr<Device>, std::less<>> devices_;
  std::vector<Device *> order_;
  AddressSpace address_space_;
  std::vector<RawImage> images_;
  bool was_reset_ = false;
};

struct DefaultPlatform {
  static void register_devices(DeviceRegistry &) {}
  static PlatformSpec defaults(const DefaultMap &map) {
    return default_platform(map);
  }
};
struct NoSideband {
  explicit NoSideband(Platform &) {}
  template <typename Top>
  void drive(Top &) const noexcept {}
};

}  // namespace axi_tb
