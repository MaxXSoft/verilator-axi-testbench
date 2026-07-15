#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "Vaxi_tb_dut.h"
#include "config.hpp"
#include "devices.hpp"
#include "elf_loader.hpp"
#include "fabric.hpp"
#include "verilated.h"
#include "verilated_binding.hpp"

#if AXI_TB_TRACE_ENABLED
#if AXI_TB_TRACE_FST_ENABLED
#include "verilated_fst_c.h"
#else
#include "verilated_vcd_c.h"
#endif
#endif

#if defined(__unix__) || defined(__APPLE__)
#include <termios.h>
#include <unistd.h>
#endif

namespace {
std::uint64_t simulation_time = 0;
}

// Legacy RTL can contain $time even when the model itself is context-based.
// Verilator's compatibility path still looks for this global callback.
double sc_time_stamp() { return static_cast<double>(simulation_time); }

namespace {

constexpr int configuration_error = 2;
constexpr int protocol_error = 3;
constexpr int timeout_error = 124;

#if AXI_TB_TRACE_ENABLED
#if AXI_TB_TRACE_FST_ENABLED
using TraceWriter = VerilatedFstC;
#else
using TraceWriter = VerilatedVcdC;
#endif
#endif

volatile std::sig_atomic_t interrupted = 0;

void handle_interrupt(int) { interrupted = 1; }

struct Options {
  std::optional<std::filesystem::path> elf;
  std::optional<std::filesystem::path> rom_image;
  std::optional<std::filesystem::path> ram_image;
  std::optional<std::filesystem::path> uart_input;
  std::optional<std::filesystem::path> uart_output;
  std::optional<std::filesystem::path> trace;
  std::uint64_t max_cycles = 10'000'000;
  std::uint64_t reset_cycles = 5;
  std::uint64_t seed = 1;
  double stall_probability = 0.0;
  bool help = false;
};

[[nodiscard]] std::uint64_t parse_unsigned(std::string_view text,
                                           std::string_view option) {
  if (text.empty() || text.front() == '-') {
    throw std::invalid_argument(std::string(option) +
                                " expects an unsigned integer");
  }
  std::string copy(text);
  char *end = nullptr;
  errno = 0;
  const auto value = std::strtoull(copy.c_str(), &end, 0);
  if (errno == ERANGE || end == copy.c_str() || *end != '\0') {
    throw std::invalid_argument("invalid value for " + std::string(option) +
                                ": " + copy);
  }
  return value;
}

[[nodiscard]] double parse_probability(std::string_view text,
                                       std::string_view option) {
  std::string copy(text);
  char *end = nullptr;
  errno = 0;
  const double value = std::strtod(copy.c_str(), &end);
  if (errno == ERANGE || end == copy.c_str() || *end != '\0' || value < 0.0 ||
      value > 1.0) {
    throw std::invalid_argument("invalid value for " + std::string(option) +
                                ": " + copy);
  }
  return value;
}

[[nodiscard]] Options parse_options(int argc, char **argv) {
  Options options;
  auto argument = [&](int &index, std::string_view name) -> std::string_view {
    if (++index >= argc) {
      throw std::invalid_argument(std::string(name) + " requires an argument");
    }
    return argv[index];
  };
  for (int index = 1; index < argc; ++index) {
    const std::string_view name(argv[index]);
    if (name == "--help" || name == "-h") {
      options.help = true;
    } else if (name == "--elf") {
      options.elf = argument(index, name);
    } else if (name == "--rom-image") {
      options.rom_image = argument(index, name);
    } else if (name == "--ram-image") {
      options.ram_image = argument(index, name);
    } else if (name == "--uart-in") {
      options.uart_input = argument(index, name);
    } else if (name == "--uart-out") {
      options.uart_output = argument(index, name);
    } else if (name == "--trace") {
      options.trace = argument(index, name);
    } else if (name == "--max-cycles") {
      options.max_cycles = parse_unsigned(argument(index, name), name);
    } else if (name == "--reset-cycles") {
      options.reset_cycles = parse_unsigned(argument(index, name), name);
    } else if (name == "--seed") {
      options.seed = parse_unsigned(argument(index, name), name);
    } else if (name == "--stall-probability") {
      options.stall_probability =
          parse_probability(argument(index, name), name);
    } else if (!name.empty() && name.front() == '+') {
      // Verilator/SystemVerilog plusargs are consumed by the model through
      // VerilatedContext::commandArgs().  They intentionally have no generic
      // simulator-side meaning.
    } else {
      throw std::invalid_argument("unknown option: " + std::string(name));
    }
  }
  if (options.elf && (options.rom_image || options.ram_image)) {
    throw std::invalid_argument(
        "--elf cannot be combined with --rom-image or --ram-image");
  }
  if (options.max_cycles == 0) {
    throw std::invalid_argument("--max-cycles must be greater than zero");
  }
#if !AXI_TB_TRACE_ENABLED
  if (options.trace) {
    throw std::invalid_argument(
        "--trace requires tracing to be enabled when configuring the target");
  }
#endif
  return options;
}

void print_help(const char *program) {
  std::cout
      << "Usage: " << program << " [options]\n\n"
      << "Images:\n"
      << "  --elf FILE             Load little-endian ELF32/ELF64 PT_LOADs\n"
      << "  --rom-image FILE       Load a raw image at the ROM base\n"
      << "  --ram-image FILE       Load a raw image at the RAM base\n\n"
      << "Simulation:\n"
      << "  --max-cycles N         Active cycles before timeout (default "
         "10000000)\n"
      << "  --reset-cycles N       Reset rising edges (default 5)\n"
      << "  --seed N               Random-stall seed (default 1)\n"
      << "  --stall-probability P  AW/W/AR READY stall probability [0,1]\n"
      << "  --trace FILE           Write the build-selected VCD or FST trace\n"
      << "  +NAME[=VALUE]          Pass a plusarg through to the RTL model\n\n"
      << "UART:\n"
      << "  --uart-in FILE|-       Input bytes (default stdin)\n"
      << "  --uart-out FILE|-      Output bytes (default stdout)\n";
}

struct FileCloser {
  void operator()(std::FILE *file) const noexcept {
    if (file != nullptr) {
      std::fclose(file);
    }
  }
};

using OwnedFile = std::unique_ptr<std::FILE, FileCloser>;

[[nodiscard]] std::FILE *open_file(
    const std::optional<std::filesystem::path> &path, const char *mode,
    std::FILE *standard, OwnedFile &owner, std::string_view description) {
  if (!path || path->string() == "-") {
    return standard;
  }
  owner.reset(std::fopen(path->string().c_str(), mode));
  if (!owner) {
    throw std::runtime_error("cannot open " + std::string(description) + " '" +
                             path->string() + "': " + std::strerror(errno));
  }
  return owner.get();
}

class TerminalGuard {
 public:
  explicit TerminalGuard(std::FILE *input) {
#if defined(__unix__) || defined(__APPLE__)
    descriptor_ = input == nullptr ? -1 : ::fileno(input);
    if (descriptor_ >= 0 && ::isatty(descriptor_) != 0 &&
        ::tcgetattr(descriptor_, &original_) == 0) {
      termios raw = original_;
      raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
      raw.c_cc[VMIN] = 0;
      raw.c_cc[VTIME] = 0;
      active_ = ::tcsetattr(descriptor_, TCSANOW, &raw) == 0;
    }
#else
    (void)input;
#endif
  }

  TerminalGuard(const TerminalGuard &) = delete;
  TerminalGuard &operator=(const TerminalGuard &) = delete;

  ~TerminalGuard() {
#if defined(__unix__) || defined(__APPLE__)
    if (active_) {
      ::tcsetattr(descriptor_, TCSANOW, &original_);
    }
#endif
  }

 private:
#if defined(__unix__) || defined(__APPLE__)
  int descriptor_ = -1;
  termios original_{};
  bool active_ = false;
#endif
};

template <typename Top, typename Trace>
void evaluate(VerilatedContext &context, Top &top, Trace *trace) {
  top.eval();
  if (trace != nullptr) {
    trace->dump(context.time());
  }
  context.timeInc(1);
  ++simulation_time;
}

struct NullTrace {
  void dump(std::uint64_t) noexcept {}
  void close() noexcept {}
};

int run_simulation(int argc, char **argv, const Options &options) {
  if (axi_tb::config::rom_size > std::numeric_limits<std::size_t>::max() ||
      axi_tb::config::ram_size > std::numeric_limits<std::size_t>::max()) {
    throw std::runtime_error(
        "configured ROM or RAM is too large for this host");
  }

  axi_tb::RomDevice rom(static_cast<std::size_t>(axi_tb::config::rom_size));
  axi_tb::RamDevice ram(static_cast<std::size_t>(axi_tb::config::ram_size));
  OwnedFile input_owner;
  OwnedFile output_owner;
  std::FILE *input =
      open_file(options.uart_input, "rb", stdin, input_owner, "UART input");
  std::FILE *output =
      open_file(options.uart_output, "wb", stdout, output_owner, "UART output");
  TerminalGuard terminal(input);
  axi_tb::FileUartBackend backend(input, output);
  axi_tb::UartDevice uart(backend);
  axi_tb::ExitDevice exit;
  axi_tb::AddressSpace address_space;
  address_space.map(axi_tb::config::rom_base, axi_tb::config::rom_size, rom,
                    "rom");
  address_space.map(axi_tb::config::ram_base, axi_tb::config::ram_size, ram,
                    "ram");
  address_space.map(axi_tb::config::uart_base, axi_tb::config::uart_size, uart,
                    "uart");
  address_space.map(axi_tb::config::exit_base, axi_tb::config::exit_size, exit,
                    "exit");

  if (options.elf) {
    const auto loaded = axi_tb::load_elf(*options.elf, address_space);
    std::cerr << "[axi-tb] loaded " << loaded.segments.size()
              << " ELF segment(s), entry=0x" << std::hex << loaded.entry
              << std::dec << '\n';
  } else {
    if (options.rom_image) {
      axi_tb::load_raw_image(*options.rom_image, address_space,
                             axi_tb::config::rom_base);
    }
    if (options.ram_image) {
      axi_tb::load_raw_image(*options.ram_image, address_space,
                             axi_tb::config::ram_base);
    }
  }

  using Binding = axi_tb::VerilatedAxiBinding<
      Vaxi_tb_dut, axi_tb::config::num_ports, axi_tb::config::address_bits,
      axi_tb::config::data_bits, axi_tb::config::id_bits>;
  using Fabric =
      axi_tb::AxiFabric<axi_tb::config::num_ports, axi_tb::config::address_bits,
                        axi_tb::config::data_bits, axi_tb::config::id_bits>;

  VerilatedContext context;
  simulation_time = 0;
  context.commandArgs(argc, argv);
  context.threads(axi_tb::config::threads);
  Vaxi_tb_dut top(&context);
  if (top.threads() != axi_tb::config::threads) {
    throw std::runtime_error(
        "generated Verilator model thread count does not match configuration");
  }
  Fabric fabric(address_space);
  fabric.set_seed(options.seed);
  fabric.set_stall_probability(options.stall_probability);

#if AXI_TB_TRACE_ENABLED
  std::unique_ptr<TraceWriter> trace;
  if (options.trace) {
    context.traceEverOn(true);
    trace = std::make_unique<TraceWriter>();
    top.trace(trace.get(), 99);
    trace->open(options.trace->string().c_str());
    if (!trace->isOpen()) {
      throw std::runtime_error("cannot open trace output: " +
                               options.trace->string());
    }
  }
  TraceWriter *trace_pointer = trace.get();
#else
  NullTrace *trace_pointer = nullptr;
#endif

  std::signal(SIGINT, handle_interrupt);
  std::uint64_t active_cycles = 0;
  std::uint64_t total_cycles = 0;
  try {
    while (active_cycles < options.max_cycles && interrupted == 0 &&
           !context.gotFinish()) {
      const bool reset = total_cycles < options.reset_cycles;
      top.clk = 0;
      top.aresetn = reset ? 0 : 1;
      Binding::drive(top, fabric.drive(reset));
      evaluate(context, top, trace_pointer);
      const auto sampled = Binding::sample(top);
      top.clk = 1;
      evaluate(context, top, trace_pointer);
      fabric.commit(sampled, reset);
      ++total_cycles;
      if (!reset) {
        ++active_cycles;
      }
      if (fabric.exit_completed()) {
        const std::uint32_t guest_code = fabric.exit_code();
        backend.flush();
        std::cerr << "[axi-tb] guest exit code " << guest_code << " (0x"
                  << std::hex << guest_code << std::dec << ") after "
                  << active_cycles << " cycle(s)\n";
        top.final();
        if (trace_pointer != nullptr) {
          trace_pointer->close();
        }
        return guest_code == 0 ? 0 : 1;
      }
    }
    top.final();
    if (trace_pointer != nullptr) {
      trace_pointer->close();
    }
  } catch (...) {
    top.final();
    if (trace_pointer != nullptr) {
      trace_pointer->close();
    }
    throw;
  }

  backend.flush();
  if (interrupted != 0) {
    std::cerr << "[axi-tb] interrupted after " << active_cycles
              << " active cycle(s)\n";
    return 130;
  }
  if (context.gotFinish()) {
    std::cerr << "[axi-tb] DUT called $finish before writing the exit device\n";
    return protocol_error;
  }
  std::cerr << "[axi-tb] timeout after " << active_cycles
            << " active cycle(s)\n";
  return timeout_error;
}

}  // namespace

int main(int argc, char **argv) {
  try {
    const Options options = parse_options(argc, argv);
    if (options.help) {
      print_help(argv[0]);
      return 0;
    }
    return run_simulation(argc, argv, options);
  } catch (const axi_tb::ProtocolError &error) {
    std::cerr << "[axi-tb] AXI protocol error: " << error.what() << '\n';
    return protocol_error;
  } catch (const axi_tb::ElfError &error) {
    std::cerr << "[axi-tb] image error: " << error.what() << '\n';
    return configuration_error;
  } catch (const std::exception &error) {
    std::cerr << "[axi-tb] configuration error: " << error.what() << '\n';
    return configuration_error;
  }
}
