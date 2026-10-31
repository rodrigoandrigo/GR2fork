// SPDX-License-Identifier: GPL-2.0-or-later

#include "d3d12_device_context.h"
#include <algorithm>
#include <cstdlib>

#include <dxgi1_4.h>
#include <winrt/base.h>

#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <mutex>
#include <d3d12sdklayers.h>

using Microsoft::WRL::ComPtr;

void D3D12DeviceContext::EnableDiagnostics() {
#ifndef _DEBUG
  char enabled[2]{};
  size_t length{};
  if (getenv_s(&length, enabled, sizeof(enabled), "GAME_D3D_DEBUG") ||
      length != sizeof(enabled) || enabled[0] != '1')
    return;
#endif
  static std::once_flag configured;
  std::call_once(configured, [] {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
      debug->EnableDebugLayer();
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings)))) {
      settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    }
  });
}

void D3D12DeviceContext::CheckResult(HRESULT result, const wchar_t *operation) const {
  if (SUCCEEDED(result))
    return;
  std::wostringstream message;
  message << operation << L": HRESULT 0x" << std::hex << static_cast<unsigned>(result);
  const auto reason = device_ ? device_->GetDeviceRemovedReason() : S_OK;
  message << L"; device reason 0x" << static_cast<unsigned>(reason);
  if (device_ && (FAILED(reason) || result == E_OUTOFMEMORY)) {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter3> adapter;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->EnumAdapterByLuid(device_->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) {
      for (const auto group : {DXGI_MEMORY_SEGMENT_GROUP_LOCAL, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL}) {
        DXGI_QUERY_VIDEO_MEMORY_INFO budget{};
        if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, group, &budget)))
          message << L"; memory segment=" << std::dec << group << L" usage=" << budget.CurrentUsage
                  << L" budget=" << budget.Budget << L" reserved=" << budget.CurrentReservation;
      }
    }
    message << std::hex;
  }
  if (device_ && FAILED(reason)) {
    ComPtr<ID3D12InfoQueue> info;
    const auto info_status = device_.As(&info);
    message << L"; info queue 0x" << static_cast<unsigned>(info_status);
    if (SUCCEEDED(info_status)) {
      const auto count = info->GetNumStoredMessagesAllowedByRetrievalFilter();
      message << L"; message count=" << std::dec << count;
      const auto first = count > 32 ? count - 32 : 0;
      for (unsigned severity = D3D12_MESSAGE_SEVERITY_CORRUPTION;
           severity <= D3D12_MESSAGE_SEVERITY_WARNING; ++severity) {
        for (auto i = first; i < count; ++i) {
          SIZE_T size{};
          if (FAILED(info->GetMessage(i, nullptr, &size)) || !size || size > 65536)
            continue;
          std::vector<uint8_t> storage(size);
          auto *entry = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
          if (SUCCEEDED(info->GetMessage(i, entry, &size)) &&
              entry->Severity == severity) {
            message << L"; D3D12 id=" << std::dec << entry->ID << L" severity=" << entry->Severity;
            if (entry->pDescription)
              message << L" " << winrt::to_hstring(entry->pDescription).c_str();
          }
        }
      }
    }
    message << std::hex;
    ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    const auto available = device_.As(&dred);
    message << L"; DRED interface 0x" << static_cast<unsigned>(available);
    if (SUCCEEDED(available)) {
      D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
      const auto status = dred->GetPageFaultAllocationOutput1(&fault);
      message << L"; page fault query 0x" << static_cast<unsigned>(status);
      if (SUCCEEDED(status)) {
        message << L"; GPU VA 0x" << fault.PageFaultVA;
        const auto allocations = [&](const D3D12_DRED_ALLOCATION_NODE1 *node,
                                     const wchar_t *kind) {
          for (unsigned i = 0; node && i < 8; ++i, node = node->pNext) {
            message << L"; " << kind << L" type=" << std::dec << node->AllocationType;
            if (node->ObjectNameW)
              message << L" name=" << node->ObjectNameW;
          }
        };
        allocations(fault.pHeadExistingAllocationNode, L"existing");
        allocations(fault.pHeadRecentFreedAllocationNode, L"freed");
      }
      D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
      const auto breadcrumb_status = dred->GetAutoBreadcrumbsOutput1(&breadcrumbs);
      message << L"; breadcrumb query 0x" << std::hex << static_cast<unsigned>(breadcrumb_status);
      if (SUCCEEDED(breadcrumb_status)) {
        auto *node = breadcrumbs.pHeadAutoBreadcrumbNode;
        for (unsigned i = 0; node && i < 32; ++i, node = node->pNext) {
          const auto completed = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
          message << L"; list=" << node->pCommandList << L" queue=" << node->pCommandQueue;
          if (node->pCommandListDebugNameW)
            message << L" name=" << node->pCommandListDebugNameW;
          message << L" completed=" << std::dec << completed << L"/" << node->BreadcrumbCount;
          if (node->pCommandHistory && completed < node->BreadcrumbCount)
            message << L" next-op=" << node->pCommandHistory[completed];
          for (UINT context = 0; node->pBreadcrumbContexts &&
                                 context < node->BreadcrumbContextsCount && context < 8; ++context) {
            const auto &entry = node->pBreadcrumbContexts[context];
            if (entry.pContextString)
              message << L" context[" << entry.BreadcrumbIndex << L"]=" << entry.pContextString;
          }
        }
      }
    }
  }
  OutputDebugStringW(message.str().c_str());
  throw std::runtime_error(winrt::to_string(message.str()));
}

D3D12DeviceContext::~D3D12DeviceContext() {
  if (direct_queue_ && fence_ && fence_event_ != INVALID_HANDLE_VALUE) {
    try {
      WaitForGpu();
    } catch (...) {
    }
  }
  if (fence_event_ != INVALID_HANDLE_VALUE) {
    CloseHandle(fence_event_);
  }
}

void D3D12DeviceContext::Initialize(IUnknown *adapter) {
  if (device_) {
    winrt::throw_hresult(E_UNEXPECTED);
  }
  winrt::check_hresult(D3D12CreateDevice(
      adapter, D3D_FEATURE_LEVEL_11_0,
      IID_PPV_ARGS(device_.ReleaseAndGetAddressOf())));

  D3D12_COMMAND_QUEUE_DESC queue_description{};
  queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  winrt::check_hresult(device_->CreateCommandQueue(
      &queue_description,
      IID_PPV_ARGS(direct_queue_.ReleaseAndGetAddressOf())));

  winrt::check_hresult(device_->CreateFence(
      0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.ReleaseAndGetAddressOf())));
  direct_queue_->SetName(L"shadPS4 UWP frontend queue");
  fence_event_ = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
  if (fence_event_ == nullptr) {
    fence_event_ = INVALID_HANDLE_VALUE;
    winrt::throw_last_error();
  }
}

void D3D12DeviceContext::CreateDirectCommands(
    ComPtr<ID3D12CommandAllocator> &allocator,
    ComPtr<ID3D12GraphicsCommandList> &list) const {
  winrt::check_hresult(device_->CreateCommandAllocator(
      D3D12_COMMAND_LIST_TYPE_DIRECT,
      IID_PPV_ARGS(allocator.ReleaseAndGetAddressOf())));
  winrt::check_hresult(device_->CreateCommandList(
      0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
      IID_PPV_ARGS(list.ReleaseAndGetAddressOf())));
}

UINT64 D3D12DeviceContext::Signal() {
  CheckResult(submission_error_, L"D3D12 queue submission");
  // UINT64_MAX is reserved by D3D12 to report device removal.
  if (fence_value_ >= std::numeric_limits<UINT64>::max() - 1U) {
    winrt::throw_hresult(E_UNEXPECTED);
  }
  const UINT64 value = fence_value_ + 1U;
  submission_error_ = direct_queue_->Signal(fence_.Get(), value);
  // A failed signal leaves already-executed lists without a completion ticket.
  // Do not allow later calls to recycle their allocators using an older ticket.
  CheckResult(submission_error_, L"D3D12 queue submission");
  fence_value_ = value;
  return value;
}

UINT64 D3D12DeviceContext::Submit(ID3D12CommandList *list) {
  return SubmitBatch(&list, 1);
}
UINT64 D3D12DeviceContext::SubmitBatch(ID3D12CommandList *const *lists, UINT count) {
  CheckResult(submission_error_, L"D3D12 queue submission");
  if (!lists || !count || count > 64 ||
      std::any_of(lists, lists + count, [](auto* list) { return list == nullptr; }))
    winrt::throw_hresult(E_INVALIDARG);
  direct_queue_->ExecuteCommandLists(count, lists);
  submitted_lists_ += count;
  return Signal();
}

UINT64 D3D12DeviceContext::CompletedValue() const {
  CheckResult(submission_error_, L"D3D12 queue submission");
  const UINT64 completed = fence_->GetCompletedValue();
  if (completed == std::numeric_limits<UINT64>::max()) {
    CheckResult(device_->GetDeviceRemovedReason(), L"D3D12 fence GetCompletedValue");
    winrt::throw_hresult(DXGI_ERROR_DEVICE_REMOVED);
  }
  return completed;
}

void D3D12DeviceContext::Wait(UINT64 ticket) {
  if (ticket > fence_value_) {
    winrt::throw_hresult(E_INVALIDARG);
  }
  if (CompletedValue() >= ticket) {
    return;
  }

  CheckResult(fence_->SetEventOnCompletion(ticket, fence_event_), L"D3D12 fence SetEventOnCompletion");
  ++blocking_waits_;
  for (;;) {
    const DWORD wait_result = WaitForSingleObjectEx(fence_event_, 500, FALSE);
    if (wait_result == WAIT_FAILED) {
      winrt::throw_last_error();
    }
    if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_TIMEOUT) {
      winrt::throw_hresult(E_UNEXPECTED);
    }
    if (CompletedValue() >= ticket) {
      return;
    }
    CheckResult(device_->GetDeviceRemovedReason(), L"D3D12 fence wait device status");
  }
}

void D3D12DeviceContext::WaitForGpu() {
  Wait(Signal());
}

bool D3D12DeviceContext::DrainAfterFailure() noexcept {
  if (!direct_queue_) {
    return true;
  }
  if (!device_ || !fence_ || fence_event_ == INVALID_HANDLE_VALUE) {
    return false;
  }
  const HRESULT removal_reason = device_->GetDeviceRemovedReason();
  if (FAILED(removal_reason)) {
    submission_error_ = removal_reason;
    return true;
  }
  if (fence_value_ >= std::numeric_limits<UINT64>::max() - 1U) {
    submission_error_ = E_UNEXPECTED;
    return false;
  }

  const UINT64 ticket = fence_value_ + 1U;
  const HRESULT signal_result = direct_queue_->Signal(fence_.Get(), ticket);
  if (FAILED(signal_result)) {
    submission_error_ = signal_result;
    const HRESULT reason_after_signal = device_->GetDeviceRemovedReason();
    if (FAILED(reason_after_signal)) {
      submission_error_ = reason_after_signal;
      return true;
    }
    return false;
  }

  fence_value_ = ticket;
  submission_error_ = S_OK;
  try {
    Wait(ticket);
    return true;
  } catch (...) {
    const HRESULT reason_after_wait = device_->GetDeviceRemovedReason();
    if (FAILED(reason_after_wait)) {
      submission_error_ = reason_after_wait;
      return true;
    }
    return false;
  }
}

bool D3D12DeviceContext::TryTrim() {
  ComPtr<IDXGIDevice3> dxgi_device;
  const HRESULT result = device_.As(&dxgi_device);
  if (result == E_NOINTERFACE) {
    return false;
  }
  winrt::check_hresult(result);
  dxgi_device->Trim();
  return true;
}
