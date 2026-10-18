#include "pch.h"

#include "DirectXPage.xaml.h"
#include <algorithm>
#include <ppltasks.h>
using namespace concurrency;
using namespace Platform;
using namespace Windows::Storage;
using namespace Windows::Storage::Pickers;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml;
using namespace shadPS4_UWP;
namespace {
std::wstring Wide(const std::string& text) {
    const auto length = MultiByteToWideChar(CP_UTF8, 0, text.data(), text.size(), nullptr, 0);
    std::wstring output(length, 0);
    MultiByteToWideChar(CP_UTF8, 0, text.data(), text.size(), output.data(), length);
    return output;
}
std::string Utf8(const std::wstring& text) {
    const auto length =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), text.size(), nullptr, 0, nullptr, nullptr);
    std::string output(length, 0);
    WideCharToMultiByte(CP_UTF8, 0, text.data(), text.size(), output.data(), length, nullptr,
                        nullptr);
    return output;
}
} // namespace
DirectXPage::DirectXPage() {
    InitializeComponent();
}
void DirectXPage::OnLoaded(Object ^, RoutedEventArgs ^) {
    if (frontend)
        return;
    frontend = std::make_shared<ImGuiFrontend>(swapChainPanel, gameSwapChainPanel);
    WeakReference weak(this);
    const auto dispatch = Dispatcher;
    frontend->choose_folder = [weak, dispatch](std::wstring purpose) {
        dispatch->RunAsync(CoreDispatcherPriority::Normal,
                           ref new DispatchedHandler([weak, purpose] {
                               if (auto page = weak.Resolve<DirectXPage>())
                                   page->PickFolder(purpose);
                           }));
    };
    frontend->edit_text = [weak, dispatch](std::string label, std::string value, bool secret,
                                           std::function<void(std::string)> completed) {
        dispatch->RunAsync(CoreDispatcherPriority::Normal,
                           ref new DispatchedHandler([weak, label, value, secret, completed] {
                               if (auto page = weak.Resolve<DirectXPage>())
                                   page->EditText(label, value, secret, completed);
                           }));
    };
    frontend->ready = [weak, dispatch] {
        dispatch->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([weak] {
                               if (auto page = weak.Resolve<DirectXPage>())
                                   page->statusText->Visibility =
                                       Windows::UI::Xaml::Visibility::Collapsed;
                           }));
    };
    frontend->failure = [weak, dispatch](std::wstring message) {
        dispatch->RunAsync(
            CoreDispatcherPriority::Normal, ref new DispatchedHandler([weak, message] {
                if (auto page = weak.Resolve<DirectXPage>()) {
                    page->statusText->Text = ref new String(message.c_str());
                    page->statusText->Visibility = Windows::UI::Xaml::Visibility::Visible;
                }
            }));
    };
    window = CoreWindow::GetForCurrentThread();
    activation_token = window->Activated +=
        ref new Windows::Foundation::TypedEventHandler < CoreWindow ^
        , WindowActivatedEventArgs ^
              > ([weak](CoreWindow ^, WindowActivatedEventArgs ^ args) {
                    if (auto page = weak.Resolve<DirectXPage>())
                        if (page->frontend)
                            page->frontend->UpdateInput([&](FrontendInput& input) {
                                input.active = args->WindowActivationState !=
                                               CoreWindowActivationState::Deactivated;
                                if (!input.active) {
                                    input.keys.fill(false);
                                    input.characters.clear();
                                    input.left = input.right = false;
                                    input.wheel = 0;
                                }
                            });
                });
    down_token = window->KeyDown += ref new Windows::Foundation::TypedEventHandler < CoreWindow ^,
    KeyEventArgs ^ > ([weak](CoreWindow ^, KeyEventArgs ^ args) {
                       if (args->VirtualKey == Windows::System::VirtualKey::GamepadB)
                           args->Handled = true;
                       if (auto page = weak.Resolve<DirectXPage>())
                           if (page->frontend)
                               page->frontend->UpdateInput([&](FrontendInput& input) {
                                   const auto key = static_cast<unsigned>(args->VirtualKey);
                                   if (!input.captured && key < input.keys.size())
                                       input.keys[key] = true;
                               });
                   });
    up_token = window->KeyUp += ref new Windows::Foundation::TypedEventHandler < CoreWindow ^,
    KeyEventArgs ^ > ([weak](CoreWindow ^, KeyEventArgs ^ args) {
                       if (args->VirtualKey == Windows::System::VirtualKey::GamepadB)
                           args->Handled = true;
                       if (auto page = weak.Resolve<DirectXPage>())
                           if (page->frontend)
                               page->frontend->UpdateInput([&](FrontendInput& input) {
                                   const auto key = static_cast<unsigned>(args->VirtualKey);
                                   if (key < input.keys.size())
                                       input.keys[key] = false;
                               });
                   });
    character_token = window->CharacterReceived +=
        ref new Windows::Foundation::TypedEventHandler < CoreWindow ^
        ,
    CharacterReceivedEventArgs ^ > ([weak](CoreWindow ^, CharacterReceivedEventArgs ^ args) {
                                     if (auto page = weak.Resolve<DirectXPage>())
                                         if (page->frontend)
                                             page->frontend->UpdateInput([&](FrontendInput& input) {
                                                 if (!input.captured && args->KeyCode >= 32 &&
                                                     input.characters.size() < 512)
                                                     input.characters.push_back(args->KeyCode);
                                             });
                                 });
    try {
        display_information = Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
        color_token = display_information->AdvancedColorInfoChanged +=
            ref new Windows::Foundation::TypedEventHandler<Windows::Graphics::Display::DisplayInformation ^, Object ^>(
                [weak](Windows::Graphics::Display::DisplayInformation ^ sender, Object ^ args) {
                    if (auto page = weak.Resolve<DirectXPage>())
                        page->OnAdvancedColorChanged(sender, args);
                });
        OnAdvancedColorChanged(display_information, nullptr);
    } catch (Platform::Exception ^) {
        display_information = nullptr;
    }
    OnPanelSizeChanged(nullptr, nullptr);
    frontend->Start(std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) +
                    L"\\shadPS4");
    const auto access =
        Windows::Storage::AccessCache::StorageApplicationPermissions::FutureAccessList;
    nlohmann::json records = nlohmann::json::array();
    const auto values = ApplicationData::Current->LocalSettings->Values;
    try {
        if (values->HasKey(L"shadps4.library.roots"))
            records = nlohmann::json::parse(
                Utf8(safe_cast<String ^>(values->Lookup(L"shadps4.library.roots"))->Data()));
        else if (access->ContainsItem(L"shadps4-game"))
            records.push_back({{"token", "shadps4-game"}, {"enabled", true}});
    } catch (...) {
        records = nlohmann::json::array();
    }
    auto roots = std::make_shared<std::vector<FrontendRoot>>();
    task<void> restored = task_from_result();
    if (records.is_array())
        for (const auto& record : records) {
            if (!record.is_object() || !record.contains("token") || !record.at("token").is_string())
                continue;
            const auto token = record.at("token").get<std::string>();
            const bool enabled = record.value("enabled", true);
            restored = restored.then([access, token, enabled, roots] {
                return create_task(access->GetFolderAsync(ref new String(Wide(token).c_str())))
                    .then([token, enabled, roots](task<StorageFolder ^> result) {
                        try {
                            roots->push_back(
                                {Platform::Agile<StorageFolder>{result.get()}, token, enabled});
                        } catch (Exception ^) {
                        }
                    });
            });
        }
    restored.then(
        [weak, roots] {
            if (auto page = weak.Resolve<DirectXPage>())
                if (page->frontend)
                    page->frontend->SetFolders(*roots);
        },
        task_continuation_context::use_current());
}
void DirectXPage::OnUnloaded(Object ^, RoutedEventArgs ^) {
    if (display_information) {
        display_information->AdvancedColorInfoChanged -= color_token;
        display_information = nullptr;
    }
    if (window) {
        window->KeyDown -= down_token;
        window->KeyUp -= up_token;
        window->CharacterReceived -= character_token;
        window->Activated -= activation_token;
        window = nullptr;
    }
    if (frontend) {
        frontend->Stop();
        frontend.reset();
    }
}
void DirectXPage::Capture(bool captured) {
    if (frontend)
        frontend->UpdateInput([&](FrontendInput& input) {
            input.captured = captured;
            input.keys.fill(false);
            input.characters.clear();
            input.left = input.right = false;
            input.wheel = 0;
        });
}
void DirectXPage::OnPointer(Object ^, Windows::UI::Xaml::Input::PointerRoutedEventArgs ^ args) {
    if (!frontend)
        return;
    const auto point = args->GetCurrentPoint(swapChainPanel);
    const auto scale_x = swapChainPanel->CompositionScaleX,
               scale_y = swapChainPanel->CompositionScaleY;
    frontend->UpdateInput([&](FrontendInput& input) {
        if (input.captured)
            return;
        input.x = point->Position.X * scale_x;
        input.y = point->Position.Y * scale_y;
        input.left = point->Properties->IsLeftButtonPressed;
        input.right = point->Properties->IsRightButtonPressed;
        input.wheel += point->Properties->MouseWheelDelta / 120.0f;
    });
}
void DirectXPage::OnPointerLost(Object ^, Windows::UI::Xaml::Input::PointerRoutedEventArgs ^) {
    if (frontend)
        frontend->UpdateInput([](FrontendInput& input) {
            input.left = input.right = false;
            input.x = input.y = -FLT_MAX;
        });
}
void DirectXPage::OnAdvancedColorChanged(Windows::Graphics::Display::DisplayInformation ^ sender, Object ^) {
    bool hdr = false;
    float white_nits = 80.0f;
    try {
        const auto info = sender->GetAdvancedColorInfo();
        white_nits = info->SdrWhiteLevelInNits;
        hdr = info->CurrentAdvancedColorKind ==
              Windows::Graphics::Display::AdvancedColorKind::HighDynamicRange;
    } catch (Platform::Exception ^) {
    }
    if (frontend)
        frontend->UpdateInput([&](FrontendInput& input) {
            input.hdr_output = hdr;
            input.sdr_white_nits = white_nits;
        });
}
void DirectXPage::OnCompositionScaleChanged(Controls::SwapChainPanel ^, Object ^) {
    OnPanelSizeChanged(nullptr, nullptr);
}
void DirectXPage::OnPanelSizeChanged(Object ^, SizeChangedEventArgs ^) {
    if (frontend)
        frontend->UpdateInput([&](FrontendInput& input) {
            input.scale_x = swapChainPanel->CompositionScaleX;
            input.scale_y = swapChainPanel->CompositionScaleY;
            input.width = static_cast<uint32_t>(
                std::max(1.0, swapChainPanel->ActualWidth * swapChainPanel->CompositionScaleX));
            input.height = static_cast<uint32_t>(
                std::max(1.0, swapChainPanel->ActualHeight * swapChainPanel->CompositionScaleY));
        });
}
void DirectXPage::PickFolder(std::wstring purpose) {
    Capture(true);
    auto picker = ref new FolderPicker();
    picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
    picker->FileTypeFilter->Append(L"*");
    WeakReference weak(this);
    create_task(picker->PickSingleFolderAsync())
        .then(
            [weak, purpose](task<StorageFolder ^> result) {
                if (auto page = weak.Resolve<DirectXPage>()) {
                    page->Capture(false);
                    try {
                        auto folder = result.get();
                        if (!folder || !page->frontend)
                            return;
                        if (purpose == L"library") {
                            const auto token =
                                Windows::Storage::AccessCache::StorageApplicationPermissions::
                                    FutureAccessList->Add(folder);
                            page->frontend->SetFolder(Platform::Agile<StorageFolder>{folder},
                                                      Utf8(token->Data()));
                        } else
                            page->frontend->ImportFolder(Platform::Agile<StorageFolder>{folder},
                                                         purpose);
                    } catch (Exception ^ error) {
                        page->statusText->Visibility = Windows::UI::Xaml::Visibility::Visible;
                        page->statusText->Text = error->Message;
                    }
                }
            },
            task_continuation_context::use_current());
}
void DirectXPage::EditText(std::string label, std::string value, bool secret,
                           std::function<void(std::string)> completed) {
    Capture(true);
    auto dialog = ref new Controls::ContentDialog();
    dialog->Title = ref new String(Wide(label).c_str());
    dialog->PrimaryButtonText = L"Aplicar";
    dialog->CloseButtonText = L"Cancelar";
    auto text = ref new Controls::TextBox();
    text->Text = ref new String(Wide(value).c_str());
    dialog->Content = text;
    WeakReference weak(this);
    create_task(dialog->ShowAsync())
        .then(
            [weak, text, completed](task<Controls::ContentDialogResult> result) {
                if (auto page = weak.Resolve<DirectXPage>())
                    page->Capture(false);
                try {
                    if (result.get() == Controls::ContentDialogResult::Primary)
                        completed(Utf8(text->Text->Data()));
                } catch (Exception ^) {
                }
            },
            task_continuation_context::use_current());
}
void DirectXPage::SaveInternalState(Windows::Foundation::Collections::IPropertySet ^) {
    if (frontend)
        frontend->UpdateInput([](FrontendInput& input) {
            input.suspended = true;
            input.keys.fill(false);
            input.left = input.right = false;
        });
}
void DirectXPage::LoadInternalState(Windows::Foundation::Collections::IPropertySet ^) {
    if (frontend)
        frontend->UpdateInput([](FrontendInput& input) { input.suspended = false; });
}
