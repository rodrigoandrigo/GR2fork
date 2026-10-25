#include "pch.h"

#include "DirectXPage.xaml.h"
#include "UwpDiagnostics.h"
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
    UwpDiagnostics::Record("main_page_xaml_initializing");
    try {
        InitializeComponent();
        UwpDiagnostics::Record("main_page_xaml_initialized");
    } catch (Exception^ error) {
        UwpDiagnostics::Write(UwpDiagnostics::crash_file, "main_page_xaml_failed hresult", static_cast<uint32_t>(error->HResult));
        UwpDiagnostics::Text(UwpDiagnostics::crash_file, "main_page_xaml_message", error->Message->Data());
        throw;
    }
}
void DirectXPage::OnLoaded(Object ^, RoutedEventArgs ^) {
    UwpDiagnostics::Record("main_page_loaded");
    if (frontend)
        return;
    frontend = std::make_shared<ImGuiFrontend>(swapChainPanel, gameSwapChainPanel);
    WeakReference weak(this);
    const auto dispatch = Dispatcher;
    frontend->set_pointer_mode = [weak, dispatch](bool visible) {
        dispatch->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([weak, visible] {
            if (auto page = weak.Resolve<DirectXPage>())
                if (page->frontend)
                    page->SetPointerMode(visible);
        }));
    };
    frontend->toggle_fullscreen = [dispatch] {
        dispatch->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([] {
            const auto view = Windows::UI::ViewManagement::ApplicationView::GetForCurrentView();
            if (view->IsFullScreenMode)
                view->ExitFullScreenMode();
            else
                view->TryEnterFullScreenMode();
        }));
    };
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
    frontend->game_text_input = [weak, dispatch](ShadPS4TextInput request,
                                                std::function<void(bool, std::wstring)> completed) {
        dispatch->RunAsync(CoreDispatcherPriority::Normal,
                          ref new DispatchedHandler([weak, request, completed] {
            if (auto page = weak.Resolve<DirectXPage>())
                page->ShowGameTextInput(request, completed);
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
                    page->SetPointerMode(true);
                }
            }));
    };
    window = CoreWindow::GetForCurrentThread();
    visibility_token = window->VisibilityChanged +=
        ref new Windows::Foundation::TypedEventHandler<CoreWindow ^, VisibilityChangedEventArgs ^>(
            [weak](CoreWindow ^, VisibilityChangedEventArgs ^ args) {
                if (auto page = weak.Resolve<DirectXPage>())
                    if (page->frontend)
                        page->frontend->UpdateInput(
                            [&](FrontendInput& input) { input.visible = args->Visible; });
            });
    activation_token = window->Activated +=
        ref new Windows::Foundation::TypedEventHandler < CoreWindow ^
        , WindowActivatedEventArgs ^
              > ([weak](CoreWindow ^, WindowActivatedEventArgs ^ args) {
                    if (auto page = weak.Resolve<DirectXPage>()) {
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
                        if (args->WindowActivationState != CoreWindowActivationState::Deactivated)
                            page->SetPointerMode(page->pointer_visible);
                    }
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
                return create_task(access->GetItemAsync(ref new String(Wide(token).c_str())))
                    .then([token, enabled, roots](task<IStorageItem ^> result) {
                        try {
                            auto item = result.get();
                            if (item->IsOfType(StorageItemTypes::Folder))
                                roots->push_back({Platform::Agile<StorageFolder>{safe_cast<StorageFolder^>(item)}, token, enabled});
                            else if (item->IsOfType(StorageItemTypes::File))
                                roots->push_back({{}, token, enabled, Platform::Agile<StorageFile>{safe_cast<StorageFile^>(item)}});
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
    dispatch->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([weak] {
        if (auto page = weak.Resolve<DirectXPage>())
            if (page->frontend)
                page->SetPointerMode(page->pointer_visible);
    }));
}
void DirectXPage::OnUnloaded(Object ^, RoutedEventArgs ^) {
    game_text_request_id = 0;
    if (game_text_dialog)
        game_text_dialog->Hide();
    if (display_information) {
        display_information->AdvancedColorInfoChanged -= color_token;
        display_information = nullptr;
    }
    if (window) {
        window->KeyDown -= down_token;
        window->KeyUp -= up_token;
        window->CharacterReceived -= character_token;
        window->Activated -= activation_token;
        window->VisibilityChanged -= visibility_token;
        window = nullptr;
    }
    if (frontend) {
        frontend->Stop();
        frontend.reset();
    }
}
void DirectXPage::Capture(bool captured) {
    dialog_open = captured;
    if (frontend)
        frontend->UpdateInput([&](FrontendInput& input) {
            input.captured = captured;
            input.keys.fill(false);
            input.characters.clear();
            input.left = input.right = false;
            input.wheel = 0;
        });
    if (!captured)
        SetPointerMode(pointer_visible);
}
void DirectXPage::SetPointerMode(bool visible) {
    pointer_visible = visible;
    if (Windows::System::Profile::AnalyticsInfo::VersionInfo->DeviceFamily != "Windows.Xbox")
        return;
    interfacePage->IsHitTestVisible = visible;
    interfacePage->IsTabStop = visible;
    interfacePage->IsEnabled = visible;
    if (dialog_open)
        return;
    bool focused = false;
    try {
        focused = visible ? interfacePage->Focus(Windows::UI::Xaml::FocusState::Programmatic)
                          : Focus(Windows::UI::Xaml::FocusState::Programmatic);
        UwpDiagnostics::Record("xbox_pointer_page visible/focused", visible, focused);
    } catch (Exception^ error) {
        UwpDiagnostics::Record("xbox_pointer_page_failed hresult", static_cast<uint32_t>(error->HResult));
        UwpDiagnostics::Text(UwpDiagnostics::session_file, "xbox_pointer_page_message", error->Message->Data());
    }
    if (frontend)
        frontend->UpdateInput([visible, focused](FrontendInput& input) {
            input.system_pointer = visible && focused;
            input.left = input.right = false;
            input.wheel = 0;
            input.x = input.y = -10000;
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
    if (purpose == L"pkg") { PickPackage(); return; }
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
void DirectXPage::PickPackage() {
    Capture(true);
    auto picker = ref new FileOpenPicker();
    picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
    picker->FileTypeFilter->Append(L".pkg");
    WeakReference weak(this);
    create_task(picker->PickSingleFileAsync()).then([weak](task<StorageFile^> result) {
        if (auto page = weak.Resolve<DirectXPage>()) {
            page->Capture(false);
            try {
                auto file = result.get();
                if (!file || !page->frontend) return;
                const auto token = Windows::Storage::AccessCache::StorageApplicationPermissions::FutureAccessList->Add(file);
                page->frontend->SetPackage(Platform::Agile<StorageFile>{file}, Utf8(token->Data()));
            } catch (Exception^ error) {
                page->statusText->Visibility = Windows::UI::Xaml::Visibility::Visible;
                page->statusText->Text = error->Message;
            }
        }
    }, task_continuation_context::use_current());
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
void DirectXPage::ShowGameTextInput(ShadPS4TextInput request,
                                    std::function<void(bool, std::wstring)> completed) {
    game_text_request_id = request.id;
    if (!request.id) {
        if (game_text_dialog)
            game_text_dialog->Hide();
        return;
    }
    if (game_text_dialog) {
        auto previous = game_text_dialog;
        game_text_dialog = nullptr;
        WeakReference weak(this);
        previous->Closed +=
            ref new Windows::Foundation::TypedEventHandler < Controls::ContentDialog ^
            , Controls::ContentDialogClosedEventArgs ^
                  > ([weak, request, completed](auto, auto) {
                        if (auto page = weak.Resolve<DirectXPage>()) {
                            if (page->game_text_request_id == request.id)
                                page->ShowGameTextInput(request, completed);
                        }
                    });
        previous->Hide();
        return;
    }
    Capture(true);
    auto dialog = ref new Controls::ContentDialog();
    game_text_dialog = dialog;
    dialog->Title = ref new String(request.title[0] ? request.title : L"Entrada de texto");
    dialog->PrimaryButtonText = L"Confirmar";
    dialog->CloseButtonText = L"Cancelar";
    auto text = ref new Controls::TextBox();
    auto password = ref new Controls::PasswordBox();
    const bool secret = (request.flags & SHADPS4_TEXT_PASSWORD) != 0;
    if (secret) {
        password->MaxLength = request.max_length;
        password->Password = ref new String(request.text);
        dialog->Content = password;
    } else {
        text->Text = ref new String(request.text);
        text->MaxLength = request.max_length;
        text->PlaceholderText = ref new String(request.placeholder);
        text->AcceptsReturn = (request.flags & SHADPS4_TEXT_MULTILINE) != 0;
        text->TextWrapping = TextWrapping::Wrap;
        auto scope = ref new Input::InputScope();
        auto name = ref new Input::InputScopeName();
        name->NameValue = (request.flags & SHADPS4_TEXT_NUMBER)
                              ? Input::InputScopeNameValue::Number
                              : Input::InputScopeNameValue::Default;
        scope->Names->Append(name);
        text->InputScope = scope;
        if (request.flags & SHADPS4_TEXT_NUMBER) {
            WeakReference dialog_weak(dialog);
            text->TextChanged +=
                ref new Controls::TextChangedEventHandler([dialog_weak](auto sender, auto) {
                    auto input = safe_cast<Controls::TextBox ^>(sender);
                    const std::wstring value = input->Text->Data();
                    if (auto target = dialog_weak.Resolve<Controls::ContentDialog>())
                        target->IsPrimaryButtonEnabled =
                            std::all_of(value.begin(), value.end(), [](wchar_t ch) {
                                return (ch >= L'0' && ch <= L'9') || ch == L',' || ch == L'-' ||
                                       ch == L'.';
                            });
                });
        }
        dialog->Content = text;
    }
    dialog->Opened += ref new Windows::Foundation::TypedEventHandler < Controls::ContentDialog ^,
        Controls::ContentDialogOpenedEventArgs ^
            > ([text, password, secret](auto, auto) {
                  if (secret)
                      password->Focus(Windows::UI::Xaml::FocusState::Programmatic);
                  else {
                      text->Focus(Windows::UI::Xaml::FocusState::Programmatic);
                      text->SelectAll();
                  }
                  Windows::UI::ViewManagement::InputPane::GetForCurrentView()->TryShow();
              });
    WeakReference weak(this);
    try {
        create_task(dialog->ShowAsync())
            .then(
                [weak, dialog, text, password, secret,
                 completed](task<Controls::ContentDialogResult> result) {
                    bool confirmed{};
                    try {
                        confirmed = result.get() == Controls::ContentDialogResult::Primary;
                    } catch (Exception ^) {
                    }
                    if (auto page = weak.Resolve<DirectXPage>()) {
                        if (page->game_text_dialog == dialog) {
                            page->game_text_dialog = nullptr;
                            page->Capture(false);
                        }
                    }
                    completed(confirmed, secret ? password->Password->Data() : text->Text->Data());
                },
                task_continuation_context::use_current());
    } catch (Exception ^) {
        game_text_dialog = nullptr;
        Capture(false);
        completed(false, {});
    }
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
