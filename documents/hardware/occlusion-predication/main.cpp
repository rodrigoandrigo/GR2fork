// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
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
  uint32_t flags_length, usage;
  uint64_t hash;
  uint32_t crc;
};
struct Commands {
  uint32_t *begin;
  uint32_t *current;
  void Packet(uint32_t op, std::initializer_list<uint32_t> body,
              bool predicated = false) {
    *current++ = 0xc0000000u | ((uint32_t(body.size()) - 1) << 16) | (op << 8) |
                 uint32_t(predicated);
    for (auto word : body)
      *current++ = word;
  }
  void Context(uint32_t reg, uint32_t value) {
    Packet(0x69, {reg - 0xa000, value});
  }
  void Shader(uint32_t reg, uint32_t value) {
    Packet(0x76, {reg - 0x2c00, value});
  }
};
uint32_t Vop1(uint32_t op, uint32_t dst, uint32_t src) {
  return 0x7e000000 | (dst << 17) | (op << 9) | src;
}
uint32_t Vop2(uint32_t op, uint32_t dst, uint32_t a, uint32_t b) {
  return (op << 25) | (dst << 17) | (b << 9) | a;
}
} // namespace

int main() {
  constexpr size_t Size = 1024 * 1024;
  off_t physical{};
  void *memory{};
  int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
                                         Size, 0x10000, 3, &physical);
  if (rc < 0)
    return 1;
  rc = sceKernelMapDirectMemory(&memory, Size, 0x33, 0, physical, 0x10000);
  if (rc < 0)
    return 2;
  auto *bytes = static_cast<uint8_t *>(memory);
  auto *vs = reinterpret_cast<uint32_t *>(bytes + 0x10000);
  auto *ps = reinterpret_cast<uint32_t *>(bytes + 0x11000);
  const uint32_t vertex[]{
      Vop2(26, 1, 129, 0), Vop2(27, 1, 130, 1), Vop2(27, 2, 130, 0),
      Vop1(6, 1, 257),     Vop1(6, 2, 258),     Vop2(8, 1, 244, 1),
      Vop2(8, 2, 244, 2),  Vop2(3, 1, 243, 1),  Vop2(3, 2, 243, 2),
      Vop1(1, 3, 128),     Vop1(1, 4, 242),     0xf80008cf,
      0x04030201,          0xbf810000};
  const uint32_t fragment[]{Vop1(1, 0, 255), 0x3f000000,      Vop1(1, 1, 255),
                            0x3e800000,      Vop1(1, 2, 255), 0x3f400000,
                            Vop1(1, 3, 255), 0x3f800000,      0xf800180f,
                            0x03020100,      0xbf810000};
  std::memcpy(vs, vertex, sizeof(vertex));
  std::memcpy(ps, fragment, sizeof(fragment));
  for (auto shader : {vs, ps}) {
    const bool is_vs = shader == vs;
    const BinaryInfo metadata{
        {'O', 'r', 'b', 'S', 'h', 'd', 'r'},
        1,
        uint32_t(is_vs ? sizeof(vertex) : sizeof(fragment)) << 8,
        0,
        is_vs ? 1u : 2u,
        0};
    std::memcpy(reinterpret_cast<uint8_t *>(shader) + 0x800, &metadata,
                sizeof(metadata));
  }
  struct Format {
    uint32_t data, number, bytes, bits;
    std::array<uint32_t, 4> source;
  };
  const std::array<Format, 1> formats{{{10, 0, 4, 8, {128, 64, 191, 255}}}};
  bool all_match = true;
  for (const auto &format : formats)
    for (uint32_t operation = 0; operation < 1; ++operation) {
      const uint32_t rop = 0xcc;
      const uint32_t mask = 15;
      const uint32_t instances = 1;
      auto *pixels = bytes + 0x40000;
      std::memset(pixels, 0x35, 64 * 16 * format.bytes);
      auto *fence = reinterpret_cast<volatile uint32_t *>(bytes + 0x30000);
      *fence = 0;
      auto *words = reinterpret_cast<uint32_t *>(bytes);
      const int initialized =
          sceGnmDrawInitDefaultHardwareState350(words, 0x1000);
      if (initialized <= 0 || initialized > 0x1000)
        return 3;
      Commands commands{words, words + initialized};
      for (auto shader : {vs, ps}) {
        const uint32_t base = shader == vs ? 0x2c48 : 0x2c08;
        const auto address = uintptr_t(shader);
        commands.Shader(base, uint32_t(address >> 8));
        commands.Shader(base + 1, uint32_t(address >> 40));
        commands.Shader(base + 2, (1u << 21) | (shader == vs ? 1 : 0));
        commands.Shader(base + 3, 0);
      }
      commands.Context(0xa318, uint32_t(uintptr_t(pixels) >> 8));
      commands.Context(0xa319, 7);
      commands.Context(0xa31a, 15);
      commands.Context(0xa31b, 0);
      commands.Context(0xa31c,
                       (format.data << 2) | (format.number << 8) | (1 << 15));
      commands.Context(0xa31d, 8);
      commands.Context(0xa08e, mask);
      commands.Context(0xa08f, 15);
      commands.Context(0xa202, (rop << 16) | (1 << 4));
      commands.Context(0xa1e0, 0);
      commands.Context(0xa200, 0);
      commands.Context(0xa203, 0);
      commands.Context(0xa204, 0x90000);
      commands.Context(0xa205, 0);
      commands.Context(0xa206, 0x3f);
      commands.Context(0xa207, 0);
      commands.Context(0xa1b1, 0);
      commands.Context(0xa1b3, 0);
      commands.Context(0xa1b4, 0);
      commands.Context(0xa1b6, 0);
      commands.Context(0xa1c3, 4);
      commands.Context(0xa1c5, 9);
      commands.Context(0xa2d5, 0);
      commands.Context(0xa2f8, 0);
      const uint32_t viewport[]{0x41000000, 0x41000000, 0x41000000,
                                0x41000000, 0x3f800000, 0};
      for (uint32_t i = 0; i < 6; ++i)
        commands.Context(0xa10f + i, viewport[i]);
      for (uint32_t reg : {0xa00cu, 0xa081u, 0xa090u, 0xa094u}) {
        commands.Context(reg, reg == 0xa00c ? 0 : 0x80000000u);
        commands.Context(reg + 1, 16 | (16 << 16));
      }
      commands.Packet(0x79, {0x242, 4});
      commands.Packet(0x2f, {instances});
      auto *query = reinterpret_cast<uint64_t *>(bytes + 0x50000);
      auto *copied = reinterpret_cast<uint64_t *>(bytes + 0x51000);
      std::memset(query, 0, 256);
      std::memset(copied, 0, 32);
      const auto q = uintptr_t(query), d = uintptr_t(copied);
      commands.Context(0xa001, 0xff000f02);
      commands.Packet(0x46, {58});
      commands.Packet(0x46, {57u | (1u << 8), uint32_t(q), uint32_t(q >> 32)});
      commands.Packet(0x2d, {3, 2});
      commands.Packet(
          0x46, {57u | (1u << 8), uint32_t(q + 8), uint32_t((q + 8) >> 32)});
      commands.Packet(
          0x20, {uint32_t(q), uint32_t(q >> 32) | (1u << 16) | (1u << 8)});
      commands.Packet(0x40,
                      {5u | (1u << 8) | (1u << 16) | (1u << 20), 0xaabbccdd,
                       0x12345678, uint32_t(d), uint32_t(d >> 32)},
                      true);
      commands.Packet(0x20, {uint32_t(q), uint32_t(q >> 32) | (1u << 16)});
      commands.Packet(0x40,
                      {5u | (1u << 8) | (1u << 20), 0xdeadbeef, 0,
                       uint32_t(d + 8), uint32_t((d + 8) >> 32)},
                      true);
      commands.Packet(0x20, {0, 0});
      const auto completion = uintptr_t(fence);
      commands.Packet(0x47, {20u | (5u << 8), uint32_t(completion),
                             uint32_t(completion >> 32) | (1u << 29), 2, 0});
      void *addresses[]{words};
      uint32_t sizes[]{uint32_t((commands.current - commands.begin) * 4)};
      rc = sceGnmSubmitCommandBuffers(1, addresses, sizes, nullptr, nullptr);
      if (rc < 0)
        return 4;
      sceGnmSubmitDone();
      for (uint32_t wait = 0; *fence != 2; ++wait) {
        if (wait == 10000)
          std::puts("GPU_FENCE TIMEOUT; retaining submitted resources");
        sceKernelUsleep(1000);
      }
      uint64_t samples = 0;
      bool ready = true;
      for (uint32_t rb = 0; rb < 8; ++rb) {
        ready &= (query[rb * 2] & query[rb * 2 + 1] & (1ull << 63)) != 0;
        samples += (query[rb * 2 + 1] - query[rb * 2]) & ~(1ull << 63);
      }
      const bool match = ready && samples == 256 &&
                         copied[0] == 0x12345678aabbccddull && copied[1] == 0;
      std::printf(
          "OCCLUSION_PREDICATION samples=%llu copied=%llx rejected=%llx %s\n",
          static_cast<unsigned long long>(samples),
          static_cast<unsigned long long>(copied[0]),
          static_cast<unsigned long long>(copied[1]), match ? "PASS" : "FAIL");
      all_match &= match;
    }
  sceKernelMunmap(memory, Size);
  sceKernelReleaseDirectMemory(physical, Size);
  return all_match ? 0 : 5;
}
