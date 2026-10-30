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
bool Run(uint8_t *bytes, const uint32_t *code, uint32_t size,
         const std::array<uint32_t, 16> &users) {
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
  commands.Register(0x2e12, 3 | (1 << 6) | (3 << 18) | (1 << 23));
  commands.Register(0x2e13, 16 << 1);
  commands.Register(0x2e15, 0x170);
  commands.Register(0x2e16, ~0u);
  commands.Register(0x2e17, ~0u);
  for (uint32_t i = 0; i < users.size(); ++i)
    commands.Register(0x2e40 + i, users[i]);
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
  sceGnmSubmitDone();
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
  const auto vop1 = [](uint32_t op, uint32_t dst, uint32_t src) {
    return 0x7e000000u | (dst << 17) | (op << 9) | src;
  };
  const auto descriptor = [&](uint32_t format, uint32_t number) {
    const uint64_t address = uintptr_t(bytes + 0x40000) >> 8;
    return std::array<uint64_t, 4>{
        address | (uint64_t(format) << 52) | (uint64_t(number) << 58),
        15ull | (15ull << 14) | (4ull << 32) | (5ull << 35) | (6ull << 38) |
            (7ull << 41) | (8ull << 52) | (9ull << 60),
        63ull << 13, 0};
  };
  std::array<uint32_t, 16> users{};
  auto rgba = descriptor(10, 0), integer = descriptor(4, 4);
  std::memcpy(users.data(), rgba.data(), 32);
  std::memcpy(users.data() + 8, integer.data(), 32);
  const uint32_t code[]{
      vop1(1, 0, 131), vop1(1, 1, 130), vop1(1, 4, 242), vop1(1, 5, 128),
      vop1(1, 6, 128), vop1(1, 7, 242), 0xf0201f00,      0x00000400,
      0xbf8c0f70,      0xf0001100,      0x00020800,      0xbf8c0f70,
      vop1(1, 0, 132), 0xf0201100,      0x00020800,      0xbf810000};
  std::memset(bytes + 0x40000, 0x5a, 4096);
  if (!Run(bytes, code, sizeof(code), users))
    return 2;
  uint32_t mismatch{};
  for (uint32_t word = 0; word < 1024; ++word) {
    uint32_t value{};
    std::memcpy(&value, bytes + 0x40000 + word * 4, 4);
    const uint32_t expected =
        word == 2 * 64 + 3 || word == 2 * 64 + 4 ? 0xff0000ff : 0x5a5a5a5a;
    mismatch += value != expected;
  }
  pass &= mismatch == 0;
  std::printf("SAME_BASE_RGBA8_R32_UINT mismatches=%u %s\n", mismatch,
              pass ? "PASS" : "FAIL");
  sceKernelMunmap(memory, size);
  sceKernelReleaseDirectMemory(physical, size);
  return pass ? 0 : 3;
}
