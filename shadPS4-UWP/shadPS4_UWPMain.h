#pragma once

#include "Common\DeviceResources.h"

namespace shadPS4_UWP
{
	class shadPS4_UWPMain : public DX::IDeviceNotify
	{
	public:
		shadPS4_UWPMain(const std::shared_ptr<DX::DeviceResources>& deviceResources);
		~shadPS4_UWPMain();
		void StartRenderLoop();
		void StopRenderLoop();
		Concurrency::critical_section& GetCriticalSection() { return m_criticalSection; }
		virtual void OnDeviceLost();
		virtual void OnDeviceRestored();

	private:
		bool Render();
		std::shared_ptr<DX::DeviceResources> m_deviceResources;
		Windows::Foundation::IAsyncAction^ m_renderLoopWorker;
		Concurrency::critical_section m_criticalSection;
	};
}
