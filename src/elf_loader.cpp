#include "elf_loader.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>

namespace axi_tb {
namespace {

constexpr std::uint32_t PT_LOAD = 1;
constexpr std::uint8_t ELF_CLASS_32 = 1;
constexpr std::uint8_t ELF_CLASS_64 = 2;
constexpr std::uint8_t ELF_DATA_LITTLE_ENDIAN = 1;
constexpr std::uint8_t ELF_VERSION_CURRENT = 1;

struct ParsedSegment {
  ElfSegment public_segment;
  std::uint64_t file_offset = 0;
  std::uint64_t alignment = 0;
  std::size_t program_header_index = 0;
};

struct ParsedHeader {
  std::uint64_t entry = 0;
  std::uint64_t program_header_offset = 0;
  std::uint64_t program_header_size = 0;
  std::uint16_t program_header_count = 0;
  bool is_64_bit = false;
};

[[nodiscard]] std::string hexadecimal(std::uint64_t value) {
  std::ostringstream stream;
  stream << "0x" << std::hex << value;
  return stream.str();
}

void require_range(std::uint64_t offset, std::uint64_t length,
                   std::uint64_t capacity, const char *description) {
  if (offset > capacity || length > capacity - offset) {
    throw ElfError(std::string("ELF ") + description + " is out of bounds");
  }
}

template <typename Integer>
[[nodiscard]] Integer read_little_endian(std::span<const std::byte> image,
                                         std::uint64_t offset,
                                         const char *description) {
  static_assert(std::is_unsigned_v<Integer>);
  require_range(offset, sizeof(Integer), image.size(), description);
  Integer value = 0;
  for (std::size_t byte = 0; byte < sizeof(Integer); ++byte) {
    value |= static_cast<Integer>(
                 std::to_integer<std::uint8_t>(image[offset + byte]))
             << (byte * 8U);
  }
  return value;
}

[[nodiscard]] std::uint64_t checked_multiply(std::uint64_t lhs,
                                             std::uint64_t rhs,
                                             const char *description) {
  if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
    throw ElfError(std::string("ELF ") + description + " overflows");
  }
  return lhs * rhs;
}

[[nodiscard]] std::uint64_t checked_add(std::uint64_t lhs, std::uint64_t rhs,
                                        const char *description) {
  if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
    throw ElfError(std::string("ELF ") + description + " overflows");
  }
  return lhs + rhs;
}

[[nodiscard]] std::vector<std::byte> read_file(
    const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    throw ElfError("cannot open image: " + path.string());
  }
  const std::streamoff end = stream.tellg();
  if (end < 0 ||
      static_cast<std::uintmax_t>(end) >
          std::numeric_limits<std::size_t>::max() ||
      end > std::numeric_limits<std::streamsize>::max()) {
    throw ElfError("image is too large: " + path.string());
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(end));
  stream.seekg(0);
  if (!bytes.empty()) {
    stream.read(reinterpret_cast<char *>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
      throw ElfError("cannot read image: " + path.string());
    }
  }
  return bytes;
}

void validate_load_target(AddressSpace &address_space,
                          const ElfSegment &segment,
                          std::size_t program_header_index) {
  const AddressSpace::Mapping *mapping =
      address_space.resolve(segment.load_address, segment.memory_size);
  if (mapping == nullptr) {
    throw ElfError("PT_LOAD[" + std::to_string(program_header_index) +
                   "] range " + hexadecimal(segment.load_address) + ".." +
                   hexadecimal(segment.load_address + segment.memory_size) +
                   " does not fit one mapped device");
  }
  if (!mapping->device->loadable()) {
    throw ElfError("PT_LOAD[" + std::to_string(program_header_index) +
                   "] targets non-loadable device '" + mapping->name + "'");
  }
  if (!mapping->device->can_load(segment.load_address - mapping->base,
                                 segment.memory_size)) {
    throw ElfError("PT_LOAD[" + std::to_string(program_header_index) +
                   "] exceeds device '" + mapping->name + "'");
  }
}

void apply_segment(std::span<const std::byte> image,
                   const ParsedSegment &segment, AddressSpace &address_space) {
  const ElfSegment &info = segment.public_segment;
  if (info.file_size != 0) {
    const auto file_bytes =
        image.subspan(static_cast<std::size_t>(segment.file_offset),
                      static_cast<std::size_t>(info.file_size));
    if (!response_is_success(
            address_space.load(info.load_address, file_bytes))) {
      throw ElfError("device rejected PT_LOAD data at " +
                     hexadecimal(info.load_address));
    }
  }

  std::uint64_t zero_address = info.load_address + info.file_size;
  std::uint64_t remaining = info.memory_size - info.file_size;
  constexpr std::array<std::byte, 4096> ZEROS{};
  while (remaining != 0) {
    const std::size_t count = static_cast<std::size_t>(
        std::min<std::uint64_t>(remaining, ZEROS.size()));
    if (!response_is_success(address_space.load(
            zero_address, std::span<const std::byte>(ZEROS).first(count)))) {
      throw ElfError("device rejected PT_LOAD BSS at " +
                     hexadecimal(zero_address));
    }
    zero_address += count;
    remaining -= count;
  }
}

[[nodiscard]] ParsedHeader parse_header(std::span<const std::byte> image) {
  require_range(0, 16, image.size(), "identification");
  if (image[0] != std::byte{0x7f} || image[1] != std::byte{'E'} ||
      image[2] != std::byte{'L'} || image[3] != std::byte{'F'}) {
    throw ElfError("input is not an ELF image");
  }

  const auto elf_class = std::to_integer<std::uint8_t>(image[4]);
  if (elf_class != ELF_CLASS_32 && elf_class != ELF_CLASS_64) {
    throw ElfError("unsupported ELF class (expected ELF32 or ELF64)");
  }
  if (std::to_integer<std::uint8_t>(image[5]) != ELF_DATA_LITTLE_ENDIAN) {
    throw ElfError("only little-endian ELF images are supported");
  }
  if (std::to_integer<std::uint8_t>(image[6]) != ELF_VERSION_CURRENT) {
    throw ElfError("unsupported ELF identification version");
  }

  ParsedHeader header;
  header.is_64_bit = elf_class == ELF_CLASS_64;
  const std::uint64_t header_size = header.is_64_bit ? 64U : 52U;
  header.program_header_size = header.is_64_bit ? 56U : 32U;
  require_range(0, header_size, image.size(), "header");

  const auto version = read_little_endian<std::uint32_t>(image, 20, "version");
  if (version != ELF_VERSION_CURRENT) {
    throw ElfError("unsupported ELF header version");
  }

  header.entry =
      header.is_64_bit
          ? read_little_endian<std::uint64_t>(image, 24, "entry point")
          : read_little_endian<std::uint32_t>(image, 24, "entry point");
  header.program_header_offset = header.is_64_bit
                                     ? read_little_endian<std::uint64_t>(
                                           image, 32, "program-header offset")
                                     : read_little_endian<std::uint32_t>(
                                           image, 28, "program-header offset");
  const auto declared_header_size = read_little_endian<std::uint16_t>(
      image, header.is_64_bit ? 52 : 40, "header size");
  const auto declared_program_header_size = read_little_endian<std::uint16_t>(
      image, header.is_64_bit ? 54 : 42, "program-header entry size");
  header.program_header_count = read_little_endian<std::uint16_t>(
      image, header.is_64_bit ? 56 : 44, "program-header count");

  if (declared_header_size < header_size) {
    throw ElfError("ELF header size is smaller than its class requires");
  }
  if (header.program_header_count == 0xffffU) {
    throw ElfError("extended ELF program-header counts are not supported");
  }
  if (header.program_header_count != 0 &&
      declared_program_header_size < header.program_header_size) {
    throw ElfError(
        "ELF program-header entry is smaller than its class requires");
  }
  header.program_header_size = declared_program_header_size;
  const auto table_size =
      checked_multiply(header.program_header_count, header.program_header_size,
                       "program-header table size");
  require_range(header.program_header_offset, table_size, image.size(),
                "program-header table");
  return header;
}

[[nodiscard]] std::optional<ParsedSegment> parse_program_header(
    std::span<const std::byte> image, const ParsedHeader &header,
    std::size_t index, AddressSpace &address_space) {
  const auto offset =
      checked_add(header.program_header_offset,
                  checked_multiply(index, header.program_header_size,
                                   "program-header offset"),
                  "program-header offset");
  const auto type =
      read_little_endian<std::uint32_t>(image, offset, "segment type");
  if (type != PT_LOAD) {
    return std::nullopt;
  }

  ParsedSegment parsed;
  parsed.program_header_index = index;
  if (header.is_64_bit) {
    parsed.public_segment.flags =
        read_little_endian<std::uint32_t>(image, offset + 4, "segment flags");
    parsed.file_offset = read_little_endian<std::uint64_t>(
        image, offset + 8, "segment file offset");
    parsed.public_segment.virtual_address = read_little_endian<std::uint64_t>(
        image, offset + 16, "segment virtual address");
    parsed.public_segment.physical_address = read_little_endian<std::uint64_t>(
        image, offset + 24, "segment physical address");
    parsed.public_segment.file_size = read_little_endian<std::uint64_t>(
        image, offset + 32, "segment file size");
    parsed.public_segment.memory_size = read_little_endian<std::uint64_t>(
        image, offset + 40, "segment memory size");
    parsed.alignment = read_little_endian<std::uint64_t>(image, offset + 48,
                                                         "segment alignment");
  } else {
    parsed.file_offset = read_little_endian<std::uint32_t>(
        image, offset + 4, "segment file offset");
    parsed.public_segment.virtual_address = read_little_endian<std::uint32_t>(
        image, offset + 8, "segment virtual address");
    parsed.public_segment.physical_address = read_little_endian<std::uint32_t>(
        image, offset + 12, "segment physical address");
    parsed.public_segment.file_size = read_little_endian<std::uint32_t>(
        image, offset + 16, "segment file size");
    parsed.public_segment.memory_size = read_little_endian<std::uint32_t>(
        image, offset + 20, "segment memory size");
    parsed.public_segment.flags =
        read_little_endian<std::uint32_t>(image, offset + 24, "segment flags");
    parsed.alignment = read_little_endian<std::uint32_t>(image, offset + 28,
                                                         "segment alignment");
  }

  if (parsed.public_segment.file_size > parsed.public_segment.memory_size) {
    throw ElfError("PT_LOAD[" + std::to_string(index) +
                   "] has p_filesz greater than p_memsz");
  }
  require_range(parsed.file_offset, parsed.public_segment.file_size,
                image.size(), "loadable segment data");
  if (parsed.alignment > 1) {
    if ((parsed.alignment & (parsed.alignment - 1U)) != 0) {
      throw ElfError("PT_LOAD[" + std::to_string(index) +
                     "] alignment is not a power of two");
    }
    if ((parsed.public_segment.virtual_address % parsed.alignment) !=
        (parsed.file_offset % parsed.alignment)) {
      throw ElfError("PT_LOAD[" + std::to_string(index) +
                     "] virtual address and file offset are incongruent");
    }
  }

  parsed.public_segment.load_address =
      parsed.public_segment.physical_address != 0
          ? parsed.public_segment.physical_address
          : parsed.public_segment.virtual_address;
  (void)checked_add(parsed.public_segment.load_address,
                    parsed.public_segment.memory_size,
                    "loadable segment address range");
  if (parsed.public_segment.memory_size == 0) {
    return std::nullopt;
  }
  validate_load_target(address_space, parsed.public_segment, index);
  return parsed;
}

[[nodiscard]] std::vector<ParsedSegment> parse_load_segments(
    std::span<const std::byte> image, const ParsedHeader &header,
    AddressSpace &address_space) {
  std::vector<ParsedSegment> segments;
  segments.reserve(header.program_header_count);
  for (std::size_t index = 0; index < header.program_header_count; ++index) {
    auto segment = parse_program_header(image, header, index, address_space);
    if (segment) {
      segments.push_back(*segment);
    }
  }
  return segments;
}

void validate_no_overlap(const std::vector<ParsedSegment> &segments) {
  std::vector<std::size_t> address_order(segments.size());
  for (std::size_t index = 0; index < address_order.size(); ++index) {
    address_order[index] = index;
  }
  std::ranges::sort(address_order, {}, [&segments](std::size_t index) {
    return segments[index].public_segment.load_address;
  });
  for (std::size_t index = 1; index < address_order.size(); ++index) {
    const ElfSegment &previous =
        segments[address_order[index - 1]].public_segment;
    const ParsedSegment &current = segments[address_order[index]];
    if (previous.load_address + previous.memory_size >
        current.public_segment.load_address) {
      throw ElfError("PT_LOAD[" + std::to_string(current.program_header_index) +
                     "] overlaps another loadable segment");
    }
  }
}

}  // namespace

ElfLoadResult load_elf(std::span<const std::byte> image,
                       AddressSpace &address_space) {
  const ParsedHeader header = parse_header(image);
  const std::vector<ParsedSegment> parsed_segments =
      parse_load_segments(image, header, address_space);
  validate_no_overlap(parsed_segments);

  // Parsing, all bounds checks, destination checks, and overlap checks have
  // completed.  From here on loadable() promises that in-range initialization
  // cannot be rejected, so malformed ELF files never partially modify memory.
  for (const ParsedSegment &segment : parsed_segments) {
    apply_segment(image, segment, address_space);
  }

  ElfLoadResult result;
  result.entry = header.entry;
  result.is_64_bit = header.is_64_bit;
  result.segments.reserve(parsed_segments.size());
  for (const ParsedSegment &segment : parsed_segments) {
    result.segments.push_back(segment.public_segment);
  }
  return result;
}

ElfLoadResult load_elf(const std::filesystem::path &path,
                       AddressSpace &address_space) {
  const std::vector<std::byte> image = read_file(path);
  return load_elf(image, address_space);
}

void load_raw_image(std::span<const std::byte> image,
                    AddressSpace &address_space, std::uint64_t address) {
  if (image.empty()) {
    return;
  }
  const AddressSpace::Mapping *mapping =
      address_space.resolve(address, image.size());
  if (mapping == nullptr) {
    throw ElfError("raw image range at " + hexadecimal(address) +
                   " does not fit one mapped device");
  }
  if (!mapping->device->loadable()) {
    throw ElfError("raw image targets non-loadable device '" + mapping->name +
                   "'");
  }
  if (!mapping->device->can_load(address - mapping->base, image.size())) {
    throw ElfError("raw image exceeds device '" + mapping->name + "'");
  }
  if (!response_is_success(address_space.load(address, image))) {
    throw ElfError("device rejected raw image at " + hexadecimal(address));
  }
}

void load_raw_image(const std::filesystem::path &path,
                    AddressSpace &address_space, std::uint64_t address) {
  const std::vector<std::byte> image = read_file(path);
  load_raw_image(image, address_space, address);
}

}  // namespace axi_tb
