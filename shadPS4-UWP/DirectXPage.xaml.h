#pragma once
#include "DirectXPage.g.h"
#include "ShadPS4Host.h"

namespace shadPS4_UWP {
public
ref class DirectXPage sealed {
public:
    DirectXPage();
    void SaveInternalState(Windows::Foundation::Collections::IPropertySet ^ state);
    void LoadInternalState(Windows::Foundation::Collections::IPropertySet ^ state);

private:
    void OnLoaded(Platform::Object ^ sender, Windows::UI::Xaml::RoutedEventArgs ^ args);
    void OnRendering(Platform::Object ^ sender, Platform::Object ^ args);
    void OnOpenGame(Platform::Object ^ sender, Windows::UI::Xaml::RoutedEventArgs ^ args);
    void RestoreGameFolder();
    void OnPause(Platform::Object ^ sender, Windows::UI::Xaml::RoutedEventArgs ^ args);
    void OnStop(Platform::Object ^ sender, Windows::UI::Xaml::RoutedEventArgs ^ args);
    void OnPanelSizeChanged(Platform::Object ^ sender,
                            Windows::UI::Xaml::SizeChangedEventArgs ^ args);
    std::shared_ptr<ShadPS4Host> core;
    bool paused{};
    bool initialized{};
    bool initializing{};
};
} // namespace shadPS4_UWP
