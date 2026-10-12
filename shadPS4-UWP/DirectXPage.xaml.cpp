#include "pch.h"

#include <algorithm>
#include <ppltasks.h>
#include "DirectXPage.xaml.h"

using namespace concurrency;
using namespace Platform;
using namespace Windows::Storage;
using namespace Windows::Storage::Pickers;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml;
using namespace shadPS4_UWP;

static task<std::wstring> MountGameFolder(std::shared_ptr<ShadPS4Host> host,
                                          StorageFolder ^ folder) {
    const Platform::Agile<StorageFolder> source{folder};
    return create_task([host, source] { return host->MountFolder(source); });
}
DirectXPage::DirectXPage() {
    InitializeComponent();
    core = std::make_shared<ShadPS4Host>(swapChainPanel);
}
void DirectXPage::OnLoaded(Object ^, RoutedEventArgs ^) {
    if (initialized || initializing) {
        return;
    }
    initializing = true;
    statusText->Text = L"Inicializando Direct3D 12…";
    const std::wstring root =
        std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\shadPS4";
    const auto width = static_cast<uint32_t>(
        std::max(1.0, swapChainPanel->ActualWidth * swapChainPanel->CompositionScaleX));
    const auto height = static_cast<uint32_t>(
        std::max(1.0, swapChainPanel->ActualHeight * swapChainPanel->CompositionScaleY));
    const auto host = core;
    create_task([host, root, width, height] { host->Initialize(root, width, height); })
        .then(
            [this](task<void> result) {
                initializing = false;
                try {
                    result.get();
                    statusText->Text = L"Direct3D 12 pronto. A execução de jogos "
                                       L"ainda não está disponível.";
                    initialized = true;
                    openButton->IsEnabled = true;
                    RestoreGameFolder();
                    Windows::UI::Xaml::Media::CompositionTarget::Rendering +=
                        ref new Windows::Foundation::EventHandler<Object ^>(
                            this, &DirectXPage::OnRendering);
                } catch (Exception ^ exception) {
                    statusText->Text = exception->Message;
                } catch (const std::exception&) {
                    statusText->Text = L"Falha ao inicializar a DLL.";
                }
            },
            task_continuation_context::use_current());
}

void DirectXPage::OnRendering(Object ^, Object ^) {
    if (initialized && !core->Render()) {
        initialized = false;
        statusText->Text = ref new String(core->Error().c_str());
    }
}
void DirectXPage::OnOpenGame(Object ^, RoutedEventArgs ^) {
    openButton->IsEnabled = false;
    auto picker = ref new FolderPicker();
    picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
    picker->FileTypeFilter->Append(L"*");
    create_task(picker->PickSingleFolderAsync())
        .then([this](StorageFolder ^ source) -> task<std::wstring> {
            if (!source) {
                return task_from_result(std::wstring{});
            }
            Windows::Storage::AccessCache::StorageApplicationPermissions::FutureAccessList
                ->AddOrReplace(L"shadps4-game", source);
            statusText->Text = L"Vinculando pasta…";
            return MountGameFolder(core, source);
        })
        .then(
            [this](task<std::wstring> result) {
                openButton->IsEnabled = true;
                try {
                    const auto message = result.get();
                    if (!message.empty()) {
                        statusText->Text = ref new String(message.c_str());
                    }
                } catch (Exception ^ exception) {
                    statusText->Text = exception->Message;
                }
            },
            task_continuation_context::use_current());
}

void DirectXPage::RestoreGameFolder() {
    const auto access =
        Windows::Storage::AccessCache::StorageApplicationPermissions::FutureAccessList;
    if (!access->ContainsItem(L"shadps4-game")) {
        return;
    }
    openButton->IsEnabled = false;
    create_task(access->GetFolderAsync(L"shadps4-game"))
        .then([this](StorageFolder ^ source) { return MountGameFolder(core, source); })
        .then(
            [this](task<std::wstring> result) {
                openButton->IsEnabled = true;
                try {
                    statusText->Text = ref new String(result.get().c_str());
                } catch (Exception ^ exception) {
                    statusText->Text = exception->Message;
                }
            },
            task_continuation_context::use_current());
}
void DirectXPage::OnPause(Object ^, RoutedEventArgs ^) {
    paused = !paused;
    core->Pause(paused);
    pauseButton->Content = paused ? L"Continuar" : L"Pausar";
}
void DirectXPage::OnStop(Object ^, RoutedEventArgs ^) {
    core->Stop();
}
void DirectXPage::OnPanelSizeChanged(Object ^, SizeChangedEventArgs ^) {
    if (core)
        core->Resize(static_cast<uint32_t>(std::max(1.0, swapChainPanel->ActualWidth *
                                                             swapChainPanel->CompositionScaleX)),
                     static_cast<uint32_t>(std::max(1.0, swapChainPanel->ActualHeight *
                                                             swapChainPanel->CompositionScaleY)));
}
void DirectXPage::SaveInternalState(Windows::Foundation::Collections::IPropertySet ^) {
    core->Pause(true);
}
void DirectXPage::LoadInternalState(Windows::Foundation::Collections::IPropertySet ^) {
    core->Pause(paused);
}
