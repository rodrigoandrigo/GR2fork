// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <orbis/GnmDriver.h>
#include <orbis/libkernel.h>
namespace {
struct BinaryInfo {
  char signature[7];
  uint8_t version;
  uint32_t length, usage;
  uint64_t hash;
  uint32_t crc;
};
struct Commands {
  uint32_t *begin;
  uint32_t *current;
  void Packet(uint32_t opcode, std::initializer_list<uint32_t> body,
              bool compute = false) {
    *current++ = 0xc0000000 | ((uint32_t(body.size()) - 1) << 16) |
                 (opcode << 8) | (compute ? 2 : 0);
    for (auto word : body)
      *current++ = word;
  }
  void Register(uint32_t reg, uint32_t value) {
    Packet(0x76, {reg - 0x2c00, value}, true);
  }
};
bool Run(uint8_t *bytes, const uint32_t *code, uint32_t size, uint32_t mode) {
  auto *shader = bytes + 0x10000;
  std::memcpy(shader, code, size);
  const BinaryInfo info{{'O', 'r', 'b', 'S', 'h', 'd', 'r'}, 1, size << 8, 0,
                        0xf1800000u + mode + code[4],        0};
  std::memcpy(shader + 0x800, &info, sizeof(info));
  auto *words = reinterpret_cast<uint32_t *>(bytes);
  int initialized = sceGnmDrawInitDefaultHardwareState350(words, 0x1000);
  if (initialized <= 0 || initialized > 0x1000)
    return false;
  Commands commands{words, words + initialized};
  for (uint32_t reg = 0x2e04; reg <= 0x2e06; ++reg)
    commands.Register(reg, 0);
  for (uint32_t reg = 0x2e07; reg <= 0x2e09; ++reg)
    commands.Register(reg, 1);
  auto address = uintptr_t(shader);
  commands.Register(0x2e0c, uint32_t(address >> 8));
  commands.Register(0x2e0d, uint32_t(address >> 40));
  commands.Register(0x2e12, 3 | (mode << 16) | (3 << 18) | (1 << 23));
  commands.Register(0x2e13, 4 << 1);
  commands.Register(0x2e15, 0x170);
  commands.Register(0x2e16, ~0u);
  commands.Register(0x2e17, ~0u);
  address = uintptr_t(bytes + 0x40000);
  commands.Packet(0x76,
                  {0x240, uint32_t(address), uint32_t(address >> 32), 64,
                   4 | (5 << 3) | (6 << 6) | (7 << 9)},
                  true);
  commands.Packet(0x15, {1, 1, 1, 0x21}, true);
  auto *fence = reinterpret_cast<volatile uint32_t *>(bytes + 0x30000);
  *fence = 0;
  auto completion = uintptr_t(fence);
  commands.Packet(0x48,
                  {47u | (6u << 8), uint32_t(completion),
                   uint32_t(completion >> 32) | (2u << 29), 1},
                  true);
  commands.Packet(0x47, {20u | (5u << 8), uint32_t(completion),
                         uint32_t(completion >> 32) | (1u << 29), 2, 0});
  void *addresses[]{words};
  uint32_t sizes[]{uint32_t((commands.current - words) * 4)};
  if (sceGnmSubmitCommandBuffers(1, addresses, sizes, nullptr, nullptr) < 0)
    return false;
  while (*fence != 2)
    sceKernelUsleep(1000);
  return true;
}
} // namespace
int main() {
  constexpr size_t size = 1024 * 1024;
  off_t physical{};
  void *memory{};
  if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size,
                                    0x10000, 3, &physical) < 0)
    return 1;
  if (sceKernelMapDirectMemory(&memory, size, 0x33, 0, physical, 0x10000) < 0) {
    sceKernelReleaseDirectMemory(physical, size);
    return 1;
  }
  auto *bytes = static_cast<uint8_t *>(memory);
  std::memset(bytes, 0, size);
  bool pass = true;
  const uint32_t fp64[]{0x7e000280, 0xe0381000,         0x80000100, 0xbf8c0f70,
                        0xd2c80005, 257u | (259u << 9), 0x7e100280, 0xe0741010,
                        0x80000508, 0xbf810000};
  for (auto pair : {std::array<double, 2>{1.0, std::ldexp(1.0, -40)},
                    std::array<double, 2>{std::ldexp(1.0, 40), 1.0}}) {
    std::memcpy(bytes + 0x40000, pair.data(), 16);
    if (!Run(bytes, fp64, sizeof(fp64), 0))
      return 2;
    double result{};
    std::memcpy(&result, bytes + 0x40010, 8);
    auto i = pair.begin();
    double expected = i[0] + i[1];
    bool match =
        std::bit_cast<uint64_t>(result) == std::bit_cast<uint64_t>(expected);
    pass &= match;
    std::printf("FP64 result=%.17g expected=%.17g %s\n", result, expected,
                match ? "PASS" : "FAIL");
  }
  const uint32_t fp32[]{0x7e000280,
                        0xe0341000,
                        0x80000100,
                        0xbf8c0f70,
                        (8u << 25) | (5u << 17) | (2u << 9) | 257u,
                        0x7e100280,
                        0xe0701010,
                        0x80000508,
                        0xbf810000};
  for (uint32_t preserve : {0u, 3u})
    for (uint32_t output : {0u, 1u}) {
      uint32_t operands[]{output ? 0x00800000u : 0x00400000u,
                          output ? 0x3f000000u : 0x40000000u};
      std::memcpy(bytes + 0x40000, operands, 8);
      if (!Run(bytes, fp32, sizeof(fp32), preserve))
        return 2;
      uint32_t result{};
      std::memcpy(&result, bytes + 0x40010, 4);
      uint32_t expected = preserve ? (output ? 0x00400000u : 0x00800000u) : 0;
      bool match = result == expected;
      pass &= match;
      std::printf("DENORM mode=%u output=%u value=%08x expected=%08x %s\n",
                  preserve, output, result, expected, match ? "PASS" : "FAIL");
    }
  sceKernelMunmap(memory, size);
  sceKernelReleaseDirectMemory(physical, size);
  return pass ? 0 : 3;
}
