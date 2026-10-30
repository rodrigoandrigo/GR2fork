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
bool Run(uint8_t *bytes, const uint32_t *code, uint32_t size, uint32_t mode,
         const std::array<uint32_t, 5> &extra = {}) {
  auto *shader = bytes + 0x10000;
  std::memcpy(shader, code, size);
  uint64_t hash = 1469598103934665603ull;
  for (uint32_t i = 0; i < size / 4; ++i)
    hash = (hash ^ code[i]) * 1099511628211ull;
  const BinaryInfo info{
      {'O', 'r', 'b', 'S', 'h', 'd', 'r'}, 1, size << 8, 0, hash, 0};
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
  commands.Register(0x2e12,
                    3 | (1 << 6) | (mode << 16) | (3 << 18) | (1 << 23));
  commands.Register(0x2e13, 12 << 1);
  commands.Register(0x2e15, 0x170);
  commands.Register(0x2e16, ~0u);
  commands.Register(0x2e17, ~0u);
  address = uintptr_t(bytes + 0x40000);
  commands.Packet(0x76,
                  {0x240, uint32_t(address), uint32_t(address >> 32), 64,
                   4 | (5 << 3) | (6 << 6) | (7 << 9)},
                  true);
  commands.Packet(
      0x76, {0x244, extra[0], extra[1], extra[2], extra[3], extra[4]}, true);
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
  uint32_t checked{};
  const auto vop1 = [](uint32_t op, uint32_t dst, uint32_t src) {
    return 0x7e000000u | (dst << 17) | (op << 9) | src;
  };
  for (uint32_t source = 0; source < 7; ++source)
    for (uint32_t dest = 0; dest < 7; ++dest)
      for (uint32_t unused = 0; unused < 3; ++unused)
        for (uint32_t sext = 0; sext < 2; ++sext) {
          const uint32_t raw = 0x8192a3b4, previous = 0x5aa55aa5;
          uint32_t source_offset = source < 4   ? source * 8
                                   : source < 6 ? (source - 4) * 16
                                                : 0,
                   source_width = source < 4   ? 8
                                  : source < 6 ? 16
                                               : 32;
          uint32_t offset = dest < 4   ? dest * 8
                            : dest < 6 ? (dest - 4) * 16
                                       : 0,
                   width = dest < 4   ? 8
                           : dest < 6 ? 16
                                      : 32;
          uint32_t source_mask =
                       source_width == 32 ? ~0u : (1u << source_width) - 1,
                   mask = width == 32 ? ~0u : (1u << width) - 1;
          uint32_t expected = (raw >> source_offset) & source_mask;
          if (sext && source_width < 32 &&
              (expected & (1u << (source_width - 1))))
            expected |= ~source_mask;
          expected &= mask;
          if (unused == 1 && width < 32 && (expected & (1u << (width - 1))))
            expected |= ~mask;
          expected <<= offset;
          if (unused == 2)
            expected |= previous & ~(mask << offset);
          uint32_t extension = 1 | (dest << 8) | (unused << 11) |
                               (source << 16) | (sext << 19) | (6u << 24);
          uint32_t code[]{
              vop1(1, 0, 128), 0xe0301000,
              0x80000100,      0xbf8c0f70,
              vop1(1, 2, 128), vop1(1, 5, 255),
              previous,        (28u << 25) | (5u << 17) | (2u << 9) | 249u,
              extension,       vop1(1, 8, 128),
              0xe0701010,      0x80000508,
              0xbf810000};
          std::memcpy(bytes + 0x40000, &raw, 4);
          if (!Run(bytes, code, sizeof(code), 0))
            return 2;
          uint32_t value{};
          std::memcpy(&value, bytes + 0x40010, 4);
          bool match = value == expected;
          pass &= match;
          ++checked;
          if (!match)
            std::printf("SDWA source=%u dest=%u unused=%u sext=%u value=%08x "
                        "expected=%08x FAIL\n",
                        source, dest, unused, sext, value, expected);
        }
  std::printf("SDWA cases=%u %s\n", checked, pass ? "PASS" : "FAIL");
  for (uint32_t operation = 0; operation < 6; ++operation)
    for (uint32_t left : {0u, 0x80u, 0x8000u, ~0u})
      for (uint32_t right : {0u, ~0u})
        for (uint32_t flag : {0u, 1u}) {
          uint32_t instruction = operation < 3 ? (0x80000000u |
                                                  ((operation == 0   ? 22u
                                                    : operation == 1 ? 26u
                                                                     : 28u)
                                                   << 23) |
                                                  (6u << 16) | (5u << 8) | 4u)
                                               : (0xbe800000u | (6u << 16) |
                                                  ((operation == 3   ? 25u
                                                    : operation == 4 ? 26u
                                                                     : 5u)
                                                   << 8) |
                                                  4u);
          uint32_t code[]{0xbf068108,      0xbe860305,    instruction,
                          0x85078081,      vop1(1, 1, 6), vop1(1, 2, 7),
                          vop1(1, 8, 128), 0xe0741010,    0x80000108,
                          0xbf810000};
          if (!Run(bytes, code, sizeof(code), 0, {left, right, 0, 0, flag}))
            return 2;
          uint32_t result[2]{};
          std::memcpy(result, bytes + 0x40010, 8);
          uint32_t expected = operation == 0   ? left | ~right
                              : operation == 1 ? ~(left | right)
                              : operation == 2 ? ~(left ^ right)
                              : operation == 3 ? uint32_t(int32_t(int8_t(left)))
                              : operation == 4
                                  ? uint32_t(int32_t(int16_t(left)))
                              : flag ? left
                                     : right;
          uint32_t scc = operation < 3 ? expected != 0 : flag;
          bool match = result[0] == expected && result[1] == scc;
          pass &= match;
          std::printf("SCALAR operation=%u left=%08x right=%08x flag=%u "
                      "result=%08x scc=%u %s\n",
                      operation, left, right, flag, result[0], result[1],
                      match ? "PASS" : "FAIL");
        }

  for (uint32_t operation : {6u, 10u, 12u, 14u, 18u, 24u, 45u})
    for (uint64_t value : {uint64_t(0), uint64_t(1), uint64_t(0x80000000),
                           uint64_t(0x8000000000000000), ~uint64_t(0),
                           uint64_t(0x0123456789abcdef)})
      for (uint32_t flag : {0u, 1u}) {
        constexpr uint64_t previous = 0x13579bdf2468ace0;
        uint64_t expected{};
        bool wide = operation == 6 || operation == 10 || operation == 12 ||
                    operation == 45;
        if (operation == 6)
          expected = flag ? value : previous;
        else if (operation == 10) {
          for (uint32_t i = 0; i < 16; ++i)
            if ((value >> (i * 4)) & 15)
              expected |= uint64_t(15) << (i * 4);
        } else if (operation == 12) {
          for (uint32_t i = 0; i < 64; ++i)
            expected |= ((value >> i) & 1) << (63 - i);
        } else if (operation == 14)
          expected = std::popcount(~value);
        else if (operation == 18)
          expected = value == ~uint64_t(0) ? ~0u : std::countr_zero(~value);
        else if (operation == 24) {
          uint64_t normalized = (value >> 63) ? ~value : value;
          expected = normalized ? std::countl_zero(normalized) : ~0u;
        } else {
          for (uint32_t i = 0; i < 16; ++i)
            if ((value >> (i * 4)) & 15)
              expected |= uint64_t(1) << i;
        }
        uint32_t code[]{0xbe8a03ff,
                        uint32_t(previous),
                        0xbe8b03ff,
                        uint32_t(previous >> 32),
                        0xbf068108,
                        0xbe800000u | (10u << 16) | (operation << 8) | 4u,
                        0x850c8081,
                        vop1(1, 1, 10),
                        vop1(1, 2, wide ? 11 : 128),
                        vop1(1, 3, 12),
                        vop1(1, 8, 128),
                        0xe0781010,
                        0x80000108,
                        0xbf810000};
        if (!Run(bytes, code, sizeof(code), 0,
                 {uint32_t(value), uint32_t(value >> 32), 0, 0, flag}))
          return 2;
        uint32_t result[3]{};
        std::memcpy(result, bytes + 0x40010, sizeof(result));
        uint32_t expected_scc =
            operation == 10 || operation == 14 || operation == 45
                ? expected != 0
                : flag;
        bool match = result[0] == uint32_t(expected) &&
                     result[1] == (wide ? uint32_t(expected >> 32) : 0) &&
                     result[2] == expected_scc;
        pass &= match;
        std::printf("SCALAR64 opcode=%u source=%016llx flag=%u result=%08x%08x "
                    "scc=%u %s\n",
                    operation, static_cast<unsigned long long>(value), flag,
                    result[1], result[0], result[2], match ? "PASS" : "FAIL");
      }
  sceKernelMunmap(memory, size);
  sceKernelReleaseDirectMemory(physical, size);
  return pass ? 0 : 3;
}
