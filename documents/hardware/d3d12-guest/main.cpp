// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <orbis/GnmDriver.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

extern "C" int sceKernelIsNeoMode();

namespace {
constexpr size_t AllocationSize = 8 * 1024 * 1024;
constexpr uint32_t Width = 1280, Height = 720;

struct ShaderBinaryInfo {
  char signature[7];
  uint8_t version;
  uint32_t flags_length;
  uint32_t usage;
  uint64_t hash;
  uint32_t crc;
};
static_assert(sizeof(ShaderBinaryInfo) == 32);

struct Commands {
  uint32_t *begin;
  uint32_t *current;
  void Packet(uint32_t opcode, std::initializer_list<uint32_t> body,
              bool compute = false) {
    *current++ = 0xc0000000 | ((uint32_t(body.size()) - 1) << 16) |
                 (opcode << 8) | (compute ? 2 : 0);
    for (const auto value : body)
      *current++ = value;
  }
  void Register(uint32_t address, uint32_t value) {
    Packet(0x76, {address - 0x2c00, value}, true);
  }
};

uint32_t Hash(const uint32_t *data, size_t count) {
  uint32_t value = 2166136261;
  for (size_t i = 0; i < count; ++i) {
    value ^= data[i];
    value *= 16777619;
  }
  return value;
}

struct NeighborWrites {
  volatile uint32_t *target;
  std::atomic<bool> started{}, stop{};
};

void *WriteNeighbors(void *argument) {
  auto &writes = *static_cast<NeighborWrites *>(argument);
  writes.started.store(true, std::memory_order_release);
  while (!writes.stop.load(std::memory_order_acquire)) {
    writes.target[1] = 0xaabbccdd;
    writes.target[2] = 0x55667788;
  }
  return nullptr;
}

bool RunCommandDependencies(uint8_t *bytes, uint8_t *alias_bytes) {
  auto *store = reinterpret_cast<uint32_t *>(bytes + 0x18000);
  auto *copy = reinterpret_cast<uint32_t *>(bytes + 0x18100);
  const uint32_t store_code[]{0x7e0002ff, 0x11223344, 0x7e020280,
                              0xe0701000, 0x80000001, 0xbf8c0f70, 0xbf810000};
  const uint32_t copy_code[]{0x7e020280, 0xe0301000, 0x80000201,
                             0xbf8c0f70, 0xe0701000, 0x80010201,
                             0xbf8c0f70, 0xbf810000};
  std::memcpy(store, store_code, sizeof(store_code));
  std::memcpy(copy, copy_code, sizeof(copy_code));
  const ShaderBinaryInfo store_info{{'O', 'r', 'b', 'S', 'h', 'd', 'r'}, 1,
      uint32_t(sizeof(store_code)) << 8, 0, 0x70303173746f7265, 0};
  const ShaderBinaryInfo copy_info{{'O', 'r', 'b', 'S', 'h', 'd', 'r'}, 1,
      uint32_t(sizeof(copy_code)) << 8, 0, 0x703031636f707921, 0};
  std::memcpy(bytes + 0x18040, &store_info, sizeof(store_info));
  std::memcpy(bytes + 0x18140, &copy_info, sizeof(copy_info));
  auto *words = reinterpret_cast<uint32_t *>(bytes + 0x30000);
  const int initialized = sceGnmDrawInitDefaultHardwareState350(words, 0x1000);
  if (initialized <= 0 || initialized > 0x1000)
    return false;
  Commands commands{words, words + initialized};
  for (uint32_t reg = 0x2e04; reg <= 0x2e06; ++reg)
    commands.Register(reg, 0);
  for (uint32_t reg = 0x2e07; reg <= 0x2e09; ++reg)
    commands.Register(reg, 1);
  commands.Register(0x2e12, 0);
  commands.Register(0x2e13, 8 << 1);
  commands.Register(0x2e15, 0x170);
  commands.Register(0x2e16, 0xffffffff);
  commands.Register(0x2e17, 0xffffffff);
  const auto program = [&](uint32_t *shader) {
    commands.Register(0x2e0c, uint32_t(uintptr_t(shader) >> 8));
    commands.Register(0x2e0d, uint32_t(uintptr_t(shader) >> 40));
  };
  const auto buffer = [&](uint32_t slot, uintptr_t address) {
    commands.Packet(0x76, {0x240 + slot, uint32_t(address),
        uint32_t(address >> 32) & 0xff, 4,
        4u | (5u << 3) | (6u << 6) | (7u << 9) | (3u << 27)}, true);
  };
  program(store);
  for (uint32_t i = 0; i < 8; ++i) {
    *reinterpret_cast<uint32_t *>(bytes + 0x1c0000 + i * 0x10000) = 0;
    buffer(0, uintptr_t(bytes + 0x1c0000 + i * 0x10000));
    commands.Packet(0x15, {1, 1, 1, 0x21}, true);
  }
  commands.Packet(0x46, {7u | (4u << 8)}, true);
  commands.Packet(0x58, {(1u << 18) | (1u << 22) | (1u << 23),
      0xffffffff, 0xff, 0, 0, 10}, true);
  program(copy);
  buffer(0, uintptr_t(alias_bytes + 0x1c0000));
  buffer(4, uintptr_t(bytes + 0x240000));
  *reinterpret_cast<uint32_t *>(bytes + 0x240000) = 0;
  commands.Packet(0x15, {1, 1, 1, 0x21}, true);
  auto *fence = reinterpret_cast<volatile uint32_t *>(bytes + 0x20100);
  *fence = 0;
  const auto completion = uintptr_t(fence);
  commands.Packet(0x48, {47u | (6u << 8), uint32_t(completion),
      uint32_t(completion >> 32) | (2u << 29), 1}, true);
  commands.Packet(0x47, {20u | (5u << 8), uint32_t(completion),
      uint32_t(completion >> 32) | (1u << 29), 2, 0});
  void *addresses[]{words};
  uint32_t sizes[]{uint32_t((commands.current - commands.begin) * 4)};
  const int rc = sceGnmSubmitCommandBuffers(1, addresses, sizes, nullptr, nullptr);
  if (rc < 0)
    return false;
  for (uint32_t wait = 0; *fence != 2; ++wait) {
    if (wait == 10000)
      std::printf("COMMAND_DEPENDENCIES TIMEOUT; retaining submitted memory\n");
    sceKernelUsleep(1000);
  }
  bool pass = *reinterpret_cast<volatile uint32_t *>(bytes + 0x240000) == 0x11223344;
  for (uint32_t i = 0; i < 8; ++i)
    pass &= *reinterpret_cast<volatile uint32_t *>(alias_bytes + 0x1c0000 + i * 0x10000) == 0x11223344;
  std::printf("COMMAND_DEPENDENCIES independent=8 alias_copy=%08x %s\n",
      *reinterpret_cast<volatile uint32_t *>(bytes + 0x240000), pass ? "PASS" : "FAIL");
  *reinterpret_cast<volatile uint32_t *>(alias_bytes + 0x1c0004) = 0xaabbccdd;
  commands.current = words + initialized;
  commands.Packet(0x58, {(1u << 18) | (1u << 22) | (1u << 23),
      0xffffffff, 0xff, 0, 0, 10}, true);
  program(copy);
  buffer(0, uintptr_t(alias_bytes + 0x1c0004));
  buffer(4, uintptr_t(bytes + 0x240004));
  commands.Packet(0x15, {1, 1, 1, 0x21}, true);
  *fence = 0;
  commands.Packet(0x48, {47u | (6u << 8), uint32_t(completion),
      uint32_t(completion >> 32) | (2u << 29), 1}, true);
  commands.Packet(0x47, {20u | (5u << 8), uint32_t(completion),
      uint32_t(completion >> 32) | (1u << 29), 2, 0});
  sizes[0] = uint32_t((commands.current - commands.begin) * 4);
  if (sceGnmSubmitCommandBuffers(1, addresses, sizes, nullptr, nullptr) < 0)
    return false;
  while (*fence != 2)
    sceKernelUsleep(1000);
  const auto neighbor = *reinterpret_cast<volatile uint32_t *>(bytes + 0x240004);
  const auto original = *reinterpret_cast<volatile uint32_t *>(bytes + 0x1c0000);
  pass &= neighbor == 0xaabbccdd && original == 0x11223344;
  std::printf("PERSISTENT_BUFFER_CPU_ALIAS neighbor=%08x original=%08x %s\n",
      neighbor, original, pass ? "PASS" : "FAIL");
  return pass;
}

bool RunSegmentedAliases(uint8_t *bytes, uint8_t *alias_bytes, off_t physical) {
  void *segmented{};
  int rc = sceKernelMapDirectMemory(&segmented, 0x20000, 0x33, 0,
                                    physical + 0x500000, 0x10000);
  if (rc < 0)
    return false;
  auto *second = static_cast<uint8_t *>(segmented) + 0x10000;
  rc = sceKernelMunmap(second, 0x10000);
  if (rc < 0) {
    sceKernelMunmap(segmented, 0x20000);
    return false;
  }
  void *fixed = second;
  rc = sceKernelMapDirectMemory(&fixed, 0x10000, 0x33, 0x10,
                                physical + 0x600000, 0x10000);
  if (rc < 0) {
    sceKernelMunmap(segmented, 0x10000);
    return false;
  }
  auto *output = reinterpret_cast<volatile uint32_t *>(second - 4);
  output[0] = output[1] = 0xdeadbeef;
  auto *source = reinterpret_cast<uint32_t *>(bytes + 0x40000);
  for (size_t i = 0; i < 64 * 16; ++i)
    source[i] = 0x11223344;
  auto *shader = reinterpret_cast<uint32_t *>(bytes + 0x11000);
  const uint32_t code[]{0x7e000280, 0x7e020280, 0xf0000100, 0x00000400,
                        0xe0301000, 0x80020500, 0xbf8c0f70, 0xe0701000,
                        0x80030400, 0xe0701004, 0x80030500, 0xbf8c0f70,
                        0xbf810000};
  std::memcpy(shader, code, sizeof(code));
  const ShaderBinaryInfo metadata{{'O', 'r', 'b', 'S', 'h', 'd', 'r'},
                                  1,
                                  uint32_t(sizeof(code)) << 8,
                                  0,
                                  0x7365676d656e7473,
                                  0};
  std::memcpy(bytes + 0x11040, &metadata, sizeof(metadata));
  uint64_t image[4]{};
  image[0] =
      (uintptr_t(source) >> 8) | (uint64_t(4) << 52) | (uint64_t(4) << 58);
  image[1] = 15 | (uint64_t(15) << 14) | (uint64_t(4) << 32) |
             (uint64_t(5) << 35) | (uint64_t(6) << 38) | (uint64_t(7) << 41) |
             (uint64_t(8) << 52) | (uint64_t(9) << 60);
  image[2] = uint64_t(63) << 13;
  uint32_t user[16]{};
  std::memcpy(user, image, sizeof(image));
  const auto buffer = uintptr_t(alias_bytes + 0x40000);
  const auto destination = uintptr_t(output);
  user[8] = uint32_t(buffer);
  user[9] = uint32_t(buffer >> 32) & 0xff;
  user[10] = 4096;
  user[11] = 4 | (5 << 3) | (6 << 6) | (7 << 9) | (3u << 27);
  user[12] = uint32_t(destination);
  user[13] = uint32_t(destination >> 32) & 0xff;
  user[14] = 8;
  user[15] = user[11];
  auto *words = reinterpret_cast<uint32_t *>(bytes + 0x30000);
  const int initialized = sceGnmDrawInitDefaultHardwareState350(words, 0x1000);
  if (initialized <= 0 || initialized > 0x1000) {
    sceKernelMunmap(segmented, 0x20000);
    return false;
  }
  Commands commands{words, words + initialized};
  commands.Register(0x2e04, 0);
  commands.Register(0x2e05, 0);
  commands.Register(0x2e06, 0);
  commands.Register(0x2e07, 1);
  commands.Register(0x2e08, 1);
  commands.Register(0x2e09, 1);
  commands.Register(0x2e0c, uint32_t(uintptr_t(shader) >> 8));
  commands.Register(0x2e0d, uint32_t(uintptr_t(shader) >> 40));
  commands.Register(0x2e12, 1);
  commands.Register(0x2e13, 16 << 1);
  commands.Register(0x2e15, 0x170);
  commands.Register(0x2e16, 0xffffffff);
  commands.Register(0x2e17, 0xffffffff);
  commands.Packet(0x76,
                  {0x240, user[0], user[1], user[2], user[3], user[4], user[5],
                   user[6], user[7], user[8], user[9], user[10], user[11],
                   user[12], user[13], user[14], user[15]},
                  true);
  commands.Packet(0x15, {1, 1, 1, 0x21}, true);
  auto *fence = reinterpret_cast<volatile uint32_t *>(bytes + 0x20100);
  *fence = 0;
  const auto completion = uintptr_t(fence);
  commands.Packet(0x48,
                  {47u | (6u << 8), uint32_t(completion),
                   uint32_t(completion >> 32) | (2u << 29), 1},
                  true);
  commands.Packet(0x47, {20u | (5u << 8), uint32_t(completion),
                         uint32_t(completion >> 32) | (1u << 29), 2, 0});
  void *addresses[]{words};
  uint32_t sizes[]{uint32_t((commands.current - commands.begin) * 4)};
  rc = sceGnmSubmitCommandBuffers(1, addresses, sizes, nullptr, nullptr);
  bool timed_out{};
  if (rc >= 0)
    for (uint32_t wait = 0; *fence != 2; ++wait) {
      if (wait == 10000) {
        timed_out = true;
        std::printf("SEGMENTED_FENCE TIMEOUT; retaining submitted memory\n");
      }
      sceKernelUsleep(1000);
    }
  const auto first_physical =
      *reinterpret_cast<volatile uint32_t *>(bytes + 0x50fffc);
  const auto second_physical =
      *reinterpret_cast<volatile uint32_t *>(bytes + 0x600000);
  const bool pass = rc >= 0 && !timed_out && output[0] == 0x11223344 &&
                    output[1] == 0x11223344 && first_physical == output[0] &&
                    second_physical == output[1];
  std::printf("SEGMENTED_IMAGE_BUFFER_ALIAS %s image=%08x buffer=%08x "
              "physical=%08x,%08x submit=%08x\n",
              pass ? "PASS" : "FAIL", output[0], output[1], first_physical,
              second_physical, uint32_t(rc));
  sceKernelMunmap(segmented, 0x20000);
  return pass;
}
bool RunNonlinearAliases(uint8_t *bytes, uint8_t *alias_bytes, off_t physical,
                         bool repeated) {
  void *segmented{};
  int rc = sceKernelMapDirectMemory(&segmented, 0x20000, 0x33, 0,
                                    physical + (repeated ? 0x500000 : 0x600000),
                                    0x10000);
  if (rc < 0)
    return false;
  auto *second = static_cast<uint8_t *>(segmented) + 0x10000;
  rc = sceKernelMunmap(second, 0x10000);
  if (rc < 0) {
    sceKernelMunmap(segmented, 0x20000);
    return false;
  }
  void *fixed = second;
  rc = sceKernelMapDirectMemory(&fixed, 0x10000, 0x33, 0x10,
                                physical + 0x500000, 0x10000);
  if (rc < 0) {
    sceKernelMunmap(segmented, 0x10000);
    return false;
  }
  std::memset(bytes + 0x500000, 0xac, 0x10000);
  std::memset(bytes + 0x600000, 0xac, 0x10000);
  auto *output = reinterpret_cast<volatile uint32_t *>(bytes + 0x40000);
  output[0] = output[1] = 0xdeadbeef;
  auto *shader = reinterpret_cast<uint32_t *>(bytes + 0x12000);
  const uint32_t code[]{
      0x7e0002ff, 0x10000,    0x7e020287, 0xe0701000, 0x80000100, 0xbf8c0f70,
      0x7e000280, 0x7e020281, 0xe0c85000, 0x80010100, 0xbf8c0f70, 0x7e0002ff,
      0x10000,    0xe0301000, 0x80000200, 0xbf8c0f70, 0x7e000280, 0xe0701000,
      0x80020100, 0xe0701004, 0x80020200, 0xbf8c0f70, 0xbf810000};
  std::memcpy(shader, code, sizeof(code));
  const ShaderBinaryInfo metadata{{'O', 'r', 'b', 'S', 'h', 'd', 'r'}, 1,
                                  uint32_t(sizeof(code)) << 8,         0,
                                  0x6e6f6e6c696e0000ULL + repeated,    0};
  std::memcpy(bytes + 0x12080, &metadata, sizeof(metadata));
  uint32_t user[16]{};
  const uintptr_t bases[]{uintptr_t(segmented),
                          uintptr_t(alias_bytes + 0x500000), uintptr_t(output)};
  const uint32_t sizes[]{0x20000, 0x110000, 8};
  for (uint32_t index = 0; index < 3; ++index) {
    user[index * 4] = uint32_t(bases[index]);
    user[index * 4 + 1] = uint32_t(bases[index] >> 32) & 0xff;
    user[index * 4 + 2] = sizes[index];
    user[index * 4 + 3] = 4 | (5 << 3) | (6 << 6) | (7 << 9) | (3u << 27);
  }
  auto *words = reinterpret_cast<uint32_t *>(bytes + 0x30000);
  const int initialized = sceGnmDrawInitDefaultHardwareState350(words, 0x1000);
  if (initialized <= 0 || initialized > 0x1000) {
    sceKernelMunmap(segmented, 0x20000);
    return false;
  }
  Commands commands{words, words + initialized};
  commands.Register(0x2e04, 0);
  commands.Register(0x2e05, 0);
  commands.Register(0x2e06, 0);
  commands.Register(0x2e07, 1);
  commands.Register(0x2e08, 1);
  commands.Register(0x2e09, 1);
  commands.Register(0x2e0c, uint32_t(uintptr_t(shader) >> 8));
  commands.Register(0x2e0d, uint32_t(uintptr_t(shader) >> 40));
  commands.Register(0x2e12, 1);
  commands.Register(0x2e13, 16 << 1);
  commands.Register(0x2e15, 0x170);
  commands.Register(0x2e16, 0xffffffff);
  commands.Register(0x2e17, 0xffffffff);
  commands.Packet(0x76,
                  {0x240, user[0], user[1], user[2], user[3], user[4], user[5],
                   user[6], user[7], user[8], user[9], user[10], user[11],
                   user[12], user[13], user[14], user[15]},
                  true);
  commands.Packet(0x15, {1, 1, 1, 0x21}, true);
  auto *fence = reinterpret_cast<volatile uint32_t *>(bytes + 0x20100);
  *fence = 0;
  const auto completion = uintptr_t(fence);
  commands.Packet(0x48,
                  {47u | (6u << 8), uint32_t(completion),
                   uint32_t(completion >> 32) | (2u << 29), 1},
                  true);
  commands.Packet(0x47, {20u | (5u << 8), uint32_t(completion),
                         uint32_t(completion >> 32) | (1u << 29), 2, 0});
  void *addresses[]{words};
  uint32_t sizes[]{uint32_t((commands.current - commands.begin) * 4)};
  rc = sceGnmSubmitCommandBuffers(1, addresses, sizes, nullptr, nullptr);
  bool timed_out{};
  if (rc >= 0)
    for (uint32_t wait = 0; *fence != 2; ++wait) {
      if (wait == 10000) {
        timed_out = true;
        std::printf("NONLINEAR_FENCE TIMEOUT; retaining submitted memory\n");
      }
      sceKernelUsleep(1000);
    }
  const auto physical_value =
      *reinterpret_cast<volatile uint32_t *>(bytes + 0x500000);
  bool padding = true;
  for (uint32_t offset = 4; offset < 0x10000; ++offset)
    padding &= bytes[0x500000 + offset] == 0xac;
  for (uint32_t offset = 0; offset < 0x10000; ++offset)
    padding &= bytes[0x600000 + offset] == 0xac;
  const bool pass = rc >= 0 && !timed_out && output[0] == 7 && output[1] == 8 &&
                    physical_value == 8 && padding;
  std::printf("NONLINEAR_PHYSICAL_ALIAS repeated=%u %s old=%u observed=%u "
              "physical=%u padding=%s submit=%08x\n",
              repeated, pass ? "PASS" : "FAIL", output[0], output[1],
              physical_value, padding ? "PASS" : "FAIL", uint32_t(rc));
  sceKernelMunmap(segmented, 0x20000);
  return pass;
}
bool RunImageFeedback(uint8_t *bytes, bool integer_constant = false,
                      uint8_t *read_alias = nullptr,
                      bool read_existing = false) {
  auto *source = reinterpret_cast<volatile uint32_t *>(bytes + 0x40000);
  if (!read_existing)
    source[0] = 0xdeadbeef;
  source[1] = 0xdeadbeef;
  const uint32_t original = read_existing ? source[0] : 0x12345678;
  const uint32_t variant = uint32_t(integer_constant) + (read_alias ? 2 : 0) +
                           (read_existing ? 4 : 0);
  auto *shader = reinterpret_cast<uint32_t *>(bytes + 0x11000 + variant * 0x80);
  uint32_t code[]{0x7e000280, 0x7e020280, 0x7e0802ff, 0x12345678, 0xf0202100,
                  0x00000400, 0xbf8c0f70, 0xf0002100, 0x00020400, 0xbf8c0f70,
                  0x7e000281, 0xf0202100, 0x00000400, 0xbf8c0f70, 0xbf810000};
  if (read_existing)
    code[4] = code[5] = 0xbf800000;
  std::memcpy(shader, code, sizeof(code));
  const ShaderBinaryInfo metadata{{'O', 'r', 'b', 'S', 'h', 'd', 'r'}, 1,
                                  uint32_t(sizeof(code)) << 8,         0,
                                  0x666565646261636b + variant,        0};
  std::memcpy(bytes + 0x11040 + variant * 0x80, &metadata, sizeof(metadata));
  uint64_t image[4]{};
  image[0] =
      (uintptr_t(source) >> 8) | (uint64_t(4) << 52) | (uint64_t(4) << 58);
  image[1] = 15 | (uint64_t(15) << 14) | (uint64_t(4) << 32) |
             (uint64_t(5) << 35) | (uint64_t(6) << 38) | (uint64_t(7) << 41) |
             (uint64_t(8) << 52) | (uint64_t(9) << 60);
  image[2] = uint64_t(63) << 13;

  uint32_t user[16]{};
  std::memcpy(user, image, sizeof(image));
  std::memcpy(user + 8, image, sizeof(image));
  if (read_alias) {
    const uint64_t reader = (uintptr_t(read_alias + 0x40000) >> 8) |
                            (uint64_t(4) << 52) | (uint64_t(4) << 58);
    user[8] = uint32_t(reader);
    user[9] = uint32_t(reader >> 32);
  }
  if (integer_constant)
    user[11] = (user[11] & ~7u) | 1;
  int rc{};
  auto *words = reinterpret_cast<uint32_t *>(bytes + 0x30000);
  const int initialized = sceGnmDrawInitDefaultHardwareState350(words, 0x1000);
  if (initialized <= 0 || initialized > 0x1000) {
    return false;
  }
  Commands commands{words, words + initialized};
  commands.Register(0x2e04, 0);
  commands.Register(0x2e05, 0);
  commands.Register(0x2e06, 0);
  commands.Register(0x2e07, 1);
  commands.Register(0x2e08, 1);
  commands.Register(0x2e09, 1);
  commands.Register(0x2e0c, uint32_t(uintptr_t(shader) >> 8));
  commands.Register(0x2e0d, uint32_t(uintptr_t(shader) >> 40));
  commands.Register(0x2e12, 1);
  commands.Register(0x2e13, 16 << 1);
  commands.Register(0x2e15, 0x170);
  commands.Register(0x2e16, 0xffffffff);
  commands.Register(0x2e17, 0xffffffff);
  commands.Packet(0x76,
                  {0x240, user[0], user[1], user[2], user[3], user[4], user[5],
                   user[6], user[7], user[8], user[9], user[10], user[11],
                   user[12], user[13], user[14], user[15]},
                  true);
  commands.Packet(0x15, {1, 1, 1, 0x21}, true);
  auto *fence = reinterpret_cast<volatile uint32_t *>(bytes + 0x20100);
  *fence = 0;
  const auto completion = uintptr_t(fence);
  commands.Packet(0x48,
                  {47u | (6u << 8), uint32_t(completion),
                   uint32_t(completion >> 32) | (2u << 29), 1},
                  true);
  commands.Packet(0x47, {20u | (5u << 8), uint32_t(completion),
                         uint32_t(completion >> 32) | (1u << 29), 2, 0});
  void *addresses[]{words};
  uint32_t sizes[]{uint32_t((commands.current - commands.begin) * 4)};
  rc = sceGnmSubmitCommandBuffers(1, addresses, sizes, nullptr, nullptr);
  bool timed_out{};
  if (rc >= 0)
    for (uint32_t wait = 0; *fence != 2; ++wait) {
      if (wait == 10000) {
        timed_out = true;
        std::printf("FEEDBACK_FENCE TIMEOUT; retaining submitted memory\n");
      }
      sceKernelUsleep(1000);
    }
  const uint32_t expected = integer_constant ? 1 : original;
  const bool pass =
      rc >= 0 && !timed_out && source[0] == original && source[1] == expected;
  std::printf("IMAGE_LOAD_FEEDBACK %s constant=%u virtual_alias=%u writer=%08x "
              "reader=%08x "
              "expected=%08x submit=%08x\n",
              pass ? "PASS" : "FAIL", uint32_t(integer_constant),
              uint32_t(read_alias != nullptr), source[0], source[1], expected,
              uint32_t(rc));
  return pass;
}
bool RunPersistentImageTransitions(uint8_t *bytes, uint8_t *read_alias) {
  if (!RunImageFeedback(bytes, false, read_alias))
    return false;
  auto *alias = reinterpret_cast<volatile uint32_t *>(read_alias + 0x40000);
  const bool gpu_read = alias[0] == 0x12345678;
  alias[0] = 0x87654321;
  alias[63] = 0xaabbccdd;
  const bool gpu_write = RunImageFeedback(bytes, false, read_alias, true);
  const bool pass = gpu_read && gpu_write && alias[0] == 0x87654321 &&
                    alias[1] == 0x87654321 && alias[63] == 0xaabbccdd;
  std::printf(
      "IMAGE_RESIDENCY_CPU_GPU phases=2 virtual_alias=%u padding=%s %s\n",
      uint32_t(read_alias != bytes), alias[63] == 0xaabbccdd ? "PASS" : "FAIL",
      pass ? "PASS" : "FAIL");
  return pass;
}
bool RunImageBufferWrites(uint8_t *bytes, uint32_t format = 4,
                          bool alternate = false, uint32_t bank = 0) {
  if (alternate && !sceKernelIsNeoMode()) {
    std::printf("IMAGE_BUFFER_ALT SKIP requires Neo mode\n");
    return true;
  }
  auto *source = reinterpret_cast<volatile uint32_t *>(bytes + 0x40000);
  auto *output = reinterpret_cast<volatile uint32_t *>(bytes + 0x20000);
  const uint32_t source_words = alternate ? 0x4000 : 1;
  for (uint32_t i = 0; i < source_words; ++i)
    source[i] = 0xdeadbeef;
  output[0] = output[1] = 0xdeadbeef;
  auto *shader = reinterpret_cast<uint32_t *>(bytes + 0x12000);
  uint32_t code[]{0x7e000280, 0x7e020280, 0x7e040280, 0x7e060280, 0x7e0802ff,
                  0x12345678, 0xf0202100, 0x00000400, 0xbf8c0f70, 0xe0305000,
                  0x80020502, 0xbf8c0f70, 0xe0705000, 0x80030503, 0x7e0802ff,
                  0x87654321, 0xe0705000, 0x80020402, 0xbf8c0f70, 0xf0002100,
                  0x00000400, 0xbf8c0f70, 0xe0705004, 0x80030403, 0xbf810000};
  const uint32_t bits = format == 1 ? 8 : format == 4 ? 32 : 16;
  const uint32_t offset = alternate    ? bank << 12
                          : bits == 8  ? 3
                          : bits == 16 ? 2
                                       : 0;
  const uint32_t first = bits == 32 ? 0x12345678 : 0x78;
  const uint32_t last = bits == 32 ? 0x87654321 : 0x21;
  const uint32_t mask = bits == 32 ? 0xffffffff : (1u << bits) - 1;
  code[0] = bits == 32 ? 0x7e000280 : bits == 8 ? 0x7e000283 : 0x7e000281;
  code[5] = first;
  code[9] = bits == 8 ? 0xe0205000 : bits == 16 ? 0xe0285000 : 0xe0305000;
  code[15] = last;
  code[16] = bits == 8 ? 0xe0605000 : bits == 16 ? 0xe0685000 : 0xe0705000;
  std::memcpy(shader, code, sizeof(code));
  const ShaderBinaryInfo metadata{{'O', 'r', 'b', 'S', 'h', 'd', 'r'},
                                  1,
                                  uint32_t(sizeof(code)) << 8,
                                  0,
                                  0x696d676275666665,
                                  0};
  std::memcpy(bytes + 0x12080, &metadata, sizeof(metadata));
  uint64_t image[4]{};
  image[0] = ((uintptr_t(source) | (uintptr_t(bank) << 12)) >> 8) |
             (uint64_t(format) << 52) | (uint64_t(4) << 58);
  image[1] = 15 | (uint64_t(15) << 14) | (uint64_t(4) << 32) |
             (uint64_t(5) << 35) | (uint64_t(6) << 38) | (uint64_t(7) << 41) |
             (uint64_t(8) << 52) | (uint64_t(9) << 60);
  image[2] = uint64_t(63) << 13;
  if (alternate) {
    image[1] = 127 | (uint64_t(127) << 14) | (uint64_t(4) << 32) |
               (uint64_t(5) << 35) | (uint64_t(6) << 38) | (uint64_t(7) << 41) |
               (uint64_t(10) << 52) | (uint64_t(9) << 60);
    image[2] = uint64_t(127) << 13;
    image[3] = uint64_t(1) << 24;
  }
  uint32_t user[16]{};
  std::memcpy(user, image, sizeof(image));
  user[8] = uint32_t(uintptr_t(source) + offset);
  user[9] = uint32_t(uintptr_t(source) >> 32) & 0xff;
  user[10] = bits / 8;
  user[11] = 4 | (5 << 3) | (6 << 6) | (7 << 9) | (3u << 27);
  user[12] = uint32_t(uintptr_t(output));
  user[13] = uint32_t(uintptr_t(output) >> 32) & 0xff;
  user[14] = 8;
  user[15] = user[11];
  int rc{};
  auto *words = reinterpret_cast<uint32_t *>(bytes + 0x30000);
  const int initialized = sceGnmDrawInitDefaultHardwareState350(words, 0x1000);
  if (initialized <= 0 || initialized > 0x1000) {
    return false;
  }
  Commands commands{words, words + initialized};
  commands.Register(0x2e04, 0);
  commands.Register(0x2e05, 0);
  commands.Register(0x2e06, 0);
  commands.Register(0x2e07, 1);
  commands.Register(0x2e08, 1);
  commands.Register(0x2e09, 1);
  commands.Register(0x2e0c, uint32_t(uintptr_t(shader) >> 8));
  commands.Register(0x2e0d, uint32_t(uintptr_t(shader) >> 40));
  commands.Register(0x2e12, 1);
  commands.Register(0x2e13, 16 << 1);
  commands.Register(0x2e15, 0x170);
  commands.Register(0x2e16, 0xffffffff);
  commands.Register(0x2e17, 0xffffffff);
  commands.Packet(0x76,
                  {0x240, user[0], user[1], user[2], user[3], user[4], user[5],
                   user[6], user[7], user[8], user[9], user[10], user[11],
                   user[12], user[13], user[14], user[15]},
                  true);
  commands.Packet(0x15, {1, 1, 1, 0x21}, true);
  auto *fence = reinterpret_cast<volatile uint32_t *>(bytes + 0x20100);
  *fence = 0;
  const auto completion = uintptr_t(fence);
  commands.Packet(0x48,
                  {47u | (6u << 8), uint32_t(completion),
                   uint32_t(completion >> 32) | (2u << 29), 1},
                  true);
  commands.Packet(0x47, {20u | (5u << 8), uint32_t(completion),
                         uint32_t(completion >> 32) | (1u << 29), 2, 0});
  void *addresses[]{words};
  uint32_t sizes[]{uint32_t((commands.current - commands.begin) * 4)};
  rc = sceGnmSubmitCommandBuffers(1, addresses, sizes, nullptr, nullptr);
  bool timed_out{};
  if (rc >= 0)
    for (uint32_t wait = 0; *fence != 2; ++wait) {
      if (wait == 10000) {
        timed_out = true;
        std::printf("IMAGE_BUFFER_FENCE TIMEOUT; retaining submitted memory\n");
      }
      sceKernelUsleep(1000);
    }
  const uint32_t bit_offset = (offset & 3) * 8;
  const uint32_t expected_word =
      (0xdeadbeef & ~(mask << bit_offset)) | (last << bit_offset);
  bool neighbors = true;
  for (uint32_t i = 0; i < source_words; ++i)
    neighbors &= source[i] == (i == offset / 4 ? expected_word : 0xdeadbeef);
  const bool pass = rc >= 0 && !timed_out && output[0] == first &&
                    output[1] == last && neighbors;
  std::printf("IMAGE_BUFFER_WRITES format=%u bits=%u alt=%u bank=%u %s "
              "image_to_buffer=%08x buffer_to_image=%08x "
              "source=%08x submit=%08x\n",
              format, bits, uint32_t(alternate), bank, pass ? "PASS" : "FAIL",
              output[0], output[1], source[offset / 4], uint32_t(rc));
  return pass;
}
struct ImageBufferAtomicCase {
  uint32_t image_op, buffer_op, initial, value, compare, result;
};

bool RunImageBufferAtomic(uint8_t *bytes, const ImageBufferAtomicCase &test,
                          bool reverse, uint32_t index) {
  auto *source = reinterpret_cast<volatile uint32_t *>(bytes + 0x40000);
  auto *output = reinterpret_cast<volatile uint32_t *>(bytes + 0x20000);
  source[0] = test.initial;
  source[1] = 0xdeadbeef;
  output[0] = output[1] = 0xdeadbeef;
  auto *shader = reinterpret_cast<uint32_t *>(bytes + 0x50000 + index * 0x100);
  const uint32_t image_op = reverse ? 17 : test.image_op;
  const uint32_t buffer_op = reverse ? test.buffer_op : 50;
  const uint32_t image_code[]{0xf0002100u | (image_op << 18), 0x00000400,
                              0xbf8c0f70};
  const uint32_t buffer_code[]{0xe0005000u | (buffer_op << 18), 0x80020602,
                               0xbf8c0f70};
  uint32_t code[]{0x7e000280, 0x7e020280,
                  0x7e040280, 0x7e060280,
                  0x7e0802ff, reverse ? 0 : test.value,
                  0x7e0a02ff, test.compare,
                  0x7e0c02ff, reverse ? test.value : 0,
                  0x7e0e02ff, test.compare,
                  0,          0,
                  0,          0,
                  0,          0,
                  0xe0701000, reverse ? 0x80030603u : 0x80030403u,
                  0xe0701004, reverse ? 0x80030403u : 0x80030603u,
                  0xbf810000};
  std::memcpy(code + 12, reverse ? buffer_code : image_code,
              sizeof(image_code));
  std::memcpy(code + 15, reverse ? image_code : buffer_code,
              sizeof(image_code));
  std::memcpy(shader, code, sizeof(code));
  const ShaderBinaryInfo metadata{{'O', 'r', 'b', 'S', 'h', 'd', 'r'}, 1,
                                  uint32_t(sizeof(code)) << 8,         0,
                                  0x61746f6d6963616c + index,          0};
  std::memcpy(bytes + 0x50080 + index * 0x100, &metadata, sizeof(metadata));
  const uint32_t number_format = test.image_op >= 29 ? 7 : 4;
  const uint64_t image[]{(uintptr_t(source) >> 8) | (uint64_t(4) << 52) |
                             (uint64_t(number_format) << 58),
                         15 | (uint64_t(15) << 14) | (uint64_t(4) << 32) |
                             (uint64_t(5) << 35) | (uint64_t(6) << 38) |
                             (uint64_t(7) << 41) | (uint64_t(8) << 52) |
                             (uint64_t(9) << 60),
                         uint64_t(63) << 13, 0};
  uint32_t user[16]{};
  std::memcpy(user, image, sizeof(image));
  user[8] = uint32_t(uintptr_t(source));
  user[9] = uint32_t(uintptr_t(source) >> 32) & 0xff;
  user[10] = 4;
  user[11] = 4 | (5 << 3) | (6 << 6) | (7 << 9) | (3u << 27);
  user[12] = uint32_t(uintptr_t(output));
  user[13] = uint32_t(uintptr_t(output) >> 32) & 0xff;
  user[14] = 8;
  user[15] = user[11];
  int rc{};
  auto *words = reinterpret_cast<uint32_t *>(bytes + 0x30000);
  const int initialized = sceGnmDrawInitDefaultHardwareState350(words, 0x1000);
  if (initialized <= 0 || initialized > 0x1000) {
    return false;
  }
  Commands commands{words, words + initialized};
  commands.Register(0x2e04, 0);
  commands.Register(0x2e05, 0);
  commands.Register(0x2e06, 0);
  commands.Register(0x2e07, 1);
  commands.Register(0x2e08, 1);
  commands.Register(0x2e09, 1);
  commands.Register(0x2e0c, uint32_t(uintptr_t(shader) >> 8));
  commands.Register(0x2e0d, uint32_t(uintptr_t(shader) >> 40));
  commands.Register(0x2e12, 1);
  commands.Register(0x2e13, 16 << 1);
  commands.Register(0x2e15, 0x170);
  commands.Register(0x2e16, 0xffffffff);
  commands.Register(0x2e17, 0xffffffff);
  commands.Packet(0x76,
                  {0x240, user[0], user[1], user[2], user[3], user[4], user[5],
                   user[6], user[7], user[8], user[9], user[10], user[11],
                   user[12], user[13], user[14], user[15]},
                  true);
  commands.Packet(0x15, {1, 1, 1, 0x21}, true);
  auto *fence = reinterpret_cast<volatile uint32_t *>(bytes + 0x20100);
  *fence = 0;
  const auto completion = uintptr_t(fence);
  commands.Packet(0x48,
                  {47u | (6u << 8), uint32_t(completion),
                   uint32_t(completion >> 32) | (2u << 29), 1},
                  true);
  commands.Packet(0x47, {20u | (5u << 8), uint32_t(completion),
                         uint32_t(completion >> 32) | (1u << 29), 2, 0});
  void *addresses[]{words};
  uint32_t sizes[]{uint32_t((commands.current - commands.begin) * 4)};
  rc = sceGnmSubmitCommandBuffers(1, addresses, sizes, nullptr, nullptr);
  bool timed_out{};
  if (rc >= 0)
    for (uint32_t wait = 0; *fence != 2; ++wait) {
      if (wait == 10000) {
        timed_out = true;
        std::printf("IMAGE_BUFFER_FENCE TIMEOUT; retaining submitted memory\n");
      }
      sceKernelUsleep(1000);
    }
  const bool pass = rc >= 0 && !timed_out && output[0] == test.initial &&
                    output[1] == test.result && source[0] == test.result &&
                    source[1] == 0xdeadbeef;
  std::printf("IMAGE_BUFFER_ATOMIC op=%u reverse=%u initial=%08x value=%08x "
              "compare=%08x "
              "old=%08x result=%08x expected=%08x %s\n",
              test.image_op, uint32_t(reverse), test.initial, test.value,
              test.compare, output[0], output[1], test.result,
              pass ? "PASS" : "FAIL");
  return pass;
}

bool RunImageBufferAtomics(uint8_t *bytes) {
  constexpr ImageBufferAtomicCase cases[]{
      {18, 51, 7, 9, 0, 0xfffffffe},
      {27, 60, 2, 7, 0, 3},
      {27, 60, 7, 7, 0, 0},
      {27, 60, 8, 7, 0, 0},
      {27, 60, 0, 0, 0, 0},
      {27, 60, 0xffffffff, 0xffffffff, 0, 0},
      {28, 61, 2, 7, 0, 1},
      {28, 61, 7, 7, 0, 6},
      {28, 61, 8, 7, 0, 7},
      {28, 61, 0, 7, 0, 7},
      {28, 61, 0, 0, 0, 0},
      {28, 61, 0, 0xffffffff, 0, 0xffffffff},
      {30, 63, 0x40000000, 0xbf800000, 0, 0xbf800000},
      {30, 63, 0xbf800000, 0x40000000, 0, 0xbf800000},
      {30, 63, 0xc0000000, 0xbf800000, 0, 0xc0000000},
      {30, 63, 0xbf800000, 0xc0000000, 0, 0xc0000000},
      {31, 64, 0x40000000, 0xbf800000, 0, 0x40000000},
      {31, 64, 0xbf800000, 0x40000000, 0, 0x40000000},
      {31, 64, 0xc0000000, 0xbf800000, 0, 0xbf800000},
      {31, 64, 0xbf800000, 0xc0000000, 0, 0xbf800000},
      {30, 63, 0, 0x80000000, 0, 0},
      {31, 64, 0x80000000, 0, 0, 0x80000000},
      {30, 63, 0x7fc12345, 0xbf800000, 0, 0x7fc12345},
      {31, 64, 0x7f812345, 0x3f800000, 0, 0x7f812345},
      {30, 63, 0x3f800000, 0x7fc12345, 0, 0x3f800000},
      {31, 64, 0xbf800000, 0xff812345, 0, 0xbf800000},
      {30, 63, 0x7f800000, 0xff800000, 0, 0xff800000},
      {31, 64, 0xff800000, 0x7f800000, 0, 0x7f800000},
      {30, 63, 2, 1, 0, 1},
      {31, 64, 0x80000002, 0x80000001, 0, 0x80000001},
      {29, 62, 0, 0x3f800000, 0x80000000, 0x3f800000},
      {29, 62, 0x80000000, 0xbf800000, 0, 0xbf800000},
      {29, 62, 0x7fc12345, 0, 0x7fc12345, 0x7fc12345},
      {29, 62, 0x3f800000, 0, 0x7fc12345, 0x3f800000},
      {29, 62, 1, 0x7f812345, 1, 0x7f812345},
      {29, 62, 0x3f800000, 0, 0x40000000, 0x3f800000},
      {29, 62, 0x7f800000, 0xff800000, 0x7f800000, 0xff800000},
      {15, 48, 3, 8, 0, 8},
      {16, 49, 3, 8, 3, 8},
      {16, 49, 3, 8, 2, 3},
      {20, 53, 0xffffffff, 2, 0, 0xffffffff},
      {21, 54, 0xffffffff, 2, 0, 2},
      {22, 55, 0xffffffff, 2, 0, 2},
      {23, 56, 0xffffffff, 2, 0, 0xffffffff},
      {24, 57, 0xff, 0x18, 0, 0x18},
      {25, 58, 0xff, 0x100, 0, 0x1ff},
      {26, 59, 0xff, 0x18, 0, 0xe7}};
  bool pass = true;
  uint32_t index{};
  for (const auto &test : cases) {
    pass &= RunImageBufferAtomic(bytes, test, false, index++);
    pass &= RunImageBufferAtomic(bytes, test, true, index++);
  }
  return pass;
}

bool RunStageSharing(uint8_t *bytes, uint8_t *read_alias, uint32_t blend_case = 0) {
  auto *source = reinterpret_cast<volatile uint32_t *>(bytes + 0x40000);
  auto *target = reinterpret_cast<volatile uint32_t *>(bytes + 0x60000);
  for (uint32_t i = 0; i < 64 * 32; ++i)
    source[i] = 0xdeadbeef;
  for (uint32_t i = 0; i < 64 * 16; ++i)
    target[i] = 0xdeadbeef;
  const auto vop1 = [](uint32_t op, uint32_t dst, uint32_t src) {
    return 0x7e000000u | (dst << 17) | (op << 9) | src;
  };
  const auto vop2 = [](uint32_t op, uint32_t dst, uint32_t src0,
                       uint32_t src1) {
    return (op << 25) | (dst << 17) | (src1 << 9) | src0;
  };
  const uint32_t vertex[]{
      vop1(1, 8, 256),     vop1(1, 9, 128),     vop1(1, 10, 128),
      vop1(1, 11, 128),    vop1(1, 12, 255),    0x3f000000,
      0xf0203100,          0x00000c08,          0xbf8c0000,
      vop2(26, 1, 129, 0), vop2(27, 1, 130, 1), vop2(27, 2, 130, 0),
      vop1(6, 1, 257),     vop1(6, 2, 258),     vop2(8, 1, 244, 1),
      vop2(8, 2, 244, 2),  vop2(3, 1, 243, 1),  vop2(3, 2, 243, 2),
      vop1(1, 3, 128),     vop1(1, 4, 242),     0xf80008cf,
      0x04030201,          0xbf810000};
  const uint32_t pixel[]{
      vop1(7, 8, 256),  vop1(7, 9, 257), vop2(37, 9, 144, 9), vop1(1, 10, 128),
      vop1(1, 11, 128), vop1(1, 0, 128), vop1(1, 1, 128),     vop1(1, 2, 128),
      vop1(1, 3, 128),  0xf0003100,      0x00000400,          0xbf8c0000,
      0xf0203100,       0x00000408,      0xbf8c0000,          vop1(1, 5, 128),
      vop1(1, 6, 128),  vop1(1, 7, 242), 0xf800180f,          0x07060504,
      0xbf810000};
  const uint32_t variant = uint32_t(read_alias != bytes);
  auto *vs = bytes + 0x70000 + variant * 0x400;
  auto *ps = bytes + 0x71000 + variant * 0x400;
  std::memcpy(vs, vertex, sizeof(vertex));
  std::memcpy(ps, pixel, sizeof(pixel));
  uint32_t vertex_length = sizeof(vertex), pixel_length = sizeof(pixel);
  if (blend_case >= 13) {
    const uint32_t strip_vertex[]{
        vop2(27, 1, 129, 0), vop2(22, 2, 129, 0),
        vop1(6, 1, 257), vop1(6, 2, 258),
        vop2(8, 1, 244, 1), vop2(8, 2, 244, 2),
        vop2(3, 1, 243, 1), vop2(3, 2, 243, 2),
        vop1(1, 3, 128), vop1(1, 4, 242),
        vop1(6, 5, 256), vop1(1, 6, 255), 0x3e800000,
        vop2(8, 5, 262, 5), 0xf80000cf, 0x04030201,
        0xf8000a0f, 0x05050505, 0xbf810000};
    const uint32_t strip_pixel[]{0xc8020002, vop1(1, 1, 242),
                                  0xf800180f, 0x01000000, 0xbf810000};
    std::memcpy(vs, strip_vertex, sizeof(strip_vertex));
    std::memcpy(ps, strip_pixel, sizeof(strip_pixel));
    vertex_length = sizeof(strip_vertex);
    pixel_length = sizeof(strip_pixel);
  }
  const ShaderBinaryInfo vs_info{{'O', 'r', 'b', 'S', 'h', 'd', 'r'}, 1,
                                 vertex_length << 8,       0,
                                 0x7374616765767300 + variant + (blend_case >= 13 ? 0x100 : 0),        0};
  const ShaderBinaryInfo ps_info{{'O', 'r', 'b', 'S', 'h', 'd', 'r'}, 1,
                                 pixel_length << 8,        0,
                                 0x7374616765707300 + variant + (blend_case >= 13 ? 0x100 : 0),        0};
  std::memcpy(vs + 0x80, &vs_info, sizeof(vs_info));
  std::memcpy(ps + 0x80, &ps_info, sizeof(ps_info));
  uint64_t image[]{
      (uintptr_t(source) >> 8) | (uint64_t(4) << 52) | (uint64_t(7) << 58),
      15 | (uint64_t(31) << 14) | (uint64_t(4) << 32) | (uint64_t(5) << 35) |
          (uint64_t(6) << 38) | (uint64_t(7) << 41) | (uint64_t(8) << 52) |
          (uint64_t(9) << 60),
      uint64_t(63) << 13, 0};
  uint32_t vs_user[8]{}, ps_user[8]{};
  std::memcpy(vs_user, image, sizeof(image));
  image[0] = (uintptr_t(read_alias + 0x40000) >> 8) | (uint64_t(4) << 52) |
             (uint64_t(7) << 58);
  std::memcpy(ps_user, image, sizeof(image));
  auto *words = reinterpret_cast<uint32_t *>(bytes + 0x30000);
  const int initialized = sceGnmDrawInitDefaultHardwareState350(words, 0x1000);
  if (initialized <= 0 || initialized > 0x1000)
    return false;
  Commands commands{words, words + initialized};
  const auto shader = [&](uint32_t reg, uint32_t value) {
    commands.Packet(0x76, {reg - 0x2c00, value});
  };
  const auto context = [&](uint32_t reg, uint32_t value) {
    commands.Packet(0x69, {reg - 0xa000, value});
  };
  shader(0x2c48, uint32_t(uintptr_t(vs) >> 8));
  shader(0x2c49, uint32_t(uintptr_t(vs) >> 40));
  shader(0x2c4a, 3 | (1 << 6));
  shader(0x2c4b, 8 << 1);
  shader(0x2c08, uint32_t(uintptr_t(ps) >> 8));
  shader(0x2c09, uint32_t(uintptr_t(ps) >> 40));
  shader(0x2c0a, 3 | (1 << 6));
  shader(0x2c0b, 8 << 1);
  for (uint32_t i = 0; i < 8; ++i) {
    shader(0x2c4c + i, vs_user[i]);
    shader(0x2c0c + i, ps_user[i]);
  }
  context(0xa2d5, 0);
  context(0xa290, 0);
  context(0xa1b1, 0);
  context(0xa1b3, 0x300);
  context(0xa1b4, 0x300);
  context(0xa1b6, 0);
  context(0xa1c3, 4);
  context(0xa1c4, 0);
  context(0xa1c5, 9);
  context(0xa200, 0);
  context(0xa202, 0x00cc0010);
  context(0xa203, 0xe00);
  context(0xa204, 1 << 19);
  context(0xa205, 0);
  context(0xa206, 0x3f);
  context(0xa207, 0);
  context(0xa292, 2);
  context(0xa2f8, 0);
  context(0xa08e, 15);
  context(0xa08f, 15);
  for (uint32_t reg : {0xa00c, 0xa081, 0xa090, 0xa094}) {
    context(reg, reg == 0xa00c ? 0 : 0x80000000);
    context(reg + 1, (16 << 16) | 16);
  }
  context(0xa0b4, 0);
  context(0xa0b5, 0x3f800000);
  const uint32_t viewport[]{0x41000000, 0x41000000, 0xc1000000,
                            0x41000000, 0x3f800000, 0};
  for (uint32_t i = 0; i < 6; ++i)
    context(0xa10f + i, viewport[i]);
  context(0xa318, uint32_t(uintptr_t(target) >> 8));
  context(0xa319, 7);
  context(0xa31a, 15);
  context(0xa31b, 0);
  context(0xa31c, 0x8028);
  context(0xa31d, 8);
  const uint32_t blend = blend_case ? blend_case - 1 : 0;
  const uint32_t source_factor = blend & 1;
  const uint32_t destination_factor = (blend >> 1) & 1;
  const uint32_t operation = (blend >> 2) ? 3 : 2;
  context(0xa1e0, blend_case && blend_case <= 8 ? source_factor | (operation << 5) |
                                 (destination_factor << 8) | (1u << 30)
                           : 0);
  if (blend_case == 9 || blend_case == 10) {
    context(0xa108, 0x3f000000);
    context(0xa1e0, 19 | (1u << 30));
    if (blend_case == 10)
      context(0xa31c, 0x18028);
  }
  if (blend_case == 12) {
    context(0xa010, 0);
    context(0xa200, 8);
    context(0xa0b4, 0x3f400000);
    context(0xa0b5, 0x3f800000);
  }
  if (blend_case >= 13) {
    context(0xa191, 1 << 10);
    context(0xa1b6, 1);
    context(0xa205, blend_case == 14 ? 1u << 19 : 0u);
  }
  commands.Packet(0x79, {0x242, blend_case == 11 ? 0u : blend_case >= 13 ? 6u : 4u});
  commands.Packet(0x2f, {1});
  commands.Packet(0x2d, {blend_case >= 13 ? 4u : 3u, 2});
  auto *fence = reinterpret_cast<volatile uint32_t *>(bytes + 0x20100);
  *fence = 0;
  const auto completion = uintptr_t(fence);
  commands.Packet(0x47, {20u | (5u << 8), uint32_t(completion),
                         uint32_t(completion >> 32) | (1u << 29), 2, 0});
  void *addresses[]{words};
  uint32_t sizes[]{uint32_t((commands.current - commands.begin) * 4)};
  const int rc =
      sceGnmSubmitCommandBuffers(1, addresses, sizes, nullptr, nullptr);
  bool timed_out{};
  if (rc >= 0)
    for (uint32_t wait = 0; *fence != 2; ++wait) {
      if (wait == 10000) {
        timed_out = true;
        std::printf(
            "STAGE_SHARING_FENCE TIMEOUT; retaining submitted memory\n");
      }
      sceKernelUsleep(1000);
    }
  bool pass = rc >= 0 && !timed_out;
  uint32_t colored{};
  for (uint32_t y = 0; y < 32; ++y)
    for (uint32_t x = 0; x < 64; ++x) {
      const bool written = blend_case != 11 && blend_case < 13 &&
                           ((y == 0 && x < 3) || (y >= 16 && x < 16));
      pass &= source[y * 64 + x] == (written ? 0x3f000000 : 0xdeadbeef);
      if (y < 16) {
        uint32_t blended = 0xff000080;
        if (blend_case && blend_case <= 8) {
          const uint32_t source_color = source_factor ? 0xff000080 : 0;
          const uint32_t destination_color = destination_factor ? 0xdeadbeef : 0;
          blended = 0;
          for (uint32_t channel = 0; channel < 4; ++channel) {
            const uint32_t src = (source_color >> (channel * 8)) & 255;
            const uint32_t dst = (destination_color >> (channel * 8)) & 255;
            const uint32_t value = operation == 2 ? (src < dst ? src : dst)
                                                   : (src > dst ? src : dst);
            blended |= value << (channel * 8);
          }
        }
        if (blend_case == 9)
          blended = 0x80000040;
        if (blend_case == 11)
          blended = 0xdeadbeef;
        if (blend_case >= 13) {
          const uint32_t value = blend_case == 13 ? (x < y ? 0 : 64)
                                                  : (x < y ? 128 : 191);
          blended = 0xff000000 | value * 0x010101;
          if (x == y && x < 16)
            continue;
        }
        const uint32_t expected = x < 16 ? blended : 0xdeadbeef;
        pass &= target[y * 64 + x] == expected;
        colored += x < 16 && target[y * 64 + x] == expected;
      }
    }
  std::printf("STAGE_SHARING_VS_PS blend_case=%u virtual_alias=%u colored=%u padding=%s %s "
              "submit=%08x\n",
              blend_case, variant, colored, pass ? "PASS" : "FAIL", pass ? "PASS" : "FAIL",
              uint32_t(rc));
  return pass;
}
bool RunMixedCmask(uint8_t *bytes, uint32_t format = 10, uint32_t number = 0,
                   uint32_t swap = 0, uint32_t word0 = 0xff000080,
                   uint32_t word1 = 0, uint32_t pixel_bytes = 4,
                   const uint8_t *expected_pixel = nullptr) {
  auto *target = reinterpret_cast<volatile uint8_t *>(bytes + 0x140000);
  auto *metadata = bytes + 0x150000;
  for (uint32_t i = 0; i < 64 * 16 * pixel_bytes; ++i)
    target[i] = 0xee;
  std::memset(metadata, 0xff, 2048);
  metadata[0] = 0xf0;
  const auto vop1 = [](uint32_t op, uint32_t dst, uint32_t src) {
    return 0x7e000000u | (dst << 17) | (op << 9) | src;
  };
  const auto vop2 = [](uint32_t op, uint32_t dst, uint32_t src0,
                       uint32_t src1) {
    return (op << 25) | (dst << 17) | (src1 << 9) | src0;
  };
  const uint32_t vertex[]{
      vop2(26, 1, 129, 0), vop2(27, 1, 130, 1), vop2(27, 2, 130, 0),
      vop1(6, 1, 257),     vop1(6, 2, 258),     vop2(8, 1, 244, 1),
      vop2(8, 2, 244, 2),  vop2(3, 1, 243, 1),  vop2(3, 2, 243, 2),
      vop1(1, 3, 128),     vop1(1, 4, 242),     0xf80008cf,
      0x04030201,          0xbf810000};
  const uint32_t pixel[]{vop1(1, 0, 242), vop1(1, 1, 128), 0xf800180f,
                         0x00010100, 0xbf810000};
  auto *vs = bytes + 0x160000;
  auto *ps = bytes + 0x161000;
  std::memcpy(vs, vertex, sizeof(vertex));
  std::memcpy(ps, pixel, sizeof(pixel));
  const ShaderBinaryInfo vs_info{{'O', 'r', 'b', 'S', 'h', 'd', 'r'},
                                 1,
                                 uint32_t(sizeof(vertex)) << 8,
                                 0,
                                 0x636d61736b767300,
                                 0};
  const ShaderBinaryInfo ps_info{{'O', 'r', 'b', 'S', 'h', 'd', 'r'},
                                 1,
                                 uint32_t(sizeof(pixel)) << 8,
                                 0,
                                 0x636d61736b707300,
                                 0};
  std::memcpy(vs + 0x80, &vs_info, sizeof(vs_info));
  std::memcpy(ps + 0x80, &ps_info, sizeof(ps_info));
  uint32_t vs_user[8]{}, ps_user[8]{};
  auto *words = reinterpret_cast<uint32_t *>(bytes + 0x30000);
  const int initialized = sceGnmDrawInitDefaultHardwareState350(words, 0x1000);
  if (initialized <= 0 || initialized > 0x1000)
    return false;
  Commands commands{words, words + initialized};
  const auto shader = [&](uint32_t reg, uint32_t value) {
    commands.Packet(0x76, {reg - 0x2c00, value});
  };
  const auto context = [&](uint32_t reg, uint32_t value) {
    commands.Packet(0x69, {reg - 0xa000, value});
  };
  shader(0x2c48, uint32_t(uintptr_t(vs) >> 8));
  shader(0x2c49, uint32_t(uintptr_t(vs) >> 40));
  shader(0x2c4a, 3 | (1 << 6));
  shader(0x2c4b, 8 << 1);
  shader(0x2c08, uint32_t(uintptr_t(ps) >> 8));
  shader(0x2c09, uint32_t(uintptr_t(ps) >> 40));
  shader(0x2c0a, 3 | (1 << 6));
  shader(0x2c0b, 8 << 1);
  for (uint32_t i = 0; i < 8; ++i) {
    shader(0x2c4c + i, vs_user[i]);
    shader(0x2c0c + i, ps_user[i]);
  }
  context(0xa2d5, 0);
  context(0xa290, 0);
  context(0xa1b1, 0);
  context(0xa1b3, 0x300);
  context(0xa1b4, 0x300);
  context(0xa1b6, 0);
  context(0xa1c3, 4);
  context(0xa1c4, 0);
  context(0xa1c5, 9);
  context(0xa200, 0);
  context(0xa202, 0x00cc0020);
  context(0xa203, 0xe00);
  context(0xa204, 1 << 19);
  context(0xa205, 0);
  context(0xa206, 0x3f);
  context(0xa207, 0);
  context(0xa292, 2);
  context(0xa2f8, 0);
  context(0xa08e, 15);
  context(0xa08f, 15);
  for (uint32_t reg : {0xa00c, 0xa081, 0xa090, 0xa094}) {
    context(reg, reg == 0xa00c ? 0 : 0x80000000);
    context(reg + 1, (16 << 16) | 16);
  }
  context(0xa0b4, 0);
  context(0xa0b5, 0x3f800000);
  const uint32_t viewport[]{0x41000000, 0x41000000, 0xc1000000,
                            0x41000000, 0x3f800000, 0};
  for (uint32_t i = 0; i < 6; ++i)
    context(0xa10f + i, viewport[i]);
  context(0xa318, uint32_t(uintptr_t(target) >> 8));
  context(0xa319, 7);
  context(0xa31a, 15);
  context(0xa31b, 0);
  context(0xa31c, 0xa000 | (format << 2) | (number << 8) | (swap << 11));
  context(0xa31d, 8);
  context(0xa31f, uint32_t(uintptr_t(metadata) >> 8));
  context(0xa320, 15);
  context(0xa323, word0);
  context(0xa324, word1);
  context(0xa1e0, 0);
  commands.Packet(0x79, {0x242, 4});
  commands.Packet(0x2f, {1});
  commands.Packet(0x2d, {3, 2});
  auto *fence = reinterpret_cast<volatile uint32_t *>(bytes + 0x20100);
  *fence = 0;
  const auto completion = uintptr_t(fence);
  commands.Packet(0x47, {20u | (5u << 8), uint32_t(completion),
                         uint32_t(completion >> 32) | (1u << 29), 2, 0});
  void *addresses[]{words};
  uint32_t sizes[]{uint32_t((commands.current - commands.begin) * 4)};
  const int rc =
      sceGnmSubmitCommandBuffers(1, addresses, sizes, nullptr, nullptr);
  bool timed_out{};
  if (rc >= 0)
    for (uint32_t wait = 0; *fence != 2; ++wait) {
      if (wait == 10000) {
        timed_out = true;
        std::printf("CMASK_FENCE TIMEOUT; retaining submitted memory\n");
      }
      sceKernelUsleep(1000);
    }
  bool pass = rc >= 0 && !timed_out;
  uint32_t cleared{};
  const uint8_t original[]{0x80, 0, 0, 0xff};
  if (!expected_pixel)
    expected_pixel = original;
  for (uint32_t y = 0; y < 16; ++y)
    for (uint32_t x = 0; x < 64; ++x) {
      const bool clear = x < 8 && y < 8;
      bool match = true;
      for (uint32_t component = 0; component < pixel_bytes; ++component)
        match &= target[(y * 64 + x) * pixel_bytes + component] ==
                 (clear ? expected_pixel[component] : 0xee);
      pass &= match;
      cleared += clear && match;
    }
  pass &= metadata[0] == 0xff;
  for (uint32_t offset = 1; offset < 2048; ++offset)
    pass &= metadata[offset] == 0xff;
  std::printf("CMASK_MIXED format=%u number=%u swap=%u clear_pixels=%u "
              "metadata=%02x padding=%s %s submit=%08x\n",
              format, number, swap, cleared, metadata[0],
              pass ? "PASS" : "FAIL", pass ? "PASS" : "FAIL", uint32_t(rc));
  return pass;
}

} // namespace

bool RunTypedClears(uint8_t *bytes) {
  struct ClearCase {
    uint32_t format, number, swap, word0, word1, bytes;
    std::array<uint8_t, 16> expected;
  };
  const ClearCase cases[]{
      {1, 5, 0, 0xffffff80, 0, 1, {0x80}},
      {2, 5, 0, 0xffff8001, 0, 2, {1, 0x80}},
      {10, 5, 0, 0x807f01ff, 0, 4, {0xff, 1, 0x7f, 0x80}},
      {5, 5, 0, 0x80017fff, 0, 4, {0xff, 0x7f, 1, 0x80}},
      {2, 7, 0, 0x7c00, 0, 2, {0, 0x7c}},
      {12,
       7,
       0,
       0x7e017c00,
       0xfe01fc00,
       8,
       {0, 0x7c, 1, 0x7e, 0, 0xfc, 1, 0xfe}},
      {4, 7, 0, 0x80000000, 0, 4, {0, 0, 0, 0x80}},
      {14,
       4,
       0,
       0xffffffff,
       0x80000001,
       16,
       {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        1, 0, 0, 0x80}},
      {10, 0, 1, 0x80402010, 0, 4, {0x40, 0x20, 0x10, 0x80}},
      {16, 0, 0, 0xf800, 0, 2, {0, 0xf8}},
      {17, 0, 0, 0x801f, 0, 2, {0x1f, 0x80}},
      {19, 0, 0, 0x4321, 0, 2, {0x21, 0x43}},
  };
  bool pass = true;
  for (const auto &value : cases)
    pass &= RunMixedCmask(bytes, value.format, value.number, value.swap,
                          value.word0, value.word1, value.bytes,
                          value.expected.data());
  std::printf("TYPED_CLEARS cases=%zu %s\n", std::size(cases),
              pass ? "PASS" : "FAIL");
  return pass;
}

int main() {
  off_t physical{};
  void *memory{};
  void *alias{};
  int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
                                         AllocationSize, 0x10000, 3, &physical);
  if (rc < 0) {
    std::printf("ALLOCATE error=%08x\n", uint32_t(rc));
    return 1;
  }
  rc = sceKernelMapDirectMemory(&memory, AllocationSize, 0x33, 0, physical,
                                0x10000);
  if (rc < 0) {
    sceKernelReleaseDirectMemory(physical, AllocationSize);
    return 2;
  }
  rc = sceKernelMapDirectMemory(&alias, AllocationSize, 0x33, 0, physical,
                                0x10000);
  if (rc < 0) {
    sceKernelMunmap(memory, AllocationSize);
    sceKernelReleaseDirectMemory(physical, AllocationSize);
    return 3;
  }
  std::memset(memory, 0, AllocationSize);
  auto *bytes = static_cast<uint8_t *>(memory);
  auto *alias_bytes = static_cast<uint8_t *>(alias);
  auto *output = reinterpret_cast<volatile uint32_t *>(bytes + 0x20000);
  auto *alias_output =
      reinterpret_cast<volatile uint32_t *>(alias_bytes + 0x20000);
  *output = 0x2468ace0;
  const bool cpu_alias = *alias_output == 0x2468ace0;
  std::printf("CPU_ALIAS %s\n", cpu_alias ? "PASS" : "FAIL");

  auto *shader = reinterpret_cast<uint32_t *>(bytes + 0x10000);
  const uint32_t code[]{0x7e0002ff, 0x11223344, 0x7e020280, 0xe0701000,
                        0x80000001, 0xbf8c0f70, 0xbf810000};
  std::memcpy(shader, code, sizeof(code));
  const ShaderBinaryInfo metadata{{'O', 'r', 'b', 'S', 'h', 'd', 'r'},
                                  1,
                                  uint32_t(sizeof(code)) << 8,
                                  0,
                                  0x11223344,
                                  0};
  std::memcpy(bytes + 0x10020, &metadata, sizeof(metadata));

  auto *command_words = reinterpret_cast<uint32_t *>(bytes + 0x30000);
  const int initialized =
      sceGnmDrawInitDefaultHardwareState350(command_words, 0x1000);
  if (initialized <= 0 || initialized > 0x1000) {
    std::printf("GNM_INIT error=%08x\n", uint32_t(initialized));
    return 4;
  }
  Commands commands{command_words, command_words + initialized};
  commands.Register(0x2e04, 0);
  commands.Register(0x2e05, 0);
  commands.Register(0x2e06, 0);
  commands.Register(0x2e07, 1);
  commands.Register(0x2e08, 1);
  commands.Register(0x2e09, 1);
  commands.Register(0x2e0c, uint32_t(uintptr_t(shader) >> 8));
  commands.Register(0x2e0d, uint32_t(uintptr_t(shader) >> 40));
  commands.Register(0x2e12, 0);
  commands.Register(0x2e13, 4 << 1);
  commands.Register(0x2e15, 0x170);
  commands.Register(0x2e16, 0xffffffff);
  commands.Register(0x2e17, 0xffffffff);
  const auto destination = uintptr_t(alias_output);
  commands.Packet(0x76,
                  {0x240, uint32_t(destination),
                   uint32_t(destination >> 32) & 0xff, 16,
                   4u | (5u << 3) | (6u << 6) | (7u << 9) | (3u << 27)},
                  true);
  commands.Packet(0x15, {1, 1, 1, 0x21}, true);
  auto *fence = reinterpret_cast<volatile uint32_t *>(bytes + 0x20100);
  const auto fence_address = uintptr_t(fence);
  commands.Packet(0x48,
                  {47u | (6u << 8), uint32_t(fence_address),
                   uint32_t(fence_address >> 32) | (2u << 29), 1},
                  true);
  commands.Packet(0x47, {20u | (5u << 8), uint32_t(fence_address),
                         uint32_t(fence_address >> 32) | (1u << 29), 2, 0});
  void *addresses[]{command_words};
  uint32_t sizes[]{
      uint32_t((commands.current - commands.begin) * sizeof(uint32_t))};
  NeighborWrites neighbor_writes{alias_output};
  OrbisPthread neighbor_thread{};
  const bool writer_started =
      scePthreadCreate(&neighbor_thread, nullptr, WriteNeighbors,
                       &neighbor_writes, "gpu-neighbor-writes") == 0;
  if (writer_started)
    while (!neighbor_writes.started.load(std::memory_order_acquire))
      sceKernelUsleep(100);
  rc = sceGnmSubmitCommandBuffers(1, addresses, sizes, nullptr, nullptr);
  if (rc < 0) {
    neighbor_writes.stop.store(true, std::memory_order_release);
    if (writer_started)
      scePthreadJoin(neighbor_thread, nullptr);
    std::printf("SUBMIT error=%08x\n", uint32_t(rc));
    return 5;
  }
  bool timed_out{};
  for (uint32_t wait = 0; *fence != 2; ++wait) {
    if (wait == 10000) {
      timed_out = true;
      std::printf("GPU_FENCE TIMEOUT; keeping submitted memory alive\n");
    }
    sceKernelUsleep(1000);
  }
  const bool gpu_alias = *output == 0x11223344 && *alias_output == 0x11223344;
  neighbor_writes.stop.store(true, std::memory_order_release);
  if (writer_started)
    scePthreadJoin(neighbor_thread, nullptr);
  const bool cpu_neighbors =
      writer_started && output[1] == 0xaabbccdd && output[2] == 0x55667788;
  std::printf("CPU_GPU_NEIGHBORS %s words=%08x,%08x\n",
              cpu_neighbors ? "PASS" : "FAIL", output[1], output[2]);
  std::printf("GPU_ALIAS %s original=%08x alias=%08x fence=%u\n",
              gpu_alias ? "PASS" : "FAIL", *output, *alias_output, *fence);
  const bool segmented_alias =
      RunSegmentedAliases(bytes, alias_bytes, physical);

  const bool nonlinear_alias =
      RunNonlinearAliases(bytes, alias_bytes, physical, false);
  const bool repeated_alias =
      RunNonlinearAliases(bytes, alias_bytes, physical, true);
  const bool image_feedback = RunImageFeedback(bytes);
  const bool image_feedback_one = RunImageFeedback(bytes, true);
  const bool image_feedback_alias = RunImageFeedback(bytes, false, alias_bytes);
  const bool image_feedback_alias_one =
      RunImageFeedback(bytes, true, alias_bytes);
  const bool image_residency = RunPersistentImageTransitions(bytes, bytes);
  const bool image_residency_alias =
      RunPersistentImageTransitions(bytes, alias_bytes);
  const bool image_buffer_atomics = RunImageBufferAtomics(bytes);
  const bool image_buffer_writes = RunImageBufferWrites(bytes);
  const bool image_buffer_r8 = RunImageBufferWrites(bytes, 1);
  const bool image_buffer_r16 = RunImageBufferWrites(bytes, 2);
  const bool image_buffer_alt = RunImageBufferWrites(bytes, 4, true);
  const bool image_buffer_alt_bank = RunImageBufferWrites(bytes, 4, true, 1);
  const bool cmask_mixed = RunMixedCmask(bytes);
  const bool typed_clears = RunTypedClears(bytes);
  const bool command_dependencies = RunCommandDependencies(bytes, alias_bytes);
  const bool stage_sharing = RunStageSharing(bytes, bytes);
  const bool stage_sharing_alias = RunStageSharing(bytes, alias_bytes);
  bool minmax_blend = true;
  for (uint32_t blend_case = 1; blend_case <= 14; ++blend_case)
    minmax_blend &= RunStageSharing(bytes, bytes, blend_case);

  auto *pixels = reinterpret_cast<uint32_t *>(bytes + 0x40000);
  for (uint32_t y = 0; y < Height; ++y)
    for (uint32_t x = 0; x < Width; ++x)
      pixels[y * Width + x] = 0xff000000 | ((x * 255 / (Width - 1)) << 16) |
                              ((y * 255 / (Height - 1)) << 8) |
                              ((x / 32 ^ y / 32) & 1 ? 255 : 0);
  std::printf("VIDEOOUT_CPU_HASH %08x\n", Hash(pixels, Width * Height));
  const int video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN,
                                    ORBIS_VIDEO_OUT_BUS_MAIN, 0, nullptr);
  OrbisVideoOutBufferAttribute attribute{};
  sceVideoOutSetBufferAttribute(&attribute, 0x80000000, 1, 0, Width, Height,
                                Width);
  void *buffers[]{alias_bytes + 0x40000};
  rc = video < 0 ? video
                 : sceVideoOutRegisterBuffers(video, 0, buffers, 1, &attribute);
  if (rc >= 0)
    for (int frame = 0; frame < 300; ++frame) {
      rc = sceVideoOutSubmitFlip(video, 0, ORBIS_VIDEO_OUT_FLIP_VSYNC, frame);
      if (rc < 0)
        break;
      while (sceVideoOutIsFlipPending(video) > 0)
        sceKernelUsleep(1000);
    }
  std::printf("VIDEOOUT_API %s error=%08x; visual validation required\n",
              rc >= 0 ? "PASS" : "FAIL", uint32_t(rc));
  if (video >= 0) {
    sceVideoOutUnregisterBuffers(video, 0);
    sceVideoOutClose(video);
  }
  sceKernelMunmap(alias, AllocationSize);
  sceKernelMunmap(memory, AllocationSize);
  sceKernelReleaseDirectMemory(physical, AllocationSize);
  return cpu_alias && gpu_alias && cpu_neighbors && segmented_alias &&
                 nonlinear_alias && repeated_alias && image_feedback &&
                 image_feedback_one && image_feedback_alias &&
                 image_feedback_alias_one && image_buffer_atomics &&
                 image_buffer_writes && image_buffer_r8 && image_buffer_r16 &&
                 image_buffer_alt && image_buffer_alt_bank && image_residency &&
                 image_residency_alias && cmask_mixed && typed_clears &&
                 stage_sharing && stage_sharing_alias && command_dependencies && minmax_blend && !timed_out && rc >= 0
             ? 0
             : 6;
}
