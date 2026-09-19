#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <vector>

#include "device.hpp"

namespace axi_tb {

class ElfError final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct ElfSegment {
  std::uint64_t virtual_address = 0;
  std::uint64_t physical_address = 0;
  std::uint64_t load_address = 0;
  std::uint64_t file_size = 0;
  std::uint64_t memory_size = 0;
  std::uint32_t flags = 0;
};

struct ElfLoadResult {
  std::uint64_t entry = 0;
  bool is_64_bit = false;
  std::vector<ElfSegment> segments;
};

// Owns input bytes until apply(). Every range, including BSS and aliases of
// the same device, is checked across all images before any device is changed.
class ImageLoadPlan {
 public:
  explicit ImageLoadPlan(AddressSpace &space) : space_(space) {}
  [[nodiscard]] ElfLoadResult add_elf(std::span<const std::byte> image);
  [[nodiscard]] ElfLoadResult add_elf(const std::filesystem::path &path);
  void add_raw(std::span<const std::byte> image, std::uint64_t address);
  void add_raw(const std::filesystem::path &path, std::uint64_t address);
  void apply();

 private:
  struct Chunk {
    ElfSegment segment;
    std::vector<std::byte> data;
  };
  AddressSpace &space_;
  std::vector<Chunk> chunks_;
};

// Loads little-endian ELF32/ELF64 PT_LOAD segments.  p_paddr is used as the
// bus address when nonzero, otherwise p_vaddr is used.  All segments are fully
// validated (bounds, destination, and overlap) before memory is modified.
[[nodiscard]] ElfLoadResult load_elf(std::span<const std::byte> image,
                                     AddressSpace &address_space);
[[nodiscard]] ElfLoadResult load_elf(const std::filesystem::path &path,
                                     AddressSpace &address_space);

// Raw-image helpers share the same strict, single-mapping boundary checks.
void load_raw_image(std::span<const std::byte> image,
                    AddressSpace &address_space, std::uint64_t address);
void load_raw_image(const std::filesystem::path &path,
                    AddressSpace &address_space, std::uint64_t address);

}  // namespace axi_tb
