#include "dzn_pso_capture.hpp"
#include <chrono>
#include <directx/d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <fstream>
#include <iostream>
#include <string>
#include <windows.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

int wmain(int argc, wchar_t **argv) {
   if (argc < 3) {
      std::wcerr << L"Usage: DozenPsoReplay pso.bin stream|legacy "
                    L"[adapter-index] [debug]\n";
      return 2;
   }
   try {
      bool legacy = std::wstring(argv[2]) == L"legacy";
      if (!legacy && std::wstring(argv[2]) != L"stream")
         throw std::runtime_error("unknown API");
      bool debug = argc > 4 && std::wstring(argv[4]) == L"debug";
      if (debug) {
         ComPtr<ID3D12Debug> layer;
         if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&layer))))
            layer->EnableDebugLayer();
         else
            throw std::runtime_error("debug layer unavailable");
      }
      DznPsoCapture::Archive a;
      std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
      if (!file)
         throw std::runtime_error("cannot open archive");
      auto length = file.tellg();
      file.seekg(0);
      file.read(reinterpret_cast<char *>(&a.header), sizeof(a.header));
      DznPsoCapture::Header expected;
      if (!file || std::memcmp(a.header.magic, expected.magic, 8) || a.header.version != 1 ||
          a.header.pointer_bytes != sizeof(void *) ||
          a.header.payload_bytes > DznPsoCapture::MaxBytes || a.header.fixups > 4096 ||
          uint64_t(length) != sizeof(a.header) + a.header.fixups * sizeof(DznPsoCapture::Fixup) +
                                  a.header.payload_bytes)
         throw std::runtime_error("invalid archive size or ABI");
      a.fixups.resize(size_t(a.header.fixups));
      a.data.resize(size_t(a.header.payload_bytes));
      file.read(reinterpret_cast<char *>(a.fixups.data()),
                a.fixups.size() * sizeof(DznPsoCapture::Fixup));
      file.read(reinterpret_cast<char *>(a.data.data()), a.data.size());
      if (!file)
         throw std::runtime_error("archive read failed");
      DznPsoCapture::Validate(a);
      ComPtr<IDXGIFactory4> factory;
      HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
      if (FAILED(hr))
         throw std::runtime_error("DXGI factory failed");
      ComPtr<IDXGIAdapter1> adapter;
      hr = factory->EnumAdapters1(argc > 3 ? std::stoul(argv[3]) : 0, &adapter);
      if (FAILED(hr))
         throw std::runtime_error("adapter not found");
      DXGI_ADAPTER_DESC1 adapter_desc;
      adapter->GetDesc1(&adapter_desc);
      std::wcout << L"ADAPTER=" << adapter_desc.Description << L" VENDOR=" << adapter_desc.VendorId
                 << L" DEVICE=" << adapter_desc.DeviceId << L"\n";
      ComPtr<ID3D12Device4> device;
      hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
      if (FAILED(hr))
         throw std::runtime_error("D3D12 device failed");
      std::vector<ComPtr<ID3D12RootSignature>> roots;
      for (auto &f : a.fixups) {
         void *pointer = a.data.data() + f.target;
         if (f.root) {
            ComPtr<ID3D12RootSignature> root;
            hr = device->CreateRootSignature(0, pointer, size_t(f.bytes), IID_PPV_ARGS(&root));
            if (FAILED(hr))
               throw std::runtime_error("root signature creation failed");
            pointer = root.Get();
            roots.push_back(root);
         }
         std::memcpy(a.data.data() + f.slot, &pointer, sizeof(pointer));
      }
      D3D12_PIPELINE_STATE_STREAM_DESC stream = {size_t(a.header.stream_bytes), a.data.data()};
      std::wcout << L"BEGIN API=" << argv[2] << L" STREAM_BYTES=" << stream.SizeInBytes
                 << std::endl;
      auto start = std::chrono::steady_clock::now();
      ComPtr<ID3D12PipelineState> pso;
      hr = legacy ? DznPsoCapture::Legacy(device.Get(), stream, pso.GetAddressOf())
                  : device->CreatePipelineState(&stream, IID_PPV_ARGS(&pso));
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count();
      std::cout << "RESULT=0x" << std::hex << uint32_t(hr) << std::dec << " ELAPSED_MS=" << ms
                << std::endl;
      ComPtr<ID3D12InfoQueue> queue;
      if (debug && SUCCEEDED(device.As(&queue)))
         for (UINT64 i = 0; i < queue->GetNumStoredMessages(); ++i) {
            SIZE_T bytes = 0;
            queue->GetMessage(i, nullptr, &bytes);
            std::vector<uint8_t> data(bytes);
            auto message = reinterpret_cast<D3D12_MESSAGE *>(data.data());
            if (SUCCEEDED(queue->GetMessage(i, message, &bytes)))
               std::cout << "D3D12[" << message->ID << "] " << message->pDescription << std::endl;
         }
      return SUCCEEDED(hr) ? 0 : hr == E_NOTIMPL ? 3 : 1;
   } catch (const std::exception &e) {
      std::cerr << e.what() << std::endl;
      return 2;
   }
}
