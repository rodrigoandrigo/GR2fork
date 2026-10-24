//
// App.xaml.cpp
// Implementação da classe App.
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
/// Inicializa o objeto singleton do aplicativo.  Esta é a primeira linha de código criado
/// executado e, como tal, é o equivalente lógico de main() ou WinMain().
/// </summary>
App::App()
{
    UwpDiagnostics::Initialize(std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\shadPS4");
    UnhandledException += ref new UnhandledExceptionEventHandler([](Object^, UnhandledExceptionEventArgs^ e) {
        UwpDiagnostics::Write(UwpDiagnostics::crash_file, "xaml_unhandled_hresult", static_cast<uint32_t>(e->Exception.Value));
    });
    try {
        Windows::System::MemoryManager::AppMemoryUsageIncreased += ref new EventHandler<Object^>([](Object^, Object^) {
            UwpDiagnostics::Memory("memory_usage_increased bytes/limit");
        });
    } catch (Platform::Exception^ error) {
        UwpDiagnostics::Record("memory_event_unavailable hresult", static_cast<uint32_t>(error->HResult));
    }
    UwpDiagnostics::Memory("app_memory bytes/limit");
	InitializeComponent();
	Suspending += ref new SuspendingEventHandler(this, &App::OnSuspending);
	Resuming += ref new EventHandler<Object^>(this, &App::OnResuming);
}

/// <summary>
/// Chamado quando o aplicativo é iniciado normalmente pelo usuário final.  Outros pontos de entrada
/// serão usados quando o aplicativo é iniciado para abrir um arquivo específico, para exibir
/// resultados da pesquisa e assim por diante.
/// </summary>
/// <param name="e">Detalhes sobre a solicitação e o processo de inicialização.</param>
void App::OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e)
{
    UwpDiagnostics::Record("app_launched previous_execution_state", static_cast<uint32_t>(e->PreviousExecutionState));
	DebugSettings->EnableFrameRateCounter = false;
	if (Windows::System::Profile::AnalyticsInfo::VersionInfo->DeviceFamily == "Windows.Xbox")
	{
        RequiresPointerMode = ApplicationRequiresPointerMode::WhenRequested;
		Windows::UI::ViewManagement::ApplicationViewScaling::TrySetDisableLayoutScaling(true);
		Windows::UI::ViewManagement::ApplicationView::GetForCurrentView()->SetDesiredBoundsMode(
			Windows::UI::ViewManagement::ApplicationViewBoundsMode::UseCoreWindow);
	}

	auto rootFrame = dynamic_cast<Frame^>(Window::Current->Content);

	// Não repita a inicialização do aplicativo quando a Janela já tiver conteúdo,
	// apenas verifique se a janela está ativa
	if (rootFrame == nullptr)
	{
		// Criar um Quadro para agir como o contexto de navegação e associá-lo a
		// uma chave SuspensionManager
		rootFrame = ref new Frame();

		rootFrame->NavigationFailed += ref new Windows::UI::Xaml::Navigation::NavigationFailedEventHandler(this, &App::OnNavigationFailed);

		// Coloque o quadro na Janela atual
		Window::Current->Content = rootFrame;
	}

	if (rootFrame->Content == nullptr)
	{
		// Quando a pilha de navegação não for restaurada, navegar para a primeira página,
		// configurando a nova página passando as informações necessárias como um parâmetro
		// parâmetro
		rootFrame->Navigate(TypeName(DirectXPage::typeid), e->Arguments);
	}

	if (m_directXPage == nullptr)
	{
		m_directXPage = dynamic_cast<DirectXPage^>(rootFrame->Content);
	}

	if (e->PreviousExecutionState == ApplicationExecutionState::Terminated)
	{
		m_directXPage->LoadInternalState(ApplicationData::Current->LocalSettings->Values);
	}
	
	// Verifique se a janela atual está ativa
	Window::Current->Activate();
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
/// Chamado quando a execução do aplicativo está sendo suspensa.  O estado do aplicativo é salvo
/// sem saber se o aplicativo será encerrado ou retomado com o conteúdo
/// da memória ainda intacto.
/// </summary>
/// <param name="sender">A fonte da solicitação de suspensão.</param>
/// <param name="e">Detalhes sobre a solicitação de suspensão.</param>
void App::OnSuspending(Object^ sender, SuspendingEventArgs^ e)
{
	(void) sender;	// Parâmetro não usado
	(void) e;	// Parâmetro não usado

    UwpDiagnostics::Memory("app_suspending bytes/limit");
	m_directXPage->SaveInternalState(ApplicationData::Current->LocalSettings->Values);
}

/// <summary>
/// Invocado quando a execução do aplicativo é retomada.
/// </summary>
/// <param name="sender">A origem da solicitação de retomada.</param>
/// <param name="args">Detalhes sobre a solicitação de retomada.</param>
void App::OnResuming(Object ^sender, Object ^args)
{
	(void) sender; // Parâmetro não usado
	(void) args; // Parâmetro não usado

	m_directXPage->LoadInternalState(ApplicationData::Current->LocalSettings->Values);
}

/// <summary>
/// Chamado quando ocorre uma falha na Navegação para uma determinada página
/// </summary>
/// <param name="sender">O Quadro com navegação com falha</param>
/// <param name="e">Detalhes sobre a falha na navegação</param>
void App::OnNavigationFailed(Platform::Object ^sender, Windows::UI::Xaml::Navigation::NavigationFailedEventArgs ^e)
{
	throw ref new FailureException("Failed to load Page " + e->SourcePageType.Name);
}

