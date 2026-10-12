#include "pch.h"
#include "shadPS4_UWPMain.h"

using namespace shadPS4_UWP;
using namespace Windows::Foundation;
using namespace Windows::System::Threading;
using namespace Concurrency;

shadPS4_UWPMain::shadPS4_UWPMain(const std::shared_ptr<DX::DeviceResources>& deviceResources) :
	m_deviceResources(deviceResources)
{
	m_deviceResources->RegisterDeviceNotify(this);
}

shadPS4_UWPMain::~shadPS4_UWPMain()
{
	m_deviceResources->RegisterDeviceNotify(nullptr);
}

void shadPS4_UWPMain::StartRenderLoop()
{
	if (m_renderLoopWorker != nullptr && m_renderLoopWorker->Status == AsyncStatus::Started)
	{
		return;
	}

	auto workItemHandler = ref new WorkItemHandler([this](IAsyncAction^ action)
	{
		while (action->Status == AsyncStatus::Started)
		{
			critical_section::scoped_lock lock(m_criticalSection);
			if (Render())
			{
				m_deviceResources->Present();
			}
		}
	});
	m_renderLoopWorker = ThreadPool::RunAsync(workItemHandler, WorkItemPriority::High, WorkItemOptions::TimeSliced);
}

void shadPS4_UWPMain::StopRenderLoop()
{
	if (m_renderLoopWorker != nullptr)
	{
		m_renderLoopWorker->Cancel();
	}
}

bool shadPS4_UWPMain::Render()
{
	auto context = m_deviceResources->GetD3DDeviceContext();
	auto viewport = m_deviceResources->GetScreenViewport();
	context->RSSetViewports(1, &viewport);
	ID3D11RenderTargetView* const targets[] = { m_deviceResources->GetBackBufferRenderTargetView() };
	context->OMSetRenderTargets(1, targets, nullptr);
	context->ClearRenderTargetView(targets[0], DirectX::Colors::CornflowerBlue);
	return true;
}

void shadPS4_UWPMain::OnDeviceLost()
{
}

void shadPS4_UWPMain::OnDeviceRestored()
{
}
