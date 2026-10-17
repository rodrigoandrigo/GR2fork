// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <orbis/GnmDriver.h>
#include <orbis/libkernel.h>

namespace {
constexpr size_t AllocationSize = 1024 * 1024;
struct BinaryInfo {
  char signature[7];
  uint8_t version;
  uint32_t flags_length, usage;
  uint64_t hash;
  uint32_t crc;
};
static_assert(sizeof(BinaryInfo) == 32);
struct Commands {
  uint32_t *begin;
  uint32_t *current;
  void Packet(uint32_t opcode, std::initializer_list<uint32_t> body,
              bool compute = false) {
    *current++ = 0xc0000000 | ((uint32_t(body.size()) - 1) << 16) |
                 (opcode << 8) | (compute ? 2 : 0);
    for (auto value : body)
      *current++ = value;
  }
  void Register(uint32_t address, uint32_t value) {
    Packet(0x76, {address - 0x2c00, value}, true);
  }
};
uint32_t Move(uint32_t vgpr, uint32_t value) {
  return 0x7e000200 | (vgpr << 17) | (128 + value);
}
bool Run(uint8_t *bytes, uint16_t value) {
  auto *texture = bytes + 0x40000;
  auto *output = reinterpret_cast<volatile uint32_t *>(bytes + 0x20000);
  auto *fence = reinterpret_cast<volatile uint32_t *>(bytes + 0x20100);
  reinterpret_cast<uint16_t *>(texture)[5 * 128 + 3] = value;
  static uint32_t sequence{};
  const size_t shader_offset = 0x10000 + sequence++ * 0x1000;
  auto *shader = reinterpret_cast<uint32_t *>(bytes + shader_offset);
  const uint32_t code[]{Move(0, 3),  Move(1, 5), Move(2, 0), Move(3, 0),
                        0xf0000f00u, 0x00000400, 0xbf8c0f70, Move(8, 0),
                        0xe07c1000,  0x80020408, 0xbf8c0f70, 0xbf810000};
  std::memcpy(shader, code, sizeof(code));
  const BinaryInfo metadata{{'O', 'r', 'b', 'S', 'h', 'd', 'r'},
                            1,
                            uint32_t(sizeof(code)) << 8,
                            0,
                            uint64_t(1),
                            0};
  std::memcpy(bytes + shader_offset + 0x40, &metadata, sizeof(metadata));
  for (uint32_t c = 0; c < 4; ++c)
    output[c] = 0xdeadbeef;
  *fence = 0;

  std::array<uint64_t, 4> descriptor{};
  descriptor[0] = (uintptr_t(texture) >> 8) | (uint64_t(2) << 52);
  descriptor[1] = 7 | (uint64_t(7) << 14) | (uint64_t(4) << 32) |
                  (uint64_t(1) << 41) | (uint64_t(8) << 52) |
                  (uint64_t(9) << 60);
  descriptor[2] = uint64_t(127) << 13;
  std::array<uint32_t, 12> user{};
  std::memcpy(user.data(), descriptor.data(), sizeof(descriptor));
  user[8] = uint32_t(uintptr_t(output));
  user[9] = uint32_t(uintptr_t(output) >> 32) & 0xff;
  user[10] = 16;
  user[11] = 4 | (5 << 3) | (6 << 6) | (7 << 9) | (3u << 27);
  auto *words = reinterpret_cast<uint32_t *>(bytes + 0x30000);
  const int initialized = sceGnmDrawInitDefaultHardwareState350(words, 0x1000);
  if (initialized <= 0 || initialized > 0x1000) {
    std::printf("GNM_INIT error=%08x\n", uint32_t(initialized));
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
  commands.Register(0x2e12, 2 | (1 << 6));
  commands.Register(0x2e13, 12 << 1);
  commands.Register(0x2e15, 0x170);
  commands.Register(0x2e16, 0xffffffff);
  commands.Register(0x2e17, 0xffffffff);
  commands.Packet(0x76,
                  {0x240, user[0], user[1], user[2], user[3], user[4], user[5],
                   user[6], user[7], user[8], user[9], user[10], user[11]},
                  true);
  commands.Packet(0x15, {1, 1, 1, 0x21}, true);
  const auto completion = uintptr_t(fence);
  commands.Packet(0x48,
                  {47u | (6u << 8), uint32_t(completion),
                   uint32_t(completion >> 32) | (2u << 29), 1},
                  true);
  commands.Packet(0x47, {20u | (5u << 8), uint32_t(completion),
                         uint32_t(completion >> 32) | (1u << 29), 2, 0});
  void *addresses[]{words};
  uint32_t sizes[]{
      uint32_t((commands.current - commands.begin) * sizeof(uint32_t))};
  const int submitted =
      sceGnmSubmitCommandBuffers(1, addresses, sizes, nullptr, nullptr);
  if (submitted < 0) {
    std::printf("SUBMIT error=%08x\n", uint32_t(submitted));
    return false;
  }
  for (uint32_t wait = 0; *fence != 2; ++wait) {
    if (wait == 10000)
      std::printf("GPU_FENCE TIMEOUT; retaining submitted resources\n");
    sceKernelUsleep(1000);
  }
  const uint32_t bits = output[0];
  float actual{};
  std::memcpy(&actual, &bits, sizeof(actual));
  const float expected = float(value) / 65535.f;
  const bool pass =
      std::isfinite(actual) && std::abs(actual - expected) <= 0.000001f;
  std::printf("UNORM16 value=%u actual=%.9f expected=%.9f %s\n", value, actual,
              expected, pass ? "PASS" : "FAIL");
  return pass;
}
} // namespace

int main() {
  off_t physical{};
  void *memory{};
  int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
                                         AllocationSize, 0x10000, 3, &physical);
  if (rc < 0)
    return 1;
  rc = sceKernelMapDirectMemory(&memory, AllocationSize, 0x33, 0, physical,
                                0x10000);
  if (rc < 0) {
    sceKernelReleaseDirectMemory(physical, AllocationSize);
    return 2;
  }
  auto *bytes = static_cast<uint8_t *>(memory);
  std::memset(bytes, 0, AllocationSize);
  bool pass = true;
  for (const uint16_t value :
       {uint16_t(0), uint16_t(1), uint16_t(32767), uint16_t(32768),
        uint16_t(65534), uint16_t(65535)})
    pass &= Run(bytes, value);
  sceKernelMunmap(memory, AllocationSize);
  sceKernelReleaseDirectMemory(physical, AllocationSize);
  std::printf("UNORM16_READ_RESULT=%s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 3;
}
