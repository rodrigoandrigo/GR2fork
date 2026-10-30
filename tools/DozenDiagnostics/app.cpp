// SPDX-License-Identifier: GPL-2.0-or-later
#include "diagnostics.cpp"
#include <mutex>
#ifdef DOZEN_DIAGNOSTICS_DESKTOP
int wmain(int argc, wchar_t **argv) {
  if (argc != 4 && argc != 5)
    return 2;
  wchar_t framework[32768]{};
  if (GetEnvironmentVariableW(L"DOZEN_DIAGNOSTICS_VCLIBS", framework, 32768))
    AddDllDirectory(framework);
  std::ofstream report(std::filesystem::path{argv[3]}, std::ios::trunc);
  return DozenDiagnostics::Run(argv[1], argv[2], [&](const std::string &line) {
    report << line << '\n';
    report.flush();
    std::printf("%s\n", line.c_str());
  }, argc != 5 || std::wstring_view(argv[4]) != L"--native");
}
#else
#include <thread>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Popups.h>
#include <winrt/Windows.UI.ViewManagement.h>
using namespace winrt;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::UI::Core;
struct App : implements<App, IFrameworkViewSource, IFrameworkView> {
  CoreWindow window{nullptr};
  std::thread worker;
  bool closed{};
  IFrameworkView CreateView() { return *this; }
  void Initialize(CoreApplicationView const &view) {
    view.Activated([this](auto &&, auto &&) { window.Activate(); });
  }
  void SetWindow(CoreWindow const &value) {
    window = value;
    window.Closed([this](auto &&, auto &&) { closed = true; });
  }
  void Load(hstring const &) {}
  void Uninitialize() {
    if (worker.joinable())
      worker.join();
  }
  void Run() {
    auto dispatcher = window.Dispatcher();
    winrt::Windows::UI::ViewManagement::ApplicationView::GetForCurrentView()
        .Title(L"Dozen Diagnostics - executando");
    worker = std::thread([dispatcher] {
      init_apartment(apartment_type::multi_threaded);
      try {
        auto folder =
            winrt::Windows::Storage::ApplicationData::Current().LocalFolder();
        std::filesystem::path output{folder.Path().c_str()};
        auto report = std::make_shared<std::ofstream>(
            output / L"dozen-diagnostics.log", std::ios::trunc);
        auto lock = std::make_shared<std::mutex>();
        auto log = [report, lock](const std::string &line) {
          std::scoped_lock guard{*lock};
          *report << line << '\n';
          report->flush();
        };
        log("DEVICE_FAMILY " +
            to_string(
                winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo()
                    .DeviceFamily()));
        std::filesystem::path package{
            winrt::Windows::ApplicationModel::Package::Current()
                .InstalledLocation()
                .Path()
                .c_str()};
        auto result = DozenDiagnostics::Run(package / L"vulkan_dzn.dll",
                                            package / L"Shaders", log);
        std::wstring text =
            L"Diagnostico concluido. Codigo: " + std::to_wstring(result) +
            L"\nRelatorio: LocalState\\dozen-diagnostics.log\n\nPASS verifica "
            L"os valores devolvidos pela GPU. UNSUPPORTED nao significa PASS.";
        dispatcher.RunAsync(CoreDispatcherPriority::Normal, [text] {
          winrt::Windows::UI::ViewManagement::ApplicationView::
              GetForCurrentView()
                  .Title(L"Dozen Diagnostics - concluido");
          winrt::Windows::UI::Popups::MessageDialog dialog{
              text, L"Dozen Diagnostics"};
          dialog.ShowAsync();
        });
      } catch (const hresult_error &error) {
        auto message = std::wstring(error.message());
        dispatcher.RunAsync(CoreDispatcherPriority::Normal, [message] {
          winrt::Windows::UI::Popups::MessageDialog{message,
                                                    L"Falha do diagnostico"}
              .ShowAsync();
        });
      }
      uninit_apartment();
    });
    while (!closed)
      dispatcher.ProcessEvents(
          CoreProcessEventsOption::ProcessOneAndAllPending);
  }
};
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  init_apartment(apartment_type::multi_threaded);
  CoreApplication::Run(make<App>());
  return 0;
}
#endif
