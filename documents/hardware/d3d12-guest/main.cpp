// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
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
  if (alternate) {
    image[1] = 127 | (uint64_t(127) << 14) | (uint64_t(4) << 32) |
               (uint64_t(5) << 35) | (uint64_t(6) << 38) | (uint64_t(7) << 41) |
               (uint64_t(10) << 52) | (uint64_t(9) << 60);
    image[2] = uint64_t(127) << 13;
    image[3] = uint64_t(1) << 24;
  }
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
bool RunImageFeedback(uint8_t *bytes, bool integer_constant = false) {
  auto *source = reinterpret_cast<volatile uint32_t *>(bytes + 0x40000);
  source[0] = source[1] = 0xdeadbeef;
  auto *shader = reinterpret_cast<uint32_t *>(bytes + 0x11000);
  const uint32_t code[]{0x7e000280, 0x7e020280, 0x7e0802ff, 0x12345678,
                        0xf0202100, 0x00000400, 0xbf8c0f70, 0xf0002100,
                        0x00020400, 0xbf8c0f70, 0x7e000281, 0xf0202100,
                        0x00000400, 0xbf8c0f70, 0xbf810000};
  std::memcpy(shader, code, sizeof(code));
  const ShaderBinaryInfo metadata{{'O', 'r', 'b', 'S', 'h', 'd', 'r'},
                                  1, uint32_t(sizeof(code)) << 8, 0,
                                  0x666565646261636b, 0};
  std::memcpy(bytes + 0x11040, &metadata, sizeof(metadata));
  uint64_t image[4]{};
  image[0] = (uintptr_t(source) >> 8) | (uint64_t(4) << 52) |
             (uint64_t(4) << 58);
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
  std::memcpy(user + 8, image, sizeof(image));
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
  const uint32_t expected = integer_constant ? 1 : 0x12345678;
  const bool pass = rc >= 0 && !timed_out && source[0] == 0x12345678 &&
                    source[1] == expected;
  std::printf("IMAGE_LOAD_FEEDBACK %s constant=%u writer=%08x reader=%08x "
              "expected=%08x submit=%08x\n",
              pass ? "PASS" : "FAIL", uint32_t(integer_constant), source[0], source[1],
              expected, uint32_t(rc));
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
  uint32_t code[]{
      0x7e000280, 0x7e020280, 0x7e040280, 0x7e060280, 0x7e0802ff,
      0x12345678, 0xf0202100, 0x00000400, 0xbf8c0f70, 0xe0305000,
      0x80020502, 0xbf8c0f70, 0xe0705000, 0x80030503, 0x7e0802ff,
      0x87654321, 0xe0705000, 0x80020402, 0xbf8c0f70, 0xf0002100,
      0x00000400, 0xbf8c0f70, 0xe0705004, 0x80030403, 0xbf810000};
  const uint32_t bits = format == 1 ? 8 : format == 4 ? 32 : 16;
  const uint32_t offset = alternate ? bank << 12 : bits == 8 ? 3 : bits == 16 ? 2 : 0;
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
                                  1, uint32_t(sizeof(code)) << 8, 0,
                                  0x696d676275666665, 0};
  std::memcpy(bytes + 0x12080, &metadata, sizeof(metadata));
  uint64_t image[4]{};
  image[0] = ((uintptr_t(source) | (uintptr_t(bank) << 12)) >> 8) | (uint64_t(format) << 52) |
             (uint64_t(4) << 58);
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
  const uint32_t expected_word = (0xdeadbeef & ~(mask << bit_offset)) |
                                 (last << bit_offset);
  bool neighbors = true;
  for (uint32_t i = 0; i < source_words; ++i)
    neighbors &= source[i] == (i == offset / 4 ? expected_word : 0xdeadbeef);
  const bool pass = rc >= 0 && !timed_out && output[0] == first &&
                    output[1] == last && neighbors;
  std::printf("IMAGE_BUFFER_WRITES format=%u bits=%u alt=%u bank=%u %s image_to_buffer=%08x buffer_to_image=%08x "
              "source=%08x submit=%08x\n", format, bits, uint32_t(alternate), bank, pass ? "PASS" : "FAIL", output[0],
              output[1], source[offset / 4], uint32_t(rc));
  return pass;
}
} // namespace

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

  const bool image_feedback = RunImageFeedback(bytes);
  const bool image_feedback_one = RunImageFeedback(bytes, true);
  const bool image_buffer_writes = RunImageBufferWrites(bytes);
  const bool image_buffer_r8 = RunImageBufferWrites(bytes, 1);
  const bool image_buffer_r16 = RunImageBufferWrites(bytes, 2);
  const bool image_buffer_alt = RunImageBufferWrites(bytes, 4, true);
  const bool image_buffer_alt_bank = RunImageBufferWrites(bytes, 4, true, 1);

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
  return cpu_alias && gpu_alias && cpu_neighbors && segmented_alias && image_feedback &&
                 image_feedback_one && image_buffer_writes && image_buffer_r8 && image_buffer_r16 && image_buffer_alt && image_buffer_alt_bank &&
                 !timed_out && rc >= 0
             ? 0
             : 6;
}
