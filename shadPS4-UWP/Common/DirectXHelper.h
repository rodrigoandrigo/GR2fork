#pragma once

#include <ppltasks.h>	// For create_task.

namespace DX
{
	inline void ThrowIfFailed(HRESULT hr)
	{
		if (FAILED(hr))
		{
			// Set a breakpoint on this line to catch Win32 API errors.
			throw Platform::Exception::CreateException(hr);
		}
	}

	// Reads a binary file asynchronously.
	inline Concurrency::task<std::vector<byte>> ReadDataAsync(const std::wstring& filename)
	{
		using namespace Windows::Storage;
		using namespace Concurrency;

		auto folder = Windows::ApplicationModel::Package::Current->InstalledLocation;

		return create_task(folder->GetFileAsync(Platform::StringReference(filename.c_str()))).then([] (StorageFile^ file) 
		{
			return FileIO::ReadBufferAsync(file);
		}).then([] (Streams::IBuffer^ fileBuffer) -> std::vector<byte> 
		{
			std::vector<byte> returnBuffer;
			returnBuffer.resize(fileBuffer->Length);
			Streams::DataReader::FromBuffer(fileBuffer)->ReadBytes(Platform::ArrayReference<byte>(returnBuffer.data(), fileBuffer->Length));
			return returnBuffer;
		});
	}

	// Convert a size in DIPs (device-independent pixels) to physical pixels.
	inline float ConvertDipsToPixels(float dips, float dpi)
	{
		static const float dipsPerInch = 96.0f;
		return floorf(dips * dpi / dipsPerInch + 0.5f); // Round to the nearest integer.
	}

#if defined(_DEBUG)
	// Check SDK layer support.
	inline bool SdkLayersAvailable()
	{
		HRESULT hr = D3D11CreateDevice(
			nullptr,
			D3D_DRIVER_TYPE_NULL,       // No actual hardware device is required.
			0,
			D3D11_CREATE_DEVICE_DEBUG,  // Check the SDK layers.
			nullptr,                    // Any feature level is sufficient.
			0,
			D3D11_SDK_VERSION,          // Always set to D3D11_SDK_VERSION for Microsoft Store applications.
			nullptr,                    // No need to retain the D3D device reference.
			nullptr,                    // No need to retrieve the feature level.
			nullptr                     // No need to retain the D3D device context reference.
			);

		return SUCCEEDED(hr);
	}
#endif
}
