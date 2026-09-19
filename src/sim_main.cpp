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
#include "elf_loader.hpp"
#include "fabric.hpp"
#include "platform.hpp"
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

constexpr int CONFIGURATION_ERROR = 2;
constexpr int PROTOCOL_ERROR = 3;
constexpr int TIMEOUT_ERROR = 124;

#if AXI_TB_TRACE_ENABLED
#if AXI_TB_TRACE_FST_ENABLED
using TraceWriter = VerilatedFstC;
#else
using TraceWriter = VerilatedVcdC;
#endif
#endif

volatile std::sig_atomic_t interrupted = 0;

void handle_interrupt(int /*signal*/) { interrupted = 1; }

struct Options {
  std::vector<std::filesystem::path> elves;
  std::vector<std::string> raw_images;
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
  const std::string copy(text);
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
  const std::string copy(text);
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

[[nodiscard]] Options parse_options(int argc, char **argv,
                                    axi_tb::PlatformSpec &spec) {
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
      options.elves.emplace_back(argument(index, name));
    } else if (name == "--rom-image") {
      spec.set("rom.image=" + std::string(argument(index, name)));
    } else if (name == "--ram-image") {
      spec.set("ram.image=" + std::string(argument(index, name)));
    } else if (name == "--uart-in") {
      spec.set("uart.input=" + std::string(argument(index, name)));
    } else if (name == "--uart-out") {
      spec.set("uart.output=" + std::string(argument(index, name)));
    } else if (name == "--set") {
      spec.set(argument(index, name));
    } else if (name == "--load") {
      options.raw_images.emplace_back(argument(index, name));
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
      << "  --elf FILE             Load ELF32/ELF64 PT_LOADs (repeatable)\n"
      << "  --load TARGET=FILE     Load raw bytes at an address or mapped "
         "instance\n"
      << "  --set ID.OPTION=VALUE  Override a registered device property\n"
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
  // Keep the same object-oriented interface as the stateful trace writers.
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  void dump(std::uint64_t /*time*/) noexcept {}
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  void close() noexcept {}
};

void load_images(const Options &options, axi_tb::Platform &platform) {
  auto &space = platform.address_space();
  axi_tb::ImageLoadPlan plan(space);
  for (const auto &image : platform.images())
    plan.add_raw(image.path, image.address);
  for (const auto &request : options.raw_images) {
    const auto equal = request.find('=');
    if (equal == request.npos)
      throw std::invalid_argument("--load expects ADDRESS|INSTANCE=FILE");
    const auto target = request.substr(0, equal);
    std::optional<std::uint64_t> address;
    for (const auto &mapping : space.mappings()) {
      if (mapping.name == target) {
        if (address)
          throw std::invalid_argument("ambiguous load target: " + target);
        address = mapping.base;
      }
    }
    if (!address) address = axi_tb::parse_unsigned(target);
    plan.add_raw(request.substr(equal + 1), *address);
  }
  for (const auto &path : options.elves) {
    const auto loaded = plan.add_elf(path);
    std::cerr << "[axi-tb] ELF " << path << ": " << loaded.segments.size()
              << " segment(s), entry=0x" << std::hex << loaded.entry << std::dec
              << '\n';
  }
  plan.apply();
}

int run_simulation(int argc, char **argv, const Options &options,
                   const axi_tb::DeviceRegistry &registry,
                   const axi_tb::PlatformSpec &spec) {
  axi_tb::Platform platform(registry, spec, axi_tb::config::ADDRESS_BITS);
  axi_tb::config::SidebandBinding sideband(platform);
  auto &address_space = platform.address_space();
  load_images(options, platform);

  using Binding = axi_tb::VerilatedAxiBinding<
      Vaxi_tb_dut, axi_tb::config::NUM_PORTS, axi_tb::config::ADDRESS_BITS,
      axi_tb::config::DATA_BITS, axi_tb::config::ID_BITS>;
  using Fabric =
      axi_tb::AxiFabric<axi_tb::config::NUM_PORTS, axi_tb::config::ADDRESS_BITS,
                        axi_tb::config::DATA_BITS, axi_tb::config::ID_BITS>;

  VerilatedContext context;
  simulation_time = 0;
  context.commandArgs(argc, argv);
  context.threads(axi_tb::config::THREADS);
  Vaxi_tb_dut top(&context);
  if (top.threads() != axi_tb::config::THREADS) {
    throw std::runtime_error(
        "generated Verilator model thread count does not match configuration");
  }
  Fabric fabric(address_space, false);
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
      platform.begin_cycle(reset);
      sideband.drive(top);
      top.clk = 0;
      top.aresetn = reset ? 0 : 1;
      Binding::drive(top, fabric.drive(reset));
      evaluate(context, top, trace_pointer);
      const auto sampled = Binding::sample(top);
      top.clk = 1;
      evaluate(context, top, trace_pointer);
      fabric.commit(sampled, reset);
      platform.end_cycle(reset);
      ++total_cycles;
      if (!reset) {
        ++active_cycles;
      }
      if (fabric.exit_completed()) {
        const std::uint32_t guest_code = fabric.exit_code();
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

  if (interrupted != 0) {
    std::cerr << "[axi-tb] interrupted after " << active_cycles
              << " active cycle(s)\n";
    return 130;
  }
  if (context.gotFinish()) {
    std::cerr << "[axi-tb] DUT called $finish before writing the exit device\n";
    return PROTOCOL_ERROR;
  }
  std::cerr << "[axi-tb] timeout after " << active_cycles
            << " active cycle(s)\n";
  return TIMEOUT_ERROR;
}

}  // namespace

int main(int argc, char **argv) {
  try {
    axi_tb::DeviceRegistry registry;
    axi_tb::register_builtin_devices(registry);
    axi_tb::config::PlatformDefinition::register_devices(registry);
    auto spec = axi_tb::config::PlatformDefinition::defaults(
        {axi_tb::config::ROM_BASE, axi_tb::config::ROM_SIZE,
         axi_tb::config::RAM_BASE, axi_tb::config::RAM_SIZE,
         axi_tb::config::UART_BASE, axi_tb::config::UART_SIZE,
         axi_tb::config::EXIT_BASE, axi_tb::config::EXIT_SIZE});
    const Options options = parse_options(argc, argv, spec);
    if (options.help) {
      print_help(argv[0]);
      registry.print_help(std::cout);
      return 0;
    }
    return run_simulation(argc, argv, options, registry, spec);
  } catch (const axi_tb::ProtocolError &error) {
    std::cerr << "[axi-tb] AXI protocol error: " << error.what() << '\n';
    return PROTOCOL_ERROR;
  } catch (const axi_tb::ElfError &error) {
    std::cerr << "[axi-tb] image error: " << error.what() << '\n';
    return CONFIGURATION_ERROR;
  } catch (const std::exception &error) {
    std::cerr << "[axi-tb] configuration error: " << error.what() << '\n';
    return CONFIGURATION_ERROR;
  }
}
