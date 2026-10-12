// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <orbis/GnmDriver.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

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
} // namespace

int main() {
  off_t physical{};
  void *memory{};
  void *alias{};
  int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
                                         AllocationSize, 0x10000, 3, &physical);
  if (rc < 0) {
    std::printf("ALLOCATE error=%08x\n", uint32_t(rc));
    return 1;
  }
  rc = sceKernelMapDirectMemory(&memory, AllocationSize, 0x33, 0, physical,
                                0x10000);
  if (rc < 0) {
    sceKernelReleaseDirectMemory(physical, AllocationSize);
    return 2;
  }
  rc = sceKernelMapDirectMemory(&alias, AllocationSize, 0x33, 0, physical,
                                0x10000);
  if (rc < 0) {
    sceKernelMunmap(memory, AllocationSize);
    sceKernelReleaseDirectMemory(physical, AllocationSize);
    return 3;
  }
  std::memset(memory, 0, AllocationSize);
  auto *bytes = static_cast<uint8_t *>(memory);
  auto *alias_bytes = static_cast<uint8_t *>(alias);
  auto *output = reinterpret_cast<volatile uint32_t *>(bytes + 0x20000);
  auto *alias_output =
      reinterpret_cast<volatile uint32_t *>(alias_bytes + 0x20000);
  *output = 0x2468ace0;
  const bool cpu_alias = *alias_output == 0x2468ace0;
  std::printf("CPU_ALIAS %s\n", cpu_alias ? "PASS" : "FAIL");

  auto *shader = reinterpret_cast<uint32_t *>(bytes + 0x10000);
  const uint32_t code[]{0x7e0002ff, 0x11223344, 0x7e020280, 0xe0701000,
                        0x80000001, 0xbf8c0f70, 0xbf810000};
  std::memcpy(shader, code, sizeof(code));
  const ShaderBinaryInfo metadata{{'O', 'r', 'b', 'S', 'h', 'd', 'r'},
                                  1,
                                  uint32_t(sizeof(code)) << 8,
                                  0,
                                  0x11223344,
                                  0};
  std::memcpy(bytes + 0x10020, &metadata, sizeof(metadata));

  auto *command_words = reinterpret_cast<uint32_t *>(bytes + 0x30000);
  const int initialized =
      sceGnmDrawInitDefaultHardwareState350(command_words, 0x1000);
  if (initialized <= 0 || initialized > 0x1000) {
    std::printf("GNM_INIT error=%08x\n", uint32_t(initialized));
    return 4;
  }
  Commands commands{command_words, command_words + initialized};
  commands.Register(0x2e04, 0);
  commands.Register(0x2e05, 0);
  commands.Register(0x2e06, 0);
  commands.Register(0x2e07, 1);
  commands.Register(0x2e08, 1);
  commands.Register(0x2e09, 1);
  commands.Register(0x2e0c, uint32_t(uintptr_t(shader) >> 8));
  commands.Register(0x2e0d, uint32_t(uintptr_t(shader) >> 40));
  commands.Register(0x2e12, 0);
  commands.Register(0x2e13, 4 << 1);
  commands.Register(0x2e15, 0x170);
  commands.Register(0x2e16, 0xffffffff);
  commands.Register(0x2e17, 0xffffffff);
  const auto destination = uintptr_t(alias_output);
  commands.Packet(0x76,
                  {0x240, uint32_t(destination),
                   uint32_t(destination >> 32) & 0xff, 4,
                   4u | (5u << 3) | (6u << 6) | (7u << 9) | (3u << 27)},
                  true);
  commands.Packet(0x15, {1, 1, 1, 0x21}, true);
  auto *fence = reinterpret_cast<volatile uint32_t *>(bytes + 0x20100);
  const auto fence_address = uintptr_t(fence);
  commands.Packet(0x48,
                  {47u | (6u << 8), uint32_t(fence_address),
                   uint32_t(fence_address >> 32) | (2u << 29), 1},
                  true);
  commands.Packet(0x47, {20u | (5u << 8), uint32_t(fence_address),
                         uint32_t(fence_address >> 32) | (1u << 29), 2, 0});
  void *addresses[]{command_words};
  uint32_t sizes[]{
      uint32_t((commands.current - commands.begin) * sizeof(uint32_t))};
  rc = sceGnmSubmitCommandBuffers(1, addresses, sizes, nullptr, nullptr);
  if (rc < 0) {
    std::printf("SUBMIT error=%08x\n", uint32_t(rc));
    return 5;
  }
  bool timed_out{};
  for (uint32_t wait = 0; *fence != 2; ++wait) {
    if (wait == 10000) {
      timed_out = true;
      std::printf("GPU_FENCE TIMEOUT; keeping submitted memory alive\n");
    }
    sceKernelUsleep(1000);
  }
  const bool gpu_alias = *output == 0x11223344 && *alias_output == 0x11223344;
  std::printf("GPU_ALIAS %s original=%08x alias=%08x fence=%u\n",
              gpu_alias ? "PASS" : "FAIL", *output, *alias_output, *fence);

  auto *pixels = reinterpret_cast<uint32_t *>(bytes + 0x40000);
  for (uint32_t y = 0; y < Height; ++y)
    for (uint32_t x = 0; x < Width; ++x)
      pixels[y * Width + x] = 0xff000000 | ((x * 255 / (Width - 1)) << 16) |
                              ((y * 255 / (Height - 1)) << 8) |
                              ((x / 32 ^ y / 32) & 1 ? 255 : 0);
  std::printf("VIDEOOUT_CPU_HASH %08x\n", Hash(pixels, Width * Height));
  const int video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN,
                                    ORBIS_VIDEO_OUT_BUS_MAIN, 0, nullptr);
  OrbisVideoOutBufferAttribute attribute{};
  sceVideoOutSetBufferAttribute(&attribute, 0x80000000, 1, 0, Width, Height,
                                Width);
  void *buffers[]{alias_bytes + 0x40000};
  rc = video < 0 ? video
                 : sceVideoOutRegisterBuffers(video, 0, buffers, 1, &attribute);
  if (rc >= 0)
    for (int frame = 0; frame < 300; ++frame) {
      rc = sceVideoOutSubmitFlip(video, 0, ORBIS_VIDEO_OUT_FLIP_VSYNC, frame);
      if (rc < 0)
        break;
      while (sceVideoOutIsFlipPending(video) > 0)
        sceKernelUsleep(1000);
    }
  std::printf("VIDEOOUT_API %s error=%08x; visual validation required\n",
              rc >= 0 ? "PASS" : "FAIL", uint32_t(rc));
  if (video >= 0) {
    sceVideoOutUnregisterBuffers(video, 0);
    sceVideoOutClose(video);
  }
  sceKernelMunmap(alias, AllocationSize);
  sceKernelMunmap(memory, AllocationSize);
  sceKernelReleaseDirectMemory(physical, AllocationSize);
  return cpu_alias && gpu_alias && !timed_out && rc >= 0 ? 0 : 6;
}
