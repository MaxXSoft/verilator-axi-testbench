#include "platform.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

#include "devices.hpp"
#if defined(__unix__) || defined(__APPLE__)
#include <termios.h>
#include <unistd.h>
#endif

namespace axi_tb {
namespace {
std::pair<std::string, std::string> endpoint(std::string_view text) {
  const auto dot = text.find('.');
  if (dot == 0 || dot == std::string_view::npos || dot + 1 == text.size()) {
    throw std::invalid_argument("expected instance.port: " + std::string(text));
  }
  return {std::string(text.substr(0, dot)), std::string(text.substr(dot + 1))};
}
void validate_name(std::string_view name) {
  if (name.empty() ||
      name.find_first_not_of(
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
          std::string_view::npos) {
    throw std::invalid_argument("invalid name: " + std::string(name));
  }
}
}  // namespace

std::uint64_t parse_unsigned(std::string_view text) {
  const auto original = text;
  int base = 10;
  if (text.starts_with("0x") || text.starts_with("0X")) {
    text.remove_prefix(2);
    base = 16;
  }
  std::uint64_t result = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), result, base);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size()) {
    throw std::invalid_argument("invalid unsigned integer: " +
                                std::string(original));
  }
  return result;
}
const std::string &DeviceConfig::text(std::string_view name) const {
  const auto found = values_.find(name);
  if (found == values_.end()) {
    throw std::invalid_argument("unknown device option: " + std::string(name));
  }
  return found->second;
}
std::uint64_t DeviceConfig::number(std::string_view name) const {
  return parse_unsigned(text(name));
}
bool DeviceConfig::boolean(std::string_view name) const {
  return text(name) == "true";
}

struct HostServices::State {
  State() = default;
  State(const State &) = delete;
  State &operator=(const State &) = delete;
  State(State &&) = delete;
  State &operator=(State &&) = delete;
  struct FileCloser {
    void operator()(std::FILE *file) const noexcept {
      // The unique_ptr deleter closes its owned C stream.
      // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
      std::fclose(file);
    }
  };
  using OwnedFile = std::unique_ptr<std::FILE, FileCloser>;
  std::vector<OwnedFile> files;
#if defined(__unix__) || defined(__APPLE__)
  struct Terminal {
    int descriptor;
    termios original;
  };
  std::vector<Terminal> terminals;
#endif
  std::FILE *open(const std::string &path, bool input) {
    std::FILE *file = input ? stdin : stdout;
    if (path != "-") {
      // Take ownership before growing the vector, which can throw.
      // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
      OwnedFile owned(std::fopen(path.c_str(), input ? "rb" : "wb"));
      if (!owned) {
        throw std::runtime_error("cannot open '" + path +
                                 "': " + std::strerror(errno));
      }
      file = owned.get();
      files.push_back(std::move(owned));
    }
    if (input) {
      // poll() must not overlook bytes prefetched into a stdio buffer.
      std::setvbuf(file, nullptr, _IONBF, 0);
#if defined(__unix__) || defined(__APPLE__)
      const int fd = ::fileno(file);
      const bool guarded = std::ranges::any_of(
          terminals, [fd](const Terminal &t) { return t.descriptor == fd; });
      termios original{};
      if (!guarded && ::isatty(fd) && ::tcgetattr(fd, &original) == 0) {
        auto raw = original;
        raw.c_lflag &=
            ~(static_cast<tcflag_t>(ICANON) | static_cast<tcflag_t>(ECHO));
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        terminals.push_back({.descriptor = fd, .original = original});
        if (::tcsetattr(fd, TCSANOW, &raw) != 0) {
          terminals.pop_back();
        }
      }
#endif
    }
    return file;
  }
  ~State() {
#if defined(__unix__) || defined(__APPLE__)
    for (const auto &terminal : terminals) {
      ::tcsetattr(terminal.descriptor, TCSANOW, &terminal.original);
    }
#endif
  }
};
HostServices::HostServices() : state_(std::make_unique<State>()) {}
HostServices::~HostServices() = default;
std::FILE *HostServices::open_input(const std::string &path) {
  return state_->open(path, true);
}
std::FILE *HostServices::open_output(const std::string &path) {
  return state_->open(path, false);
}

void DeviceRegistry::add(std::string name, DeviceType type) {
  validate_name(name);
  if (!type.create) {
    throw std::invalid_argument("missing device factory: " + name);
  }
  std::set<std::string> names;
  for (const auto &option : type.options) {
    validate_name(option.name);
    if (!names.insert(option.name).second) {
      throw std::invalid_argument("duplicate device option: " + option.name);
    }
    if (option.minimum > option.maximum) {
      throw std::invalid_argument("invalid option bounds: " + option.name);
    }
  }
  if (!type.image_option.empty()) {
    const auto found =
        std::ranges::find(type.options, type.image_option, &DeviceOption::name);
    if (found == type.options.end() || found->kind != OptionKind::String) {
      throw std::invalid_argument("image option must be a string property");
    }
  }
  if (!types_.emplace(name, std::move(type)).second) {
    throw std::invalid_argument("duplicate device type: " + name);
  }
}
const DeviceType &DeviceRegistry::type(std::string_view name) const {
  const auto found = types_.find(name);
  if (found == types_.end()) {
    throw std::invalid_argument("unknown device type: " + std::string(name));
  }
  return found->second;
}
DeviceConfig DeviceRegistry::configure(std::string_view name,
                                       const Properties &properties) const {
  const auto &schema = type(name).options;
  Properties result;
  for (const auto &[key, value] : properties) {
    if (std::ranges::find(schema, key, &DeviceOption::name) == schema.end()) {
      throw std::invalid_argument("unknown option for " + std::string(name) +
                                  ": " + key);
    }
  }
  for (const auto &option : schema) {
    const auto found = properties.find(option.name);
    const auto value =
        found == properties.end() ? option.default_value : found->second;
    if (option.kind == OptionKind::Unsigned) {
      const auto number = parse_unsigned(value);
      if (number < option.minimum || number > option.maximum) {
        throw std::invalid_argument("option out of range: " + option.name);
      }
    } else if (option.kind == OptionKind::Boolean && value != "true" &&
               value != "false") {
      throw std::invalid_argument("option expects true or false: " +
                                  option.name);
    }
    result.emplace(option.name, value);
  }
  return DeviceConfig(std::move(result));
}
void DeviceRegistry::print_help(std::ostream &stream) const {
  stream << "\nDevice properties (--set INSTANCE.PROPERTY=VALUE):\n";
  for (const auto &[name, type] : types_) {
    stream << "  " << name << '\n';
    for (const auto &option : type.options) {
      stream << "    " << option.name << " (default '" << option.default_value
             << "'): " << option.help << '\n';
    }
  }
}

DeviceSpec &PlatformSpec::device(std::string_view id) {
  const auto found = std::ranges::find(devices, id, &DeviceSpec::id);
  if (found == devices.end()) {
    throw std::invalid_argument("unknown device instance: " + std::string(id));
  }
  return *found;
}
void PlatformSpec::remove(std::string_view id) {
  (void)device(id);
  std::erase_if(devices, [id](const auto &d) { return d.id == id; });
  std::erase_if(mappings, [id](const auto &m) { return m.device == id; });
  std::erase_if(connections, [id](const auto &c) {
    return endpoint(c.source).first == id ||
           endpoint(c.destination).first == id;
  });
}
void PlatformSpec::map(std::string id, std::uint64_t base, std::uint64_t size) {
  (void)device(id);
  std::erase_if(mappings, [&id](const auto &m) { return m.device == id; });
  mappings.push_back({.device = std::move(id), .base = base, .size = size});
}
void PlatformSpec::set(std::string_view assignment) {
  const auto equal = assignment.find('=');
  if (equal == std::string_view::npos) {
    throw std::invalid_argument("expected instance.option=value");
  }
  auto [id, key] = endpoint(assignment.substr(0, equal));
  device(id).properties[key] = assignment.substr(equal + 1);
}

void register_builtin_devices(DeviceRegistry &registry) {
  const auto memory_options = std::vector<DeviceOption>{
      {
          .name = "size",
          .kind = OptionKind::Unsigned,
          .default_value = "65536",
          .help = "Memory capacity in bytes",
          .minimum = 1,
          .maximum = std::numeric_limits<std::size_t>::max(),
      },
      {
          .name = "image",
          .kind = OptionKind::String,
          .default_value = "",
          .help = "Raw image loaded at the device's sole mapping",
      },
  };
  registry.add("rom",
               {
                   .options = memory_options,
                   .create =
                       [](const DeviceConfig &c, HostServices & /*host*/) {
                         return std::make_unique<RomDevice>(
                             static_cast<std::size_t>(c.number("size")));
                       },
                   .image_option = "image",
               });
  registry.add("ram",
               {
                   .options = memory_options,
                   .create =
                       [](const DeviceConfig &c, HostServices & /*host*/) {
                         return std::make_unique<RamDevice>(
                             static_cast<std::size_t>(c.number("size")));
                       },
                   .image_option = "image",
               });
  registry.add(
      "uart",
      {
          .options =
              {
                  {
                      .name = "input",
                      .kind = OptionKind::String,
                      .default_value = "-",
                      .help = "Input file, or - for stdin",
                  },
                  {
                      .name = "output",
                      .kind = OptionKind::String,
                      .default_value = "-",
                      .help = "Output file, or - for stdout",
                  },
                  {
                      .name = "character-cycles",
                      .kind = OptionKind::Unsigned,
                      .default_value = "16",
                      .help = "Simulated cycles per RX timeout character",
                      .minimum = 1,
                      .maximum = UINT64_MAX / 4,
                  },
                  {
                      .name = "input-poll-cycles",
                      .kind = OptionKind::Unsigned,
                      .default_value = "1024",
                      .help = "Cycles between empty host input polls (1 for "
                              "immediate polling)",
                      .minimum = 1,
                  },
              },
          .create =
              [](const DeviceConfig &c, HostServices &host) {
                auto *input = host.open_input(c.text("input"));
                auto *output = host.open_output(c.text("output"));
                auto uart = std::make_unique<UartDevice>(input, output);
                uart->set_character_cycles(c.number("character-cycles"));
                uart->set_input_poll_cycles(c.number("input-poll-cycles"));
                return uart;
              },
          .image_option = {},
      });
  registry.add("exit", {
                           .options = {},
                           .create =
                               [](const DeviceConfig & /*config*/,
                                  HostServices & /*host*/) {
                                 return std::make_unique<ExitDevice>();
                               },
                           .image_option = {},
                       });
}
PlatformSpec default_platform(const DefaultMap &map) {
  PlatformSpec spec;
  spec.devices = {
      {
          .id = "rom",
          .type = "rom",
          .properties = {{"size", std::to_string(map.rom_size)}},
      },
      {
          .id = "ram",
          .type = "ram",
          .properties = {{"size", std::to_string(map.ram_size)}},
      },
      {.id = "uart", .type = "uart", .properties = {}},
      {.id = "exit", .type = "exit", .properties = {}},
  };
  spec.mappings = {
      {.device = "rom", .base = map.rom_base, .size = map.rom_size},
      {.device = "ram", .base = map.ram_base, .size = map.ram_size},
      {.device = "uart", .base = map.uart_base, .size = map.uart_size},
      {.device = "exit", .base = map.exit_base, .size = map.exit_size},
  };
  return spec;
}

Platform::Platform(const DeviceRegistry &registry, const PlatformSpec &spec,
                   unsigned address_bits) {
  if (address_bits != 32 && address_bits != 64) {
    throw std::invalid_argument("invalid address width");
  }
  // Validate every property before any factory opens files or allocates memory.
  std::map<std::string, DeviceConfig, std::less<>> configs;
  for (const auto &d : spec.devices) {
    validate_name(d.id);
    if (!configs.emplace(d.id, registry.configure(d.type, d.properties))
             .second) {
      throw std::invalid_argument("duplicate device instance: " + d.id);
    }
  }
  for (const auto &d : spec.devices) {
    const auto &type = registry.type(d.type);
    auto created = type.create(configs.at(d.id), host_);
    if (!created) {
      throw std::invalid_argument("factory returned null: " + d.id);
    }
    devices_.emplace(d.id, std::move(created));
  }
  for (const auto &m : spec.mappings) {
    if (address_bits == 32 && (m.base >= (UINT64_C(1) << 32U) ||
                               m.size > (UINT64_C(1) << 32U) - m.base)) {
      throw std::invalid_argument(
          "mapping does not fit 32-bit address space: " + m.device);
    }
    address_space_.map(m.base, m.size, device(m.device), m.device);
  }
  address_space_.freeze();
  for (const auto &d : spec.devices) {
    const auto &key = registry.type(d.type).image_option;
    if (key.empty() || configs.at(d.id).text(key).empty()) {
      continue;
    }
    const auto count =
        std::ranges::count(spec.mappings, d.id, &MappingSpec::device);
    if (count != 1) {
      throw std::invalid_argument(
          "image property requires exactly one mapping: " + d.id);
    }
    const auto m = std::ranges::find(spec.mappings, d.id, &MappingSpec::device);
    images_.push_back({.path = configs.at(d.id).text(key), .address = m->base});
  }
  connect(spec);
  reset();
}

void Platform::connect(const PlatformSpec &spec) {
  std::map<Device *, std::vector<Device *>> successors;
  std::map<Device *, unsigned> indegree;
  for (const auto &d : spec.devices) {
    indegree[&device(d.id)] = 0;
  }
  for (const auto &connection : spec.connections) {
    const auto [src_id, src_port] = endpoint(connection.source);
    const auto [dst_id, dst_port] = endpoint(connection.destination);
    auto &src = device(src_id);
    auto &dst = device(dst_id);
    auto *input = dst.input(dst_port);
    if (!input) {
      throw std::invalid_argument("unknown input: " + connection.destination);
    }
    if (input->source) {
      throw std::invalid_argument("input has multiple drivers: " +
                                  connection.destination);
    }
    input->source = &output(connection.source, input->width);
    successors[&src].push_back(&dst);
    ++indegree[&dst];
  }
  // Stable topological order makes combinational IRQ propagation deterministic.
  for (const auto &d : spec.devices) {
    if (indegree[&device(d.id)] == 0) {
      order_.push_back(&device(d.id));
    }
  }
  for (std::size_t i = 0; i < order_.size(); ++i) {
    for (auto *next : successors[order_[i]]) {
      if (--indegree[next] == 0) {
        order_.push_back(next);
      }
    }
  }
  if (order_.size() != devices_.size()) {
    throw std::invalid_argument("sideband connection cycle");
  }
}
Device &Platform::device(std::string_view id) const {
  const auto found = devices_.find(id);
  if (found == devices_.end()) {
    throw std::invalid_argument("unknown device instance: " + std::string(id));
  }
  return *found->second;
}
const Signal &Platform::output(std::string_view name, unsigned width) const {
  const auto [id, port] = endpoint(name);
  const auto *signal = device(id).output(port);
  if (!signal) {
    throw std::invalid_argument("unknown output: " + std::string(name));
  }
  if (signal->width != width || width == 0 || width > 64) {
    throw std::invalid_argument("signal width mismatch: " + std::string(name));
  }
  return *signal;
}
void Platform::settle() noexcept {
  for (auto *d : order_) {
    d->settle();
  }
}
void Platform::reset() noexcept {
  for (auto *d : order_) {
    d->reset();
  }
  settle();
}
void Platform::begin_cycle(bool reset_active) {
  if (reset_active && !was_reset_) {
    reset();
  }
  was_reset_ = reset_active;
}
void Platform::end_cycle(bool reset_active) {
  if (!reset_active) {
    for (auto *d : order_) {
      d->tick();
    }
    settle();
  }
}
}  // namespace axi_tb
