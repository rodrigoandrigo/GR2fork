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

bool Measure(uint8_t *bytes, uint32_t domain, uint32_t topology,
             uint32_t partition, float factor, uint32_t &count,
             uint32_t &hash) {
  std::memset(bytes + 0x200000, 0, 0x400000);
  const auto mov = [](uint32_t dst, uint32_t src) {
    return 0x7e000200u | (dst << 17) | src;
  };
  const auto alu = [](uint32_t op, uint32_t dst, uint32_t a, uint32_t b) {
    return (op << 25) | (dst << 17) | (b << 9) | a;
  };
  std::vector<uint32_t> local{mov(8, 128), mov(1, 128)};
  for (uint32_t component = 0; component < 4; ++component)
    local.insert(local.end(), {0xd8340000u | (component * 4), 0x00000108});
  local.push_back(0xbf810000);
  std::vector<uint32_t> hull{
      0xc20a0100,  0xbf8c0000,  mov(8, 20),
      0xd8d80000,  0x09000008,  0xbf8c0000,
      mov(0, 128), mov(4, 255), std::bit_cast<uint32_t>(factor)};
  const uint32_t factors = domain == 0 ? 2 : domain == 1 ? 4 : 6;
  for (uint32_t index = 0; index < factors; ++index)
    hull.insert(hull.end(), {0xe0701000u | (index * 4), 0x10030400});
  hull.push_back(0xbf810000);
  const std::vector<uint32_t> evaluation{
      0xc20a0103,        0xbf8c0000,        mov(8, 20),
      0xd8d80000,        0x09000008,        0xbf8c0000,
      alu(8, 0, 246, 0), alu(8, 1, 246, 1), alu(3, 0, 243, 0),
      alu(3, 1, 243, 1), mov(2, 128),       mov(3, 242),
      mov(8, 128),       mov(12, 129),      0xe0c81000,
      0x80010c08,        0xbf8c0f70,        0xf80008cf,
      0x03020100,        0xbf810000};
  const std::vector<uint32_t> pixel{mov(0, 242), mov(1, 128), mov(2, 128),
                                    mov(3, 242), 0xf800180f,  0x03020100,
                                    0xbf810000};
  auto *ls = bytes + 0x160000;
  auto *hs = bytes + 0x162000;
  auto *ds = bytes + 0x164000;
  auto *ps = bytes + 0x166000;
  const uint64_t key =
      (uint64_t(domain) << 48) | std::bit_cast<uint32_t>(factor);
  Bake(ls, local, 0xf1100001);
  Bake(hs, hull, 0xf1100002 ^ key);
  Bake(ds, evaluation, 0xf1100003);
  Bake(ps, pixel, 0xf1100004);
  const uint32_t constants[]{16, 16, 1, 0, 0, 48, 48, 0, 0};
  std::memcpy(bytes + 0x180000, constants, sizeof(constants));
  uint32_t hs_user[16]{}, ds_user[8]{};
  const auto descriptor = [](uint32_t *words, uintptr_t address,
                             uint32_t size) {
    words[0] = uint32_t(address);
    words[1] = uint32_t(address >> 32);
    words[2] = size;
    words[3] = 4 | (5 << 3) | (6 << 6) | (7 << 9) | (7 << 12) | (4 << 15);
  };
  descriptor(hs_user, uintptr_t(bytes + 0x180000), sizeof(constants));
  descriptor(hs_user + 12, uintptr_t(bytes + 0x300000), 65536);
  descriptor(ds_user, uintptr_t(bytes + 0x180000), sizeof(constants));
  descriptor(ds_user + 4, uintptr_t(bytes + 0x500000), 4);
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
  const auto program = [&](uint32_t reg, uint8_t *code, uint32_t user,
                           uint32_t extra) {
    shader(reg, uint32_t(uintptr_t(code) >> 8));
    shader(reg + 1, uint32_t(uintptr_t(code) >> 40));
    shader(reg + 2, 3 | (3 << 6));
    shader(reg + 3, (user << 1) | extra);
  };
  program(0x2d48, ls, 0, 1 << 7);
  program(0x2d08, hs, 16, 0);
  program(0x2c48, ds, 8, 0);
  program(0x2c08, ps, 0, 0);
  for (uint32_t i = 0; i < 16; ++i)
    shader(0x2d0c + i, hs_user[i]);
  for (uint32_t i = 0; i < 8; ++i)
    shader(0x2c4c + i, ds_user[i]);
  context(0xa2d5, 0x45);
  context(0xa2d6, 1 | (3 << 8) | (1 << 14));
  context(0xa2db, domain | (partition << 2) | (topology << 5));
  context(0xa286, std::bit_cast<uint32_t>(64.f));
  context(0xa287, std::bit_cast<uint32_t>(1.f));
  commands.Packet(0x79, {0x250, uint32_t(uintptr_t(bytes + 0x300000) >> 8)});
  commands.Packet(0x79, {0x24e, 256});
  commands.Packet(0x79, {0x242, 9});
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
  context(0xa205, (1 << 3) | (2 << 5) | (2 << 8));
  context(0xa280, (8 << 16) | 8);
  context(0xa282, 8);
  context(0xa206, 0x3f);
  context(0xa207, 0);
  context(0xa292, 2);
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
  context(0xa318, uint32_t(uintptr_t(bytes + 0x200000) >> 8));
  context(0xa319, 7);
  context(0xa31a, 15);
  context(0xa31c, 0x8028);
  context(0xa31d, 8);
  context(0xa2f8, 0);
  context(0xa201, 0);
  context(0xa293, 0);
  context(0xa30e, 0xffffffff);
  context(0xa30f, 0xffffffff);
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
  if (rc < 0)
    return false;
  for (uint32_t wait = 0; *fence != 2; ++wait) {
    if (wait == 10000)
      return false;
    sceKernelUsleep(1000);
  }
  std::memcpy(&count, bytes + 0x500000, 4);
  hash = 2166136261;
  for (uint32_t y = 0; y < 16; ++y)
    for (uint32_t x = 0; x < 16; ++x)
      for (uint32_t c = 0; c < 4; ++c)
        hash = (hash ^ bytes[0x200000 + (y * 64 + x) * 4 + c]) * 16777619;
  std::printf("TESS_CASE domain=%u topology=%u partition=%u factor=%08x "
              "count=%u hash=%08x\n",
              domain, topology, partition, std::bit_cast<uint32_t>(factor),
              count, hash);
  return true;
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
  for (uint32_t domain = 0; domain < 3; ++domain)
    for (uint32_t topology = 0; topology < 4; ++topology) {
      if (domain == 0 ? topology > 1 : topology == 1)
        continue;
      for (float factor : {0.f, 1.f, 1.5f, 3.1f, 4.1f, 8.f, 64.f}) {
        uint32_t count[2]{}, hash[2]{};
        for (uint32_t partition = 0; partition < 2; ++partition)
          if (!Measure(bytes, domain, topology, partition, factor,
                       count[partition], hash[partition]))
            return 1;
        pass &= count[0] == count[1] && hash[0] == hash[1];
      }
    }
  std::printf("TESS_RESULT %s\n", pass ? "PASS" : "FAIL");
  sceKernelMunmap(memory, AllocationSize);
  sceKernelReleaseDirectMemory(physical, AllocationSize);
  return pass ? 0 : 1;
}
