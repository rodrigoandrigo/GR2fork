// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

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
bool Measure(uint8_t *bytes, uint32_t kind, uint32_t variant, bool shared,
             uint32_t &hash) {
  auto *target = bytes + 0x200000;
  auto *texture = bytes + 0x400000;
  std::memset(target, 0, 4096);
  std::memset(texture, 0, 16384);
  const uint32_t offsets[]{0, 1024, 1280, 1536, 1792};
  for (uint32_t mip = 0; mip < 4; ++mip)
    for (uint32_t i = offsets[mip]; i < offsets[mip + 1]; i += 4) {
      texture[i] = variant >= 5 && variant <= 8 ? ((i / 4) & 1 ? 255 : 0)
                   : mip == 0                   ? 0
                   : mip == 1                   ? 128
                                                : 255;
      texture[i + 1] = uint8_t(mip * 40);
      texture[i + 3] = 255;
    }
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
  const auto bits = [](float value) {
    uint32_t word{};
    std::memcpy(&word, &value, 4);
    return word;
  };
  std::vector<uint32_t> pixel;
  const auto scalar = [&](uint32_t reg, float value) {
    pixel.push_back(vop1(1, reg, 255));
    pixel.push_back(bits(value));
  };
  const float level = variant == 1   ? 1.5f
                      : variant == 2 ? 0.75f
                      : variant == 7 ? -1.f
                      : variant == 4 ? 0.f
                                     : 0.5f;
  const float scale = variant >= 9 && variant <= 12 ? 0.5f : 0.125f;
  if (shared)
    pixel.insert(pixel.end(), {vop1(1, 20, 255), 16380, vop1(1, 21, 128),
                               0xe0701000, 0x80031514, 0xbf8c0f70});
  uint32_t opcode = 36, output = 4, address = 0;
  if (kind == 0) {
    scalar(0, variant == 8 ? 0.46875f : 0.5f);
    scalar(1, 0.5f);
    scalar(2, level);
  }
  if (kind == 1) {
    opcode = 34;
    output = 8;
    scalar(0, 0);
    scalar(1, scale);
    scalar(2, variant >= 9 && variant <= 12 ? 0.0625f : scale);
    scalar(3, 0);
    scalar(4, 0.5f);
    scalar(5, 0.5f);
    if (variant == 16)
      opcode = 104;
    if (variant == 13) {
      opcode = 35;
      scalar(6, 2.f);
    }
  }
  if (kind == 2) {
    opcode = 32;
    pixel.insert(pixel.end(), {vop2(8, 0, 255, 0), bits(scale),
                               vop2(8, 1, 255, 1), bits(scale)});
    if (variant == 14) {
      opcode = 33;
      scalar(2, 2.f);
    }
    if (variant == 15) {
      opcode = 38;
      address = 8;
      scalar(8, 0.5f);
      pixel.push_back(vop1(1, 9, 256));
      pixel.push_back(vop1(1, 10, 257));
      scalar(11, 2.f);
    }
  }
  pixel.insert(pixel.end(), {0xf0000f00 | (opcode << 18),
                             0x00400000 | (output << 8) | address, 0xf800180f,
                             (output + 3) << 24 | (output + 2) << 16 |
                                 (output + 1) << 8 | output,
                             0xbf810000});
  auto *vs = bytes + 0x160000;
  auto *ps = bytes + 0x161000;
  std::memcpy(vs, vertex, sizeof(vertex));
  std::memcpy(ps, pixel.data(), pixel.size() * 4);
  const ShaderBinaryInfo vs_info{{'O', 'r', 'b', 'S', 'h', 'd', 'r'},
                                 1,
                                 uint32_t(sizeof(vertex)) << 8,
                                 0,
                                 0x73616d706c657673,
                                 0};
  const ShaderBinaryInfo ps_info{{'O', 'r', 'b', 'S', 'h', 'd', 'r'},
                                 1,
                                 uint32_t(pixel.size() * 4) << 8,
                                 0,
                                 0x73616d7000000000ull + (kind << 16) +
                                     (variant << 1) + shared,
                                 0};
  std::memcpy(vs + 0x100, &vs_info, sizeof(vs_info));
  std::memcpy(ps + 0x100, &ps_info, sizeof(ps_info));
  uint32_t vs_user[8]{}, ps_user[16]{};
  uint64_t image[4]{(uintptr_t(texture) >> 8) | (uint64_t(10) << 52),
                    15 | (uint64_t(15) << 14) | (uint64_t(4) << 32) |
                        (uint64_t(5) << 35) | (uint64_t(6) << 38) |
                        (uint64_t(7) << 41) | (uint64_t(3) << 48) |
                        (uint64_t(9) << 52) | (uint64_t(9) << 60),
                    uint64_t(15) << 13, 0};
  std::memcpy(ps_user, image, sizeof(image));
  const uint64_t mag = variant == 7 ? 0 : 1;
  const uint64_t min = variant >= 9 && variant <= 12 ? 3 : variant == 8 ? 0 : 1;
  const uint64_t mip = variant == 2 ? 1 : 2;
  const uint64_t reduction = variant == 5 ? 1 : variant == 6 ? 2 : 0;
  uint64_t sampler[2]{
      2 | (uint64_t(2) << 3) | (reduction << 29) | (uint64_t(0xfff) << 44) |
          (uint64_t(variant >= 9 && variant <= 12 ? variant - 8 : 0) << 9),
      (mag << 20) | (min << 22) | (mip << 26)};
  if (variant == 3)
    sampler[1] |= 256;
  if (variant == 4) {
    sampler[0] &= ~((uint64_t(0xfff) << 32) | (uint64_t(0xfff) << 44));
    sampler[0] |= (uint64_t(256) << 32) | (uint64_t(384) << 44);
  }
  std::memcpy(ps_user + 8, sampler, sizeof(sampler));
  ps_user[12] = uint32_t(uintptr_t(texture));
  ps_user[13] = uint32_t(uintptr_t(texture) >> 32) & 0xff;
  ps_user[14] = 16384;
  ps_user[15] = 4 | (5 << 3) | (6 << 6) | (7 << 9) | (3u << 27);
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
  bool timed_out{};
  if (rc >= 0)
    for (uint32_t wait = 0; *fence != 2; ++wait) {
      if (wait == 10000) {
        timed_out = true;
        hash = 2166136261;
        for (uint32_t y = 0; y < 16; ++y)
          for (uint32_t x = 0; x < 16; ++x)
            for (uint32_t c = 0; c < 4; ++c)
              hash = (hash ^ target[(y * 64 + x) * 4 + c]) * 16777619;
        std::printf("SAMPLING kind=%u variant=%u shared=%u "
                    "rgba=%02x%02x%02x%02x hash=%08x submit=%08x\n",
                    kind, variant, shared, target[0], target[1], target[2],
                    target[3], hash, uint32_t(rc));
        return rc >= 0 && !timed_out;
      }
    } // namespace
  int main() {
    off_t physical{};
    int rc =
        sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
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
    for (uint32_t kind : {0u, 1u, 2u})
      for (uint32_t variant = 0; variant < 17; ++variant) {
        if ((variant == 13 && kind != 1) ||
            ((variant == 14 || variant == 15) && kind != 2) ||
            (variant == 16 && kind != 1))
          continue;
        uint32_t normal{}, shared{};
        pass &= Measure(bytes, kind, variant, false, normal);
        pass &= Measure(bytes, kind, variant, true, shared);
        pass &= normal == shared;
      }
    std::printf("SAMPLING_RESULT %s\n", pass ? "PASS" : "FAIL");
    sceKernelMunmap(memory, AllocationSize);
    sceKernelReleaseDirectMemory(physical, AllocationSize);
    return pass ? 0 : 1;
  }
