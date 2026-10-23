#pragma once
#include "DirectXPage.g.h"
#include "ImGuiFrontend.h"
namespace shadPS4_UWP {
public
ref class DirectXPage sealed {
public:
    DirectXPage();
    void SaveInternalState(Windows::Foundation::Collections::IPropertySet ^ state);
    void LoadInternalState(Windows::Foundation::Collections::IPropertySet ^ state);

private:
    void OnLoaded(Platform::Object ^ sender, Windows::UI::Xaml::RoutedEventArgs ^ args);
    void OnUnloaded(Platform::Object ^ sender, Windows::UI::Xaml::RoutedEventArgs ^ args);
    void OnPointer(Platform::Object ^ sender,
                   Windows::UI::Xaml::Input::PointerRoutedEventArgs ^ args);
    void OnPanelSizeChanged(Platform::Object ^ sender,
                            Windows::UI::Xaml::SizeChangedEventArgs ^ args);
    void OnAdvancedColorChanged(Windows::Graphics::Display::DisplayInformation ^ sender,
                                Platform::Object ^ args);
    Windows::Graphics::Display::DisplayInformation ^ display_information;
    Windows::Foundation::EventRegistrationToken color_token;
    void PickFolder(std::wstring purpose);
    void EditText(std::string label, std::string value, bool secret,
                  std::function<void(std::string)> completed);
    void Capture(bool captured);
    void OnPointerLost(Platform::Object ^ sender,
                       Windows::UI::Xaml::Input::PointerRoutedEventArgs ^ args);
    void OnCompositionScaleChanged(Windows::UI::Xaml::Controls::SwapChainPanel ^ sender,
                                   Platform::Object ^ args);
    std::shared_ptr<ImGuiFrontend> frontend;
    Windows::UI::Core::CoreWindow ^ window;
    Windows::Foundation::EventRegistrationToken down_token;
    Windows::Foundation::EventRegistrationToken up_token;
    Windows::Foundation::EventRegistrationToken character_token;
    Windows::Foundation::EventRegistrationToken activation_token;
    Windows::Foundation::EventRegistrationToken visibility_token;
};
} // namespace shadPS4_UWP
