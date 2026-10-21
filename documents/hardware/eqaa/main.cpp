// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <orbis/GnmDriver.h>
#include <orbis/libkernel.h>

namespace {
constexpr size_t AllocationSize = 8 * 1024 * 1024;
struct ShaderBinaryInfo {
  char signature[7];
  uint8_t version;
  uint32_t flags_length, usage;
  uint64_t hash;
  uint32_t crc;
};
static_assert(sizeof(ShaderBinaryInfo) == 32);
struct Commands {
  uint32_t *begin, *current;
  void Packet(uint32_t opcode, std::initializer_list<uint32_t> body) {
    *current++ =
        0xc0000000 | ((uint32_t(body.size()) - 1) << 16) | (opcode << 8);
    for (auto value : body)
      *current++ = value;
  }
};
bool Measure(uint8_t *bytes, uint32_t coverage_log2, uint32_t fragment_log2,
             uint32_t iteration_log2, bool sample_frequency, uint32_t mask) {
  auto *counter = reinterpret_cast<volatile uint32_t *>(bytes + 0x20000);
  *counter = 0;
  std::memset(bytes + 0x600000, 0, 1024 * 1024);
  const uint32_t expanded = 12 | coverage_log2;
  std::memset(bytes + 0x700000, expanded | (expanded << 4), 2048);
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
  const uint32_t pixel[]{vop1(1, 0, 128), vop1(1, 1, 129), 0xe0c81000,
                         0x80000100, 0xbf810000};
  auto *vs = bytes + 0x160000;
  auto *ps = bytes + 0x161000;
  std::memcpy(vs, vertex, sizeof(vertex));
  std::memcpy(ps, pixel, sizeof(pixel));
  const ShaderBinaryInfo vs_info{{'O', 'r', 'b', 'S', 'h', 'd', 'r'},
                                 1,
                                 uint32_t(sizeof(vertex)) << 8,
                                 0,
                                 0x6571616176730000,
                                 0};
  const ShaderBinaryInfo ps_info{{'O', 'r', 'b', 'S', 'h', 'd', 'r'},
                                 1,
                                 uint32_t(sizeof(pixel)) << 8,
                                 0,
                                 0x6571616170730000,
                                 0};
  std::memcpy(vs + 0x80, &vs_info, sizeof(vs_info));
  std::memcpy(ps + 0x80, &ps_info, sizeof(ps_info));
  uint32_t vs_user[8]{}, ps_user[8]{};
  ps_user[0] = uint32_t(uintptr_t(counter));
  ps_user[1] = uint32_t(uintptr_t(counter) >> 32) & 0xff;
  ps_user[2] = 4;
  ps_user[3] = 4 | (5 << 3) | (6 << 6) | (7 << 9) | (3u << 27);
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
  context(0xa08e, 0);
  context(0xa08f, 0);
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
  context(0xa318, uint32_t(uintptr_t(bytes + 0x200000) >> 8));
  context(0xa319, 63 | (63 << 20));
  context(0xa31a, 2047);
  context(0xa31b, 0);
  context(0xa31c, 0x8028 | (coverage_log2 != fragment_log2 ? 1u << 14 : 0));
  context(0xa31d,
          14 | (14 << 5) | (coverage_log2 << 12) | (fragment_log2 << 15));
  context(0xa31f, uint32_t(uintptr_t(bytes + 0x700000) >> 8));
  context(0xa320, 15);
  context(0xa321, uint32_t(uintptr_t(bytes + 0x600000) >> 8));
  context(0xa322, coverage_log2 == 3 ? 4095 : 1023);
  context(0xa2f8, coverage_log2 | (coverage_log2 << 20));
  context(0xa201, coverage_log2 | (iteration_log2 << 4) | (coverage_log2 << 8) |
                      (coverage_log2 << 12) | (1u << 20));
  context(0xa293, sample_frequency ? 1u << 16 : 0);
  context(0xa30e, mask | (mask << 16));
  context(0xa30f, mask | (mask << 16));
  for (uint32_t pixel = 0; pixel < 4; ++pixel)
    for (uint32_t group = 0; group < 4; ++group) {
      uint32_t positions{};
      for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t sample = group * 4 + i;
        const uint32_t x = uint32_t(int32_t(sample % 4) * 4 - 6) & 15;
        const uint32_t y = uint32_t(int32_t(sample / 4) * 4 - 6) & 15;
        positions |= (x | (y << 4)) << (i * 8);
      }
      context(0xa2fe + pixel * 4 + group, positions);
    }
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
        std::printf("EQAA_FENCE TIMEOUT; retaining submitted memory\n");
      }
      sceKernelUsleep(1000);
    }
  std::printf("EQAA coverage=%u fragments=%u iterations=%u sample_frequency=%u "
              "mask=%04x invocations=%u submit=%08x\n",
              1u << coverage_log2, 1u << fragment_log2, 1u << iteration_log2,
              sample_frequency, mask, uint32_t(*counter), uint32_t(rc));
  return rc >= 0 && !timed_out;
}
} // namespace
int main() {
  off_t physical{};
  int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
                                         AllocationSize, 0x10000, 3, &physical);
  if (rc < 0)
    return 1;
  void *memory{};
  rc = sceKernelMapDirectMemory(&memory, AllocationSize, 0x33, 0, physical,
                                0x10000);
  if (rc < 0) {
    sceKernelReleaseDirectMemory(physical, AllocationSize);
    return 1;
  }
  auto *bytes = static_cast<uint8_t *>(memory);
  std::memset(bytes, 0, AllocationSize);
  bool pass = true;
  for (uint32_t coverage : {1u, 2u, 3u})
    for (uint32_t fragments = 0; fragments <= coverage; ++fragments)
      for (uint32_t iterations = 0; iterations <= fragments; ++iterations)
        for (bool frequency : {false, true})
          for (uint32_t mask : {1u, 3u, (1u << (1u << coverage)) - 1})
            pass &= Measure(bytes, coverage, fragments, iterations, frequency,
                            mask);
  sceKernelMunmap(memory, AllocationSize);
  sceKernelReleaseDirectMemory(physical, AllocationSize);
  return pass ? 0 : 1;
}
