// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <orbis/GnmDriver.h>
#include <orbis/libkernel.h>
#include <vector>

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
void Bake(uint8_t *destination, std::vector<uint32_t> code, uint64_t hash) {
  if (code.front() == 0xbeeb03ff)
    code[1] = 255;
  else
    code.insert(code.begin(), {0xbeeb03ff, 255});
  std::memcpy(destination, code.data(), code.size() * 4);
  const ShaderBinaryInfo info{{'O', 'r', 'b', 'S', 'h', 'd', 'r'},
                              1,
                              uint32_t(code.size() * 4) << 8,
                              0,
                              hash,
                              0};
  std::memcpy(destination + 0x800, &info, sizeof(info));
}
bool Measure(uint8_t *bytes, uint32_t kind, bool last, uint32_t &hash) {
  constexpr bool onchip = false;
  auto *target = bytes + 0x200000;
  std::memset(target, 0, 4096);
  std::memset(bytes + 0x300000, 0, 0x300000);
  const auto vop1 = [](uint32_t op, uint32_t dst, uint32_t src) {
    return 0x7e000000u | (dst << 17) | (op << 9) | src;
  };
  const auto vop2 = [](uint32_t op, uint32_t dst, uint32_t src0,
                       uint32_t src1) {
    return (op << 25) | (dst << 17) | (src1 << 9) | src0;
  };
  std::vector<uint32_t> export_code{
      vop2(26, 1, 129, 0), vop2(27, 1, 130, 1), vop2(27, 2, 130, 0),
      vop1(6, 1, 257),     vop1(6, 2, 258),     vop2(8, 1, 244, 1),
      vop2(8, 2, 244, 2),  vop2(3, 1, 243, 1),  vop2(3, 2, 243, 2),
      vop1(1, 3, 128),     vop1(1, 4, 242),     vop1(1, 8, 128)};
  for (uint32_t c = 0; c < 4; ++c)
    export_code.insert(export_code.end(),
                       {0xe0705000u | c * 4, 0x04000008u | ((c + 1) << 8)});
  export_code.push_back(0xbf810000);
  std::vector<uint32_t> geometry{vop1(1, 8, 128), vop1(1, 12, 129), 0xe0c81000,
                                 0x80020c08, 0xbf8c0f70};
  const uint32_t input_regs[]{0, 1, 3};
  for (uint32_t v = 0; v < 3; ++v) {
    const uint32_t source = last ? v : (v + 2) % 3;
    geometry.push_back(vop2(26, 8, 130, input_regs[source]));
    for (uint32_t c = 0; c < 4; ++c)
      geometry.insert(geometry.end(), {0xbe9003ff, c * 256, 0xe0305000,
                                       0x10010008u | ((4 + c) << 8)});
    geometry.push_back(0xbf8c0f70);
    for (uint32_t c = 0; c < 4; ++c)
      geometry.insert(geometry.end(), {0xe0705000u | (c * 12 + v * 4),
                                       0x0c000000u | ((4 + c) << 8)});
    geometry.insert(geometry.end(),
                    {vop1(1, 9, 255),
                     std::bit_cast<uint32_t>(float(source + 1) * .25f),
                     0xe0705000u | (48 + v * 4), 0x0c000900});
    geometry.push_back(0xbf900022);
  }
  geometry.insert(geometry.end(), {0xbf900012, 0xbf900003, 0xbf810000});
  const std::vector<uint32_t> copy{
      0xbeeb03ff, 255,        0xe0301000, 0x80000000, 0xe03010c0, 0x80000100,
      0xe0301180, 0x80000200, 0xe0301240, 0x80000300, 0xe0301300, 0x80000400,
      0xf80008cf, 0x03020100, 0xf8000201, 0x00000004, 0xbf810000};
  const std::vector<uint32_t> pixel{
      0xc8020002, 0xbf800000, vop1(1, 1, 128), vop1(1, 2, 242),
      0xf800180f, 0x02010100, 0xbf810000};
  auto *vs = bytes + 0x160000, *es = bytes + 0x162000, *gs = bytes + 0x164000,
       *ps = bytes + 0x166000;
  Bake(vs, copy, 0xf0900001);
  Bake(es, export_code, 0xf0900002);
  Bake(gs, geometry, 0xf0900003);
  Bake(ps, pixel, 0xf0900004);
  uint32_t vs_user[8]{}, ps_user[16]{}, es_user[4]{}, gs_user[12]{};
  const auto descriptor = [&](uint32_t *out, uintptr_t address, bool swizzled) {
    out[0] = uint32_t(address);
    out[1] =
        uint32_t(address >> 32) | (swizzled ? (16u << 16) | (1u << 31) : 0);
    out[2] = 65536;
    out[3] = 4 | (5 << 3) | (6 << 6) | (7 << 9) | (7 << 12) | (4 << 15) |
             (swizzled ? (1 << 19) | (3 << 21) | (1 << 23) : 0);
  };
  descriptor(es_user, uintptr_t(bytes + 0x300000), true);
  descriptor(gs_user, uintptr_t(bytes + 0x400000), true);
  descriptor(gs_user + 4, uintptr_t(bytes + 0x300000), false);
  descriptor(gs_user + 8, uintptr_t(bytes + 0x500000), false);
  descriptor(vs_user, uintptr_t(bytes + 0x400000), false);
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
  shader(0x2c0a, 5 | (2 << 6));
  shader(0x2c0b, 16 << 1);
  for (uint32_t i = 0; i < 16; ++i) {
    if (i < 8)
      shader(0x2c4c + i, vs_user[i]);
    shader(0x2c0c + i, ps_user[i]);
  }

  shader(0x2cc8, uint32_t(uintptr_t(es) >> 8));
  shader(0x2cc9, uint32_t(uintptr_t(es) >> 40));
  shader(0x2cca, 3 | (2 << 6));
  shader(0x2ccb, (4 << 1) | (onchip ? 1u << 20 : 0));
  shader(0x2c88, uint32_t(uintptr_t(gs) >> 8));
  shader(0x2c89, uint32_t(uintptr_t(gs) >> 40));
  shader(0x2c8a, 3 | (3 << 6));
  shader(0x2c8b, 12 << 1);
  for (uint32_t i = 0; i < 4; ++i)
    shader(0x2ccc + i, es_user[i]);
  for (uint32_t i = 0; i < 12; ++i)
    shader(0x2c8c + i, gs_user[i]);
  context(0xa2d5, 0xb0);
  context(0xa291, 3 | (1 << 11));
  context(0xa29b, 2);
  context(0xa2ab, 4);
  context(0xa2ac, 24);
  context(0xa2ce, 3);
  context(0xa2d7, 8);
  context(0xa2e4, 0);
  commands.Packet(0x79, {0x240, 256, 256});
  context(0xa290, 3 | (3 << 4) | ((onchip ? 3u : 0u) << 21));
  context(0xa1b1, 1);
  context(0xa1b3, 2);
  context(0xa1b4, 2);
  context(0xa1b6, 1);
  context(0xa191, 1 << 10);
  context(0xa1c3, 4);
  context(0xa1c4, 0);
  context(0xa1c5, 9);
  context(0xa200, 0);
  context(0xa202, 0x00cc0020);
  context(0xa203, 0xe00);
  context(0xa204, 1 << 19);
  const uint32_t polygon_mode = kind == 1 ? 0 : kind >= 2 ? 1 : 2;
  context(0xa205, (1 << 3) | (polygon_mode << 5) | (polygon_mode << 8) |
                      (last ? 1u << 19 : 0));
  context(0xa280, (8 << 16) | 8);
  context(0xa282, kind >= 2 ? 16 : 8);
  context(0xa283, kind == 3 ? 0x00015555 : 0x0001ffff);
  context(0xa20a, 0x3f800000);
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
  context(0xa2f8, 0);
  context(0xa201, 0);
  context(0xa293, 0);
  context(0xa30e, 0xffffffff);
  context(0xa30f, 0xffffffff);
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
  if (rc >= 0) {
    for (uint32_t wait = 0; *fence != 2; ++wait) {
      if (wait == 10000)
        return false;
      sceKernelUsleep(1000);
    }
  }
  hash = 2166136261;
  for (uint32_t y = 0; y < 16; ++y)
    for (uint32_t x = 0; x < 16; ++x)
      for (uint32_t c = 0; c < 4; ++c)
        hash = (hash ^ target[(y * 64 + x) * 4 + c]) * 16777619;
  uint32_t counter{};
  std::memcpy(&counter, bytes + 0x500000, 4);
  std::printf("F15_CASE last=%u kind=%u hash=%08x counter=%u submit=%08x\n",
              last, kind, hash, counter, uint32_t(rc));
  return rc >= 0 && counter == 1;
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
  for (uint32_t kind = 0; kind < 4; ++kind) {
    uint32_t offchip{}, onchip{};
    if (!Measure(bytes, kind, false, offchip) ||
        !Measure(bytes, kind, true, onchip))
      return 1;
    pass &= offchip == onchip;
  }
  std::printf("F15_FLAT_RESULT %s\n", pass ? "PASS" : "FAIL");
  sceKernelMunmap(memory, AllocationSize);
  sceKernelReleaseDirectMemory(physical, AllocationSize);
  return pass ? 0 : 1;
}
