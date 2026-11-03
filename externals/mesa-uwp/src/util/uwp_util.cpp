#define _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS

#include "uwp_util.h"

#include <atomic>
#include <array>
#include "util/log.h"
#include <cstring>
#include <cstdio>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <future>
#include <memory>
#include <unknwn.h>
#include <roapi.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Graphics.Display.Core.h>

using namespace winrt;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::System::Profile;
using namespace winrt::Windows::Graphics::Display;
using namespace winrt::Windows::Graphics::Display::Core;

static bool _uwpWindowReferenceFailed = false;
static Microsoft::WRL::ComPtr<IAgileReference> _uwpWindowReference;
static std::mutex _uwpWindowReferenceMutex;
static mesa_uwp_swapchain_attach_callback _uwpSwapchainAttach = nullptr;
static void *_uwpSwapchainAttachOpaque = nullptr;
static unsigned long long dimensions_generation = 0;
static int current_height = -1;
static int current_width = -1;
static std::atomic<bool> offscreen_drawable{false};

extern "C" int uwp_get_cache_directory(char *path, size_t capacity)
{
   const HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
   int result = 0;
   try {
      const auto folder = winrt::Windows::Storage::ApplicationData::Current().LocalCacheFolder();
      const auto text = winrt::to_string(folder.Path());
      if (text.size() < capacity) {
         memcpy(path, text.c_str(), text.size() + 1);
         result = 1;
      }
   } catch (...) {
   }
   if (SUCCEEDED(initialized))
      RoUninitialize();
   return result;
}

extern "C" int uwp_is_desktop_device(void)
{
   const HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
   int result = 0;
   try {
      result = AnalyticsInfo::VersionInfo().DeviceFamily() == L"Windows.Desktop";
   } catch (...) {
   }
   if (SUCCEEDED(initialized))
      RoUninitialize();
   return result;
}

extern "C" void mesa_uwp_set_offscreen(int enabled)
{
   offscreen_drawable.store(enabled != 0);
}

extern "C" int mesa_uwp_is_offscreen(void)
{
   return offscreen_drawable.load();
}

extern "C" void* uwp_get_window_reference(void)
{
   /* The XAML panel is supplied by the UI apartment, while Mesa creates its
    * framebuffer on the emulator thread. Resolve an apartment-correct COM
    * proxy on every calling thread instead of sharing the raw XAML pointer. */
   thread_local Microsoft::WRL::ComPtr<IUnknown> resolved_window;
   Microsoft::WRL::ComPtr<IAgileReference> agile_reference;
   {
      std::lock_guard<std::mutex> lock(_uwpWindowReferenceMutex);
      agile_reference = _uwpWindowReference;
      if (_uwpWindowReferenceFailed)
         return nullptr;
   }
   if (agile_reference) {
      resolved_window.Reset();
      if (SUCCEEDED(agile_reference->Resolve(IID_PPV_ARGS(&resolved_window))))
         return resolved_window.Get();
      return nullptr;
   }

   thread_local CoreWindow window = nullptr;
   try {
      window = CoreWindow::GetForCurrentThread();
   } catch (...) {
      return nullptr;
   }
   if (!window)
      return nullptr;

   return winrt::get_abi(window);
}

extern "C" void uwp_set_window_reference(void *window, int width, int height)
{
   IUnknown *reference = static_cast<IUnknown *>(window);
   Microsoft::WRL::ComPtr<IAgileReference> agile_reference;
   HRESULT result = S_OK;
   if (reference) {
      result = RoGetAgileReference(AGILEREFERENCE_DEFAULT, IID_IUnknown, reference,
                                   &agile_reference);
   }
   {
      std::lock_guard<std::mutex> lock(_uwpWindowReferenceMutex);
      _uwpWindowReference = agile_reference;
      _uwpWindowReferenceFailed = FAILED(result);
      current_width = width;
      current_height = height;
      ++dimensions_generation;
   }
}

extern "C" void mesa_uwp_set_swapchain_attach_callback(
   mesa_uwp_swapchain_attach_callback callback, void *opaque)
{
   std::lock_guard<std::mutex> lock(_uwpWindowReferenceMutex);
   _uwpSwapchainAttach = callback;
   _uwpSwapchainAttachOpaque = opaque;
}

extern "C" long mesa_uwp_attach_swapchain(void *swapchain)
{
   mesa_uwp_swapchain_attach_callback callback;
   void *opaque;
   {
      std::lock_guard<std::mutex> lock(_uwpWindowReferenceMutex);
      callback = _uwpSwapchainAttach;
      opaque = _uwpSwapchainAttachOpaque;
   }
   return callback ? callback(opaque, swapchain) : E_NOTIMPL;
}

/* Query WinRT display objects only on the UI apartment. Do not retain these
 * apartment-bound objects globally, and never pump UI events on a worker. */
static void uwp_query_dimensions(CoreWindow const &window,
                                 unsigned long long generation)
{
   int width = -1, height = -1;
   if (AnalyticsInfo::VersionInfo().DeviceFamily() == L"Windows.Xbox") {
      auto hdmi = HdmiDisplayInformation::GetForCurrentView();
      if (hdmi) {
         auto mode = hdmi.GetCurrentDisplayMode();
         if (mode) {
            width = static_cast<int>(mode.ResolutionWidthInRawPixels());
            height = static_cast<int>(mode.ResolutionHeightInRawPixels());
         }
      }
   }
   if (width < 0 || height < 0) {
      auto display = DisplayInformation::GetForCurrentView();
      float scale = static_cast<int>(display.ResolutionScale()) / 100.0f;
      auto bounds = window.Bounds();
      width = static_cast<int>(bounds.Width * scale);
      height = static_cast<int>(bounds.Height * scale);
   }
   std::lock_guard<std::mutex> lock(_uwpWindowReferenceMutex);
   /* A delayed query must not overwrite a newer size supplied by the host. */
   if (generation == dimensions_generation) {
      if (current_width == -1)
         current_width = width;
      if (current_height == -1)
         current_height = height;
   }
}

static int uwp_get_dimension(bool height)
{
   unsigned long long generation;
   {
      std::lock_guard<std::mutex> lock(_uwpWindowReferenceMutex);
      int value = height ? current_height : current_width;
      if (value != -1)
         return value;
      generation = dimensions_generation;
   }
   try {
      auto window = CoreApplication::MainView().CoreWindow();
      auto dispatcher = window.Dispatcher();
      if (dispatcher.HasThreadAccess()) {
         uwp_query_dimensions(window, generation);
      } else {
         /* Shared ownership keeps the completion alive if the UI callback
          * runs after the bounded wait has expired. */
         auto completion = std::make_shared<std::promise<void>>();
         auto finished = completion->get_future();
         dispatcher.RunAsync(CoreDispatcherPriority::Normal,
            DispatchedHandler([completion, generation]() {
               try {
                  uwp_query_dimensions(CoreWindow::GetForCurrentThread(), generation);
               } catch (...) {
                  /* No C++ exception may escape the dispatcher callback. */
               }
               completion->set_value();
            }));
         finished.wait_for(std::chrono::seconds(1));
      }
   } catch (...) {
      /* Missing views, shutdown and WinRT errors leave the size unknown.
       * Preserve the C ABI and let the host supply dimensions or retry. */
   }
   std::lock_guard<std::mutex> lock(_uwpWindowReferenceMutex);
   return height ? current_height : current_width;
}

extern "C" int uwp_get_height(void)
{
   return uwp_get_dimension(true);
}

extern "C" int uwp_get_width(void)
{
   return uwp_get_dimension(false);
}

static std::mutex shader_compile_mutex;
static std::condition_variable shader_compile_cv;
static unsigned active_shader_compiles;
static thread_local unsigned shader_compile_depth;

static bool shader_compile_memory_pressure()
{
   try {
      const auto used = winrt::Windows::System::MemoryManager::AppMemoryUsage();
      const auto limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
      return limit && (used >= limit || limit - used < (512ull << 20));
   } catch (...) {
      return true;
   }
}

extern "C" int uwp_shader_compile_begin(void)
{
   static const bool desktop = uwp_is_desktop_device();
   if (desktop)
      return 0;
   if (shader_compile_depth) {
      ++shader_compile_depth;
      return 1;
   }
   uwp_compile_diagnostic_phase("gate_mutex", -1);
   std::unique_lock lock(shader_compile_mutex);
   uwp_compile_diagnostic_phase("gate_memory", -1);
   shader_compile_cv.wait(lock, [] {
      return !active_shader_compiles || !shader_compile_memory_pressure();
   });
   ++active_shader_compiles;
   uwp_compile_diagnostic_phase("gate_admitted", -1);
   shader_compile_depth = 1;
   return 1;
}

extern "C" void uwp_shader_compile_end(void)
{
   if (--shader_compile_depth)
      return;
   {
      std::lock_guard lock(shader_compile_mutex);
      --active_shader_compiles;
   }
   shader_compile_cv.notify_all();
}

static std::mutex native_pso_mutex;
static std::condition_variable native_pso_cv;
static unsigned admitted_native_psos;
static std::atomic<unsigned> native_pso_limit;
static std::once_flag native_pso_configuration;

static void configure_native_pso_concurrency()
{
   unsigned limit = 0;
   char directory[4096];
   if (uwp_get_cache_directory(directory, sizeof(directory))) {
      char path[4200];
      std::snprintf(path, sizeof(path), "%s/dozen-pso-concurrency.txt", directory);
      if (FILE *file = std::fopen(path, "r")) {
         unsigned requested;
         if (std::fscanf(file, "%u", &requested) == 1 && requested <= 4)
            limit = requested;
         std::fclose(file);
      }
   }
   native_pso_limit.store(limit);
   mesa_logw("[CompileDiag] native_pso_configuration limit=%u (0=unlimited)", limit);
}

extern "C" MESA_UWP_EXPORT void mesa_uwp_set_native_pso_concurrency(unsigned limit)
{
   std::call_once(native_pso_configuration, configure_native_pso_concurrency);
   {
      std::lock_guard lock(native_pso_mutex);
      native_pso_limit.store(limit <= 4 ? limit : 4);
   }
   native_pso_cv.notify_all();
}

extern "C" void uwp_native_pso_begin(void)
{
   std::call_once(native_pso_configuration, configure_native_pso_concurrency);
   uwp_compile_diagnostic_phase("native_pso_slot", -1);
   std::unique_lock lock(native_pso_mutex);
   native_pso_cv.wait(lock, [] {
      const auto limit = native_pso_limit.load();
      return !limit || admitted_native_psos < limit;
   });
   ++admitted_native_psos;
}

extern "C" void uwp_native_pso_end(void)
{
   {
      std::lock_guard lock(native_pso_mutex);
      --admitted_native_psos;
   }
   native_pso_cv.notify_one();
}

extern "C" uint64_t uwp_compile_diagnostic_begin(const char *scope)
{
   return 0;
}

extern "C" void uwp_compile_diagnostic_phase(const char *phase, int stage)
{
}

extern "C" void uwp_compile_diagnostic_pso(const char *json)
{
}

extern "C" void uwp_compile_diagnostic_native_pso(int begin, int64_t result)
{
   if (!begin && result < 0)
      mesa_loge("Dozen native PSO creation failed result=0x%llx", (unsigned long long)result);
}

extern "C" void uwp_compile_diagnostic_end(uint64_t id, int64_t result)
{
}

extern "C" MESA_UWP_EXPORT void mesa_uwp_report_compiles(void)
{
}

extern "C" MESA_UWP_EXPORT int mesa_uwp_get_compile_diagnostic(unsigned thread, char *json, size_t capacity)
{
   if (!json || !capacity)
      return -1;
   const int length = std::snprintf(json, capacity, "{\"active\":false}");
   return length >= 0 && (size_t)length < capacity ? 0 : -1;
}
