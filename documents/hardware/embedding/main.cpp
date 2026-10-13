#include <atomic>
#include <cstdio>
#include <orbis/libkernel.h>
#include <pthread.h>

namespace {
std::atomic_bool finished{};
std::atomic_uint ticks{};

void *Worker(void *) {
  while (!finished.load()) {
    ++ticks;
    sceKernelUsleep(1000);
  }
  return nullptr;
}
}

int main() {
  auto *executable = std::fopen("/app0/eboot.bin", "rb");
  if (!executable)
    return 1;
  unsigned char header[16]{};
  const auto read = std::fread(header, 1, sizeof(header), executable);
  std::fclose(executable);
  if (read != sizeof(header))
    return 2;
  pthread_t workers[2]{};
  for (auto &worker : workers)
    if (pthread_create(&worker, nullptr, Worker, nullptr))
      return 3;
#ifdef EMBEDDING_MANUAL_STOP
  for (;;)
    sceKernelUsleep(10000);
#else
  sceKernelUsleep(3000000);
  finished.store(true);
  for (auto &worker : workers)
    if (pthread_join(worker, nullptr))
      return 4;
  std::printf("EMBEDDING_EXIT ticks=%u\n", ticks.load());
  return ticks.load() ? 0 : 5;
#endif
}
