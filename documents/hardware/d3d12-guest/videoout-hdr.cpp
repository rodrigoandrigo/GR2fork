// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

int main() {
  constexpr uint32_t Width = 1280, Height = 720;
  constexpr size_t Size = 8 * 1024 * 1024;
  off_t physical{};
  void *memory{};
  int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), Size,
                                         0x10000, 3, &physical);
  if (rc < 0)
    return 1;
  rc = sceKernelMapDirectMemory(&memory, Size, 0x33, 0, physical, 0x10000);
  if (rc < 0) {
    sceKernelReleaseDirectMemory(physical, Size);
    return 2;
  }
  const int video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN,
                                    0, nullptr);
  if (video < 0)
    return 3;
  const auto pq = [](double nits) {
    const double p = std::pow(nits / 10000.0, 2610.0 / 16384.0);
    return uint32_t(std::round(1023.0 * std::pow((3424.0 / 4096.0 +
        2413.0 / 128.0 * p) / (1.0 + 2392.0 / 128.0 * p), 2523.0 / 32.0)));
  };
  const std::array<double, 8> nits{0, 10, 80, 100, 203, 400, 1000, 10000};
  const std::array<uint16_t, 8> halves{0, 0x3000, 0x3c00, 0x3d00,
                                        0x4100, 0x4500, 0x4a40, 0x57d0};
  for (const uint32_t format : {0x80000000u, 0xc1060000u, 0x88740000u,
                                 0xc1060000u, 0x80000000u}) {
    std::memset(memory, 0, Size);
    for (uint32_t y = 0; y < Height; ++y)
      for (uint32_t x = 0; x < Width; ++x) {
        const uint32_t band = x * 8 / Width;
        if (format == 0xc1060000u) {
          auto *pixel = static_cast<uint16_t *>(memory) + (y * Width + x) * 4;
          pixel[0] = y < Height / 2 ? halves[band] : 0;
          pixel[1] = y < Height / 2 ? halves[band] : 0x3800;
          pixel[2] = halves[band];
          pixel[3] = 0x3c00;
        } else {
          uint32_t value = format == 0x88740000u ? pq(nits[band]) : band * 255 / 7;
          static_cast<uint32_t *>(memory)[y * Width + x] = format == 0x88740000u
              ? 0xc0000000u | (value << 20) | (value << 10) | value
              : 0xff000000u | (value << 16) | (value << 8) | value;
        }
      }
    OrbisVideoOutBufferAttribute attribute{};
    sceVideoOutSetBufferAttribute(&attribute, format, 1, 0, Width, Height, Width);
    void *buffers[]{memory};
    const int group = sceVideoOutRegisterBuffers(video, 0, buffers, 1, &attribute);
    std::printf("VIDEOOUT_FORMAT=%08x REGISTER=%08x\n", format, uint32_t(group));
    if (group < 0) {
      rc = group;
      break;
    }
    for (int frame = 0; frame < 600; ++frame) {
      rc = sceVideoOutSubmitFlip(video, 0, ORBIS_VIDEO_OUT_FLIP_VSYNC, frame);
      if (rc < 0)
        break;
      while (sceVideoOutIsFlipPending(video) > 0)
        sceKernelUsleep(1000);
    }
    while (sceVideoOutIsFlipPending(video) > 0)
      sceKernelUsleep(1000);
    std::printf("VIDEOOUT_FORMAT=%08x FLIP=%08x; visual comparison required\n",
                format, uint32_t(rc));
    sceVideoOutUnregisterBuffers(video, group);
    if (rc < 0)
      break;
  }
  sceVideoOutClose(video);
  sceKernelMunmap(memory, Size);
  sceKernelReleaseDirectMemory(physical, Size);
  return rc < 0 ? 4 : 0;
}
