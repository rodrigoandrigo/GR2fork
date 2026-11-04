//
// App.xaml.cpp
// App class implementation.
//

#include "pch.h"
#include "DirectXPage.xaml.h"
#include "UwpDiagnostics.h"

using namespace shadPS4_UWP;

using namespace Platform;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Storage;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Controls::Primitives;
using namespace Windows::UI::Xaml::Data;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Interop;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Navigation;
/// <summary>
/// Initializes the singleton application object. This is the first line of application code
/// executed and, as such, is the logical equivalent of main() or WinMain().
/// </summary>
App::App()
{
    UwpDiagnostics::Initialize(std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\shadPS4");
    UnhandledException += ref new UnhandledExceptionEventHandler([](Object^, UnhandledExceptionEventArgs^ e) {
        UwpDiagnostics::Write(UwpDiagnostics::crash_file, "xaml_unhandled_hresult", static_cast<uint32_t>(e->Exception.Value));
        UwpDiagnostics::Text(UwpDiagnostics::crash_file, "xaml_unhandled_message", e->Message->Data());
    });
    try {
        Windows::System::MemoryManager::AppMemoryUsageIncreased += ref new EventHandler<Object^>([](Object^, Object^) {
            UwpDiagnostics::Memory("memory_usage_increased bytes/limit");
        });
    } catch (Platform::Exception^ error) {
        UwpDiagnostics::Record("memory_event_unavailable hresult", static_cast<uint32_t>(error->HResult));
    }
    UwpDiagnostics::Memory("app_memory bytes/limit");
    UwpDiagnostics::Record("app_xaml_initializing");
	InitializeComponent();
    UwpDiagnostics::Record("app_xaml_initialized");
    if (Windows::System::Profile::AnalyticsInfo::VersionInfo->DeviceFamily == "Windows.Xbox") {
        UwpDiagnostics::Record("xbox_pointer_policy_initializing");
        try {
            RequiresPointerMode = ApplicationRequiresPointerMode::WhenRequested;
            UwpDiagnostics::Record("xbox_pointer_policy_ready");
        } catch (Exception^ error) {
            UwpDiagnostics::Record("xbox_pointer_policy_unavailable hresult", static_cast<uint32_t>(error->HResult));
            UwpDiagnostics::Text(UwpDiagnostics::session_file, "xbox_pointer_policy_message", error->Message->Data());
        }
    }
	Suspending += ref new SuspendingEventHandler(this, &App::OnSuspending);
	Resuming += ref new EventHandler<Object^>(this, &App::OnResuming);
}

/// <summary>
/// Called when the application is launched normally by the end user. Other entry points
/// are used when the application is launched to open a specific file, display
/// search results, and so on.
/// </summary>
/// <param name="e">Details about the launch request and process.</param>
void App::OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e)
{
    UwpDiagnostics::Record("app_launched previous_execution_state", static_cast<uint32_t>(e->PreviousExecutionState));
	if (Windows::System::Profile::AnalyticsInfo::VersionInfo->DeviceFamily == "Windows.Xbox")
	{
        try {
            UwpDiagnostics::Record("xbox_layout_scaling_initializing");
            const auto disabled = Windows::UI::ViewManagement::ApplicationViewScaling::TrySetDisableLayoutScaling(true);
            UwpDiagnostics::Record("xbox_layout_scaling_ready disabled", disabled);
        } catch (Exception^ error) {
            UwpDiagnostics::Record("xbox_layout_scaling_unavailable hresult", static_cast<uint32_t>(error->HResult));
            UwpDiagnostics::Text(UwpDiagnostics::session_file, "xbox_layout_scaling_message", error->Message->Data());
        }
        try {
            UwpDiagnostics::Record("xbox_bounds_initializing");
            const auto applied = Windows::UI::ViewManagement::ApplicationView::GetForCurrentView()->SetDesiredBoundsMode(
                Windows::UI::ViewManagement::ApplicationViewBoundsMode::UseCoreWindow);
            UwpDiagnostics::Record("xbox_bounds_ready applied", applied);
        } catch (Exception^ error) {
            UwpDiagnostics::Record("xbox_bounds_unavailable hresult", static_cast<uint32_t>(error->HResult));
            UwpDiagnostics::Text(UwpDiagnostics::session_file, "xbox_bounds_message", error->Message->Data());
        }
	}

	auto rootFrame = dynamic_cast<Frame^>(Window::Current->Content);

	// Do not repeat application initialization when the Window already contains content;
	// just ensure that the window is active.
	if (rootFrame == nullptr)
	{
		// Create a Frame to act as the navigation context and associate it with
		// a SuspensionManager key.
        UwpDiagnostics::Record("root_frame_initializing");
		rootFrame = ref new Frame();

		rootFrame->NavigationFailed += ref new Windows::UI::Xaml::Navigation::NavigationFailedEventHandler(this, &App::OnNavigationFailed);

		// Place the frame in the current Window.
		Window::Current->Content = rootFrame;
	}

	if (rootFrame->Content == nullptr)
	{
		// When the navigation stack is not restored, navigate to the first page,
		// configuring the new page by passing the required information as a navigation
		// parameter.
        UwpDiagnostics::Record("main_page_navigating");
		rootFrame->Navigate(TypeName(DirectXPage::typeid), e->Arguments);
        UwpDiagnostics::Record("main_page_navigated");
	}

	if (m_directXPage == nullptr)
	{
		m_directXPage = dynamic_cast<DirectXPage^>(rootFrame->Content);
	}

	if (e->PreviousExecutionState == ApplicationExecutionState::Terminated)
	{
		m_directXPage->LoadInternalState(ApplicationData::Current->LocalSettings->Values);
	}
	
	// Ensure that the current window is active.
    UwpDiagnostics::Record("window_activating");
	Window::Current->Activate();
    UwpDiagnostics::Record("window_activated");
	if (!m_backNavigationRegistered)
	{
		try
		{
			Windows::UI::Core::SystemNavigationManager::GetForCurrentView()->BackRequested +=
				ref new EventHandler<Windows::UI::Core::BackRequestedEventArgs^>(
					[](Object^, Windows::UI::Core::BackRequestedEventArgs^ args) {
						args->Handled = true;
					});
			m_backNavigationRegistered = true;
		}
		catch (Exception^ exception)
		{
			OutputDebugStringW(exception->Message->Data());
		}
	}
}
/// <summary>
/// Called when application execution is suspended. The application state is saved
/// without knowing whether the application will be terminated or resumed with its
/// memory contents still intact.
/// </summary>
/// <param name="sender">The source of the suspension request.</param>
/// <param name="e">Details about the suspension request.</param>
void App::OnSuspending(Object^ sender, SuspendingEventArgs^ e)
{
	(void) sender;	// Unused parameter.
	(void) e;	// Unused parameter.

    UwpDiagnostics::Memory("app_suspending bytes/limit");
	if (m_directXPage)
        m_directXPage->SaveInternalState(ApplicationData::Current->LocalSettings->Values);
}

/// <summary>
/// Invoked when application execution resumes.
/// </summary>
/// <param name="sender">The source of the resume request.</param>
/// <param name="args">Details about the resume request.</param>
void App::OnResuming(Object ^sender, Object ^args)
{
	(void) sender; // Unused parameter.
	(void) args; // Unused parameter.

	if (m_directXPage)
        m_directXPage->LoadInternalState(ApplicationData::Current->LocalSettings->Values);
}

/// <summary>
/// Called when navigation to a specific page fails.
/// </summary>
/// <param name="sender">The Frame whose navigation failed.</param>
/// <param name="e">Details about the navigation failure.</param>
void App::OnNavigationFailed(Platform::Object ^sender, Windows::UI::Xaml::Navigation::NavigationFailedEventArgs ^e)
{
	throw ref new FailureException("Failed to load Page " + e->SourcePageType.Name);
}

