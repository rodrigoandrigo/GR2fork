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

uint32_t Render(uint8_t *bytes, bool indirect, bool indexed, uint32_t primitive,
                uint32_t active) {
  constexpr uint32_t format = 10, number = 0, swap = 0, pixel_bytes = 4,
                     word0 = 0, word1 = 0;
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
      vop1(1, 3, 128),     vop1(1, 4, 242),     vop1(6, 5, 0),
      vop1(6, 6, 1),       vop1(1, 7, 255),     0x3c800000,
      vop2(8, 5, 263, 5),  vop2(8, 6, 263, 6),  vop2(3, 1, 261, 1),
      vop2(3, 2, 262, 2),  0xf80008cf,          0x04030201,
      0xbf810000};
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
  uint32_t vs_user[8]{13, 7}, ps_user[8]{};
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
  context(0xa31c, 0x8000 | (format << 2) | (number << 8) | (swap << 11));
  context(0xa31d, 8);
  context(0xa31f, uint32_t(uintptr_t(metadata) >> 8));
  context(0xa320, 15);
  context(0xa323, word0);
  context(0xa324, word1);
  context(0xa1e0, 0);
  commands.Packet(0x79, {0x242, primitive});
  commands.Packet(0x2f, {1});
  const uint32_t vertices = primitive == 4 ? 3 : 8;
  auto *index_data = reinterpret_cast<uint32_t *>(bytes + 0x182000);
  for (uint32_t i = 0; i < vertices; ++i)
    index_data[i] = i;
  const auto index_address = uintptr_t(index_data);
  if (indexed) {
    commands.Packet(0x2a, {1});
    commands.Packet(0x26,
                    {uint32_t(index_address), uint32_t(index_address >> 32)});
    commands.Packet(0x13, {vertices});
  }
  if (indirect) {
    auto *arguments = reinterpret_cast<uint32_t *>(bytes + 0x180000);
    std::memset(arguments, 0, 64);
    arguments[0] = vertices;
    arguments[1] = 1;
    arguments[2] = indexed ? 0 : 13;
    arguments[3] = indexed ? 13 : 7;
    if (indexed)
      arguments[4] = 7;
    auto *count = reinterpret_cast<uint32_t *>(bytes + 0x181000);
    *count = active;
    const auto base = uintptr_t(arguments), count_address = uintptr_t(count);
    commands.Packet(0x11, {1, uint32_t(base), uint32_t(base >> 32)});
    commands.Packet(indexed ? 0x9d : 0x9c,
                    {0, 0x4c, 0x4d, 1u << 30, 2, uint32_t(count_address),
                     uint32_t(count_address >> 32), indexed ? 28u : 24u,
                     indexed ? 0u : 2u});
  } else if (active) {
    if (indexed)
      commands.Packet(0x27, {vertices, uint32_t(index_address),
                             uint32_t(index_address >> 32), vertices, 0});
    else
      commands.Packet(0x2d, {vertices, 2});
  }
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
  if (rc < 0 || timed_out)
    return 0;
  return Hash(reinterpret_cast<const uint32_t *>(bytes + 0x140000), 64 * 16);
}
} // namespace
int main() {
  off_t physical{};
  void *memory{};
  if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
                                    AllocationSize, 0x10000, 3, &physical) < 0)
    return 1;
  if (sceKernelMapDirectMemory(&memory, AllocationSize, 0x33, 0, physical,
                               0x10000) < 0) {
    sceKernelReleaseDirectMemory(physical, AllocationSize);
    return 2;
  }
  bool pass = true;
  for (uint32_t primitive : {4u, 5u, 6u, 19u, 20u})
    for (bool indexed : {false, true})
      for (uint32_t count : {0u, 1u, 2u}) {
        const auto direct = Render(static_cast<uint8_t *>(memory), false,
                                   indexed, primitive, count);
        const auto indirect = Render(static_cast<uint8_t *>(memory), true,
                                     indexed, primitive, count);
        const bool equal = direct && direct == indirect;
        pass &= equal;
        std::printf("INDIRECT_SGPRS primitive=%u indexed=%u active=%u "
                    "direct=%08x indirect=%08x %s\n",
                    primitive, indexed, count, direct, indirect,
                    equal ? "PASS" : "FAIL");
      }
  sceKernelMunmap(memory, AllocationSize);
  sceKernelReleaseDirectMemory(physical, AllocationSize);
  return pass ? 0 : 3;
}
