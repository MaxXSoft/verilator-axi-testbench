#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "devices.hpp"
#include "elf_loader.hpp"

namespace {

using axi_tb::AddressSpace;
using axi_tb::ElfError;
using axi_tb::RamDevice;
using axi_tb::Response;
using axi_tb::RomDevice;

template <typename Integer>
void put_le(std::vector<std::byte> &image, std::size_t offset, Integer value) {
  for (std::size_t byte = 0; byte < sizeof(Integer); ++byte) {
    image.at(offset + byte) =
        static_cast<std::byte>((value >> (byte * 8U)) & 0xffU);
  }
}

void put_ident(std::vector<std::byte> &image, std::uint8_t elf_class) {
  image[0] = std::byte{0x7f};
  image[1] = std::byte{'E'};
  image[2] = std::byte{'L'};
  image[3] = std::byte{'F'};
  image[4] = static_cast<std::byte>(elf_class);
  image[5] = std::byte{1};
  image[6] = std::byte{1};
}

std::vector<std::byte> make_elf32() {
  std::vector<std::byte> image(0x120, std::byte{0});
  put_ident(image, 1);
  put_le<std::uint16_t>(image, 16, 2);
  put_le<std::uint16_t>(image, 18, 243);
  put_le<std::uint32_t>(image, 20, 1);
  put_le<std::uint32_t>(image, 24, 0x100);
  put_le<std::uint32_t>(image, 28, 52);
  put_le<std::uint16_t>(image, 40, 52);
  put_le<std::uint16_t>(image, 42, 32);
  put_le<std::uint16_t>(image, 44, 2);

  // Executable ROM segment: four file bytes and four bytes of BSS.
  put_le<std::uint32_t>(image, 52 + 0, 1);
  put_le<std::uint32_t>(image, 52 + 4, 0x100);
  put_le<std::uint32_t>(image, 52 + 8, 0x100);
  put_le<std::uint32_t>(image, 52 + 12, 0x100);
  put_le<std::uint32_t>(image, 52 + 16, 4);
  put_le<std::uint32_t>(image, 52 + 20, 8);
  put_le<std::uint32_t>(image, 52 + 24, 5);
  put_le<std::uint32_t>(image, 52 + 28, 0x100);

  // Writable RAM segment.
  constexpr std::size_t second = 52 + 32;
  put_le<std::uint32_t>(image, second + 0, 1);
  put_le<std::uint32_t>(image, second + 4, 0x110);
  put_le<std::uint32_t>(image, second + 8, 0x8000);
  put_le<std::uint32_t>(image, second + 12, 0x8000);
  put_le<std::uint32_t>(image, second + 16, 3);
  put_le<std::uint32_t>(image, second + 20, 5);
  put_le<std::uint32_t>(image, second + 24, 6);
  put_le<std::uint32_t>(image, second + 28, 1);

  image[0x100] = std::byte{0x13};
  image[0x101] = std::byte{0x05};
  image[0x102] = std::byte{0x10};
  image[0x103] = std::byte{0x00};
  image[0x110] = std::byte{0xaa};
  image[0x111] = std::byte{0xbb};
  image[0x112] = std::byte{0xcc};
  return image;
}

std::vector<std::byte> make_elf64() {
  std::vector<std::byte> image(0x108, std::byte{0});
  put_ident(image, 2);
  put_le<std::uint16_t>(image, 16, 2);
  put_le<std::uint16_t>(image, 18, 243);
  put_le<std::uint32_t>(image, 20, 1);
  put_le<std::uint64_t>(image, 24, 0x9000);
  put_le<std::uint64_t>(image, 32, 64);
  put_le<std::uint16_t>(image, 52, 64);
  put_le<std::uint16_t>(image, 54, 56);
  put_le<std::uint16_t>(image, 56, 1);

  put_le<std::uint32_t>(image, 64 + 0, 1);
  put_le<std::uint32_t>(image, 64 + 4, 6);
  put_le<std::uint64_t>(image, 64 + 8, 0x100);
  put_le<std::uint64_t>(image, 64 + 16, 0x9000);
  put_le<std::uint64_t>(image, 64 + 24, 0); // Fall back to p_vaddr.
  put_le<std::uint64_t>(image, 64 + 32, 4);
  put_le<std::uint64_t>(image, 64 + 40, 8);
  put_le<std::uint64_t>(image, 64 + 48, 0x100);
  image[0x100] = std::byte{1};
  image[0x101] = std::byte{2};
  image[0x102] = std::byte{3};
  image[0x103] = std::byte{4};
  return image;
}

template <typename Callable> void expect_elf_error(Callable &&callable) {
  bool failed = false;
  try {
    callable();
  } catch (const ElfError &) {
    failed = true;
  }
  assert(failed);
}

void test_elf32() {
  RomDevice rom(0x1000);
  RamDevice ram(0x100);
  AddressSpace space;
  space.map(0, rom.size(), rom, "rom");
  space.map(0x8000, ram.size(), ram, "ram");

  const std::array<std::byte, 4> dirty{std::byte{0xff}, std::byte{0xff},
                                       std::byte{0xff}, std::byte{0xff}};
  assert(rom.load(0x104, dirty) == Response::okay);
  assert(ram.load(3, dirty) == Response::okay);

  const auto image = make_elf32();
  const auto result = axi_tb::load_elf(image, space);
  assert(!result.is_64_bit);
  assert(result.entry == 0x100);
  assert(result.segments.size() == 2);
  assert(result.segments[0].load_address == 0x100);
  assert(result.segments[1].load_address == 0x8000);

  assert(rom.bytes()[0x100] == std::byte{0x13});
  assert(rom.bytes()[0x101] == std::byte{0x05});
  assert(rom.bytes()[0x102] == std::byte{0x10});
  assert(rom.bytes()[0x103] == std::byte{0x00});
  for (std::size_t offset = 0x104; offset < 0x108; ++offset) {
    assert(rom.bytes()[offset] == std::byte{0});
  }
  assert(ram.bytes()[0] == std::byte{0xaa});
  assert(ram.bytes()[1] == std::byte{0xbb});
  assert(ram.bytes()[2] == std::byte{0xcc});
  assert(ram.bytes()[3] == std::byte{0});
  assert(ram.bytes()[4] == std::byte{0});

  const std::array<std::byte, 3> raw{std::byte{9}, std::byte{8}, std::byte{7}};
  axi_tb::load_raw_image(raw, space, 0x8010);
  assert(ram.bytes()[0x10] == std::byte{9});
  assert(ram.bytes()[0x11] == std::byte{8});
  assert(ram.bytes()[0x12] == std::byte{7});
  expect_elf_error([&] { axi_tb::load_raw_image(raw, space, 0x80ff); });
}

void test_elf64_and_path_loader() {
  RamDevice ram(0x100);
  AddressSpace space;
  space.map(0x9000, ram.size(), ram, "ram64");
  const std::array<std::byte, 4> dirty{std::byte{0xff}, std::byte{0xff},
                                       std::byte{0xff}, std::byte{0xff}};
  assert(ram.load(4, dirty) == Response::okay);

  const auto image = make_elf64();
  const auto result = axi_tb::load_elf(image, space);
  assert(result.is_64_bit);
  assert(result.entry == 0x9000);
  assert(result.segments.size() == 1);
  for (std::size_t offset = 0; offset < 4; ++offset) {
    assert(ram.bytes()[offset] == static_cast<std::byte>(offset + 1));
  }
  for (std::size_t offset = 4; offset < 8; ++offset) {
    assert(ram.bytes()[offset] == std::byte{0});
  }

  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("axi-tb-elf-" + std::to_string(nonce) + ".bin");
  {
    std::ofstream output(path, std::ios::binary);
    assert(output.good());
    output.write(reinterpret_cast<const char *>(image.data()),
                 static_cast<std::streamsize>(image.size()));
    assert(output.good());
  }
  const auto path_result = axi_tb::load_elf(path, space);
  assert(path_result.entry == 0x9000);
  std::filesystem::remove(path);
}

void test_malformed_images_are_rejected_atomically() {
  RomDevice rom(0x1000);
  RamDevice ram(0x100);
  AddressSpace space;
  space.map(0, rom.size(), rom, "rom");
  space.map(0x8000, ram.size(), ram, "ram");

  auto bad_endian = make_elf32();
  bad_endian[5] = std::byte{2};
  expect_elf_error([&] { (void)axi_tb::load_elf(bad_endian, space); });

  auto truncated = make_elf32();
  truncated.resize(40);
  expect_elf_error([&] { (void)axi_tb::load_elf(truncated, space); });

  auto file_larger_than_memory = make_elf32();
  put_le<std::uint32_t>(file_larger_than_memory, 52 + 16, 9);
  expect_elf_error(
      [&] { (void)axi_tb::load_elf(file_larger_than_memory, space); });

  auto outside_mapping = make_elf32();
  put_le<std::uint32_t>(outside_mapping, 52 + 12, 0xffc);
  expect_elf_error([&] { (void)axi_tb::load_elf(outside_mapping, space); });

  auto bad_alignment = make_elf32();
  put_le<std::uint32_t>(bad_alignment, 52 + 28, 3);
  expect_elf_error([&] { (void)axi_tb::load_elf(bad_alignment, space); });

  // The second segment overlaps the first.  Mark the first destination before
  // calling the loader to prove that complete validation precedes all writes.
  auto overlap = make_elf32();
  constexpr std::size_t second = 52 + 32;
  put_le<std::uint32_t>(overlap, second + 8, 0x104);
  put_le<std::uint32_t>(overlap, second + 12, 0x104);
  const std::array<std::byte, 1> marker{std::byte{0x77}};
  assert(rom.load(0x100, marker) == Response::okay);
  expect_elf_error([&] { (void)axi_tb::load_elf(overlap, space); });
  assert(rom.bytes()[0x100] == std::byte{0x77});

  auto file_out_of_bounds = make_elf32();
  put_le<std::uint32_t>(file_out_of_bounds, 52 + 4, 0x11f);
  put_le<std::uint32_t>(file_out_of_bounds, 52 + 16, 4);
  expect_elf_error([&] { (void)axi_tb::load_elf(file_out_of_bounds, space); });

  // AddressSpace mappings are deliberately independent from a device's
  // backing capacity.  ELF preflight must validate both layers.
  RomDevice short_rom(0x100);
  RamDevice short_ram(0x100);
  AddressSpace oversized_mapping;
  oversized_mapping.map(0, 0x1000, short_rom, "oversized-rom-window");
  oversized_mapping.map(0x8000, 0x100, short_ram, "ram");
  expect_elf_error(
      [&] { (void)axi_tb::load_elf(make_elf32(), oversized_mapping); });
}

} // namespace

int main() {
  test_elf32();
  test_elf64_and_path_loader();
  test_malformed_images_are_rejected_atomically();
  return 0;
}
