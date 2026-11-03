#include "dzn_pso_capture.h"
#ifdef _XBOX_UWP
#include "dzn_pso_capture.hpp"
#include "util/log.h"
#include "util/uwp_util.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <windows.h>

namespace {
struct Pending {
   DznPsoCapture::Archive archive;
   std::string key, directory;
   std::mutex save_mutex;
   bool saved = false;
};
std::mutex captures_mutex;
std::unordered_map<unsigned, std::shared_ptr<Pending>> captures;
std::atomic<int> capture_enabled{-1};
std::atomic<int> legacy_override{-1};
size_t captured_bytes;
std::wstring Wide(const std::string &s) {
   int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), -1, nullptr, 0);
   if (!n)
      throw std::runtime_error("invalid capture path");
   std::vector<wchar_t> out(n);
   MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), -1, out.data(), n);
   return out.data();
}
std::string CachePath() {
   char path[4096];
   if (!uwp_get_cache_directory(path, sizeof(path)))
      throw std::runtime_error("LocalCache unavailable");
   return path;
}
void Write(const std::string &path, const void *p, size_t bytes) {
   FILE *f = _wfopen(Wide(path).c_str(), L"wb");
   if (!f)
      throw std::runtime_error("capture open failed");
   bool ok = std::fwrite(p, 1, bytes, f) == bytes;
   int closed = std::fclose(f);
   if (!ok || closed)
      throw std::runtime_error("capture write failed");
}
} // namespace
extern "C" MESA_UWP_EXPORT void mesa_uwp_set_pso_capture(int enabled) {
   capture_enabled.store(enabled != 0);
}
extern "C" void dzn_pso_capture_prepare(const void *stream, const uint8_t *hash) {
   try {
      int enabled = capture_enabled.load();
      if (enabled < 0) {
         enabled = !uwp_is_desktop_device();
         capture_enabled.store(enabled);
      }
      if (!enabled)
         return;
      auto pending = std::make_shared<Pending>();
      pending->archive =
          DznPsoCapture::Capture(*static_cast<const D3D12_PIPELINE_STATE_STREAM_DESC *>(stream));
      char key[96];
      if (hash)
         for (unsigned i = 0; i < 32; ++i)
            std::snprintf(key + i * 2, 3, "%02x", hash[i]);
      else
         std::snprintf(key, sizeof(key), "snapshot-%lu-%lu-%llu", GetCurrentProcessId(),
                       GetCurrentThreadId(), GetTickCount64());
      pending->key = key;
      std::lock_guard<std::mutex> lock(captures_mutex);
      if (captured_bytes + pending->archive.data.size() > (64u << 20))
         throw std::runtime_error("active capture budget exhausted");
      auto &old = captures[GetCurrentThreadId()];
      if (old)
         captured_bytes -= old->archive.data.size();
      old = pending;
      captured_bytes += pending->archive.data.size();
   } catch (const std::exception &e) {
      mesa_logw("[CompileDiag] pso_capture_prepare_failed tid=%u reason=%s", GetCurrentThreadId(),
                e.what());
   }
}
extern "C" void dzn_pso_capture_finish(void) {
   std::lock_guard<std::mutex> lock(captures_mutex);
   auto it = captures.find(GetCurrentThreadId());
   if (it != captures.end()) {
      captured_bytes -= it->second->archive.data.size();
      captures.erase(it);
   }
}
extern "C" MESA_UWP_EXPORT int mesa_uwp_dump_pso(unsigned thread, char *path, size_t capacity) {
   try {
      std::shared_ptr<Pending> p;
      {
         std::unique_lock<std::mutex> lock(captures_mutex, std::try_to_lock);
         if (!lock.owns_lock())
            return 2;
         auto it = captures.find(thread);
         if (it == captures.end())
            return 1;
         p = it->second;
      }
      std::unique_lock<std::mutex> save(p->save_mutex, std::try_to_lock);
      if (!save.owns_lock())
         return 2;
      const std::string base = p->saved ? std::string{} : CachePath() + "/dozen-pso-captures";
      const std::string dir = p->saved ? p->directory : base + "/" + p->key;
      if (!path || dir.size() + 1 > capacity)
         return -1;
      if (!p->saved) {
         if (!CreateDirectoryW(Wide(base).c_str(), nullptr) &&
             GetLastError() != ERROR_ALREADY_EXISTS)
            throw std::runtime_error("capture directory failed");
         if (!CreateDirectoryW(Wide(dir).c_str(), nullptr) &&
             GetLastError() != ERROR_ALREADY_EXISTS)
            throw std::runtime_error("capture directory failed");
         auto &a = p->archive;
         std::vector<uint8_t> file(sizeof(a.header) +
                                   a.fixups.size() * sizeof(DznPsoCapture::Fixup) + a.data.size());
         std::memcpy(file.data(), &a.header, sizeof(a.header));
         std::memcpy(file.data() + sizeof(a.header), a.fixups.data(),
                     a.fixups.size() * sizeof(DznPsoCapture::Fixup));
         std::memcpy(file.data() + sizeof(a.header) +
                         a.fixups.size() * sizeof(DznPsoCapture::Fixup),
                     a.data.data(), a.data.size());
         Write(dir + "/pso.bin.tmp", file.data(), file.size());
         if (!MoveFileExW(Wide(dir + "/pso.bin.tmp").c_str(), Wide(dir + "/pso.bin").c_str(),
                          MOVEFILE_REPLACE_EXISTING))
            throw std::runtime_error("capture commit failed");
         unsigned root = 0;
         for (auto &f : a.fixups)
            if (f.root)
               Write(dir + "/root-" + std::to_string(root++) + ".rootsig", a.data.data() + f.target,
                     size_t(f.bytes));
         DznPsoCapture::Walk(a.data.data(), size_t(a.header.stream_bytes),
                             [&](D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, size_t off) {
                                if (type >= D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS &&
                                    type <= D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS)
                                   for (auto &f : a.fixups)
                                      if (f.slot == off)
                                         Write(dir + "/shader-" + std::to_string(unsigned(type)) +
                                                   ".dxil",
                                               a.data.data() + f.target, size_t(f.bytes));
                             });
         std::string manifest =
             "{\"version\":1,\"capture_key\":\"" + p->key +
             "\",\"archive\":\"pso.bin\",\"pointer_bytes\":" + std::to_string(sizeof(void *)) +
             ",\"stream_bytes\":" + std::to_string(a.header.stream_bytes) +
             ",\"payload_bytes\":" + std::to_string(a.data.size()) + "}";
         Write(dir + "/capture.json", manifest.data(), manifest.size());
         p->directory = dir;
         p->saved = true;
         mesa_logw("[CompileDiag] pso_capture_saved tid=%u path=%s bytes=%zu", thread, dir.c_str(),
                   a.data.size());
      }
      std::memcpy(path, dir.c_str(), dir.size() + 1);
      return 0;
   } catch (const std::exception &e) {
      mesa_logw("[CompileDiag] pso_capture_failed tid=%u reason=%s", thread, e.what());
      return -1;
   }
}
extern "C" long dzn_pso_legacy_create(void *device, const void *desc, void **out) {
   try {
      return DznPsoCapture::Legacy(static_cast<ID3D12Device *>(device),
                                   *static_cast<const D3D12_PIPELINE_STATE_STREAM_DESC *>(desc),
                                   reinterpret_cast<ID3D12PipelineState **>(out));
   } catch (...) {
      return E_INVALIDARG;
   }
}
extern "C" MESA_UWP_EXPORT void mesa_uwp_set_pso_legacy(int enabled) {
   legacy_override.store(enabled != 0);
}

extern "C" int dzn_pso_use_legacy(void) {
   const int override_mode = legacy_override.load();
   if (override_mode >= 0)
      return override_mode;

   static const bool selected = [] {
      try {
         const auto path = CachePath() + "/dozen-pso-api.txt";
         FILE *file = _wfopen(Wide(path).c_str(), L"r");
         if (!file)
            return false;
         char mode[16] = {};
         int read = std::fscanf(file, "%15s", mode);
         std::fclose(file);
         bool legacy = read == 1 && std::strcmp(mode, "legacy") == 0;
         mesa_logw("[CompileDiag] pso_api_selection mode=%s", legacy ? "legacy" : "stream");
         return legacy;
      } catch (...) {
         return false;
      }
   }();
   return selected;
}
#endif
