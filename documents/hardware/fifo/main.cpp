#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

int main() {
  constexpr uint32_t Width = 640;
  constexpr uint32_t Height = 360;
  constexpr uint32_t Frames = 240;
  constexpr size_t BufferSize = (Width * Height * 4 + 65535) & ~size_t{65535};
  std::array<void *, 3> buffers{};
  int64_t physical;
  if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
                                    BufferSize * 3, 65536, 3, &physical))
    return 1;
  void *mapped = nullptr;
  if (sceKernelMapDirectMemory(&mapped, BufferSize * 3, 0x33, 0, physical,
                               65536))
    return 2;
  for (size_t i = 0; i < buffers.size(); ++i)
    buffers[i] = static_cast<uint8_t *>(mapped) + i * BufferSize;
  const int handle = sceVideoOutOpen(255, 0, 0, nullptr);
  if (handle < 0)
    return 3;
  OrbisVideoOutBufferAttribute attribute{};
  sceVideoOutSetBufferAttribute(&attribute, 0x80002200, 1, 0, Width, Height,
                                Width);
  if (sceVideoOutRegisterBuffers(handle, 0, buffers.data(), buffers.size(),
                                 &attribute) < 0)
    return 4;
  OrbisVideoOutFlipStatus initial{};
  if (sceVideoOutGetFlipStatus(handle, &initial))
    return 5;
  std::array<int64_t, 3> last_use{-1, -1, -1};
  const auto start = sceKernelGetProcessTime();
  for (uint32_t frame = 0; frame < Frames; ++frame) {
    const auto slot = frame % buffers.size();
    OrbisVideoOutFlipStatus status{};
    while (last_use[slot] >= 0) {
      if (sceVideoOutGetFlipStatus(handle, &status))
        return 6;
      if (status.num - initial.num >= uint64_t(last_use[slot] + 2))
        break;
      if (sceKernelGetProcessTime() - start > 30000000)
        return 7;
      sceKernelUsleep(1000);
    }
    auto *pixels = static_cast<uint32_t *>(buffers[slot]);
    const uint32_t color = 0xff000000 | ((frame * 73 & 255) << 16) |
                           ((frame * 29 & 255) << 8) | (frame & 255);
    for (uint32_t y = 0; y < Height; ++y)
      for (uint32_t x = 0; x < Width; ++x)
        pixels[y * Width + x] =
            y < 64 && x < 480
                ? ((frame >> (x / 20)) & 1 ? 0xffffffff : 0xff000000)
                : color;
    if (sceVideoOutSubmitFlip(handle, slot, 1, frame))
      return 8;
    last_use[slot] = frame;
    std::printf("SUBMIT frame=%u slot=%zu time=%llu\n", frame, slot,
                static_cast<unsigned long long>(sceKernelGetProcessTime()));
  }
  OrbisVideoOutFlipStatus status{};
  do {
    if (sceVideoOutGetFlipStatus(handle, &status))
      return 9;
    if (sceKernelGetProcessTime() - start > 30000000)
      return 10;
    sceKernelUsleep(1000);
  } while (status.num - initial.num < Frames);
  std::printf("COMPLETE flips=%llu arg=%lld pending=%d\n",
              static_cast<unsigned long long>(status.num - initial.num),
              static_cast<long long>(status.flipArg), status.numFlipPending);
  return status.flipArg == Frames - 1 && status.numFlipPending == 0 ? 0 : 11;
}
