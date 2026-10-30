// SPDX-License-Identifier: GPL-2.0-or-later
namespace NativeAlias {
using Microsoft::WRL::ComPtr;
void Check(HRESULT hr, const char *operation) {
  if (FAILED(hr))
    throw std::runtime_error(std::string(operation) + " HRESULT=" +
                             std::to_string(static_cast<unsigned long>(hr)));
}
struct Format {
  const char *name;
  DXGI_FORMAT resource, integer, normalized;
  unsigned channels, bits;
  bool signed_value;
};
const Format Formats[] = {
    {"R8_SNORM", DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UINT,
     DXGI_FORMAT_R8_SNORM, 1, 8, true},
    {"R8G8B8A8_SNORM", DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UINT,
     DXGI_FORMAT_R8G8B8A8_SNORM, 4, 8, true},
    {"R16_UNORM", DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UINT,
     DXGI_FORMAT_R16_UNORM, 1, 16, false},
    {"R16G16B16A16_UNORM", DXGI_FORMAT_R16G16B16A16_TYPELESS,
     DXGI_FORMAT_R16G16B16A16_UINT, DXGI_FORMAT_R16G16B16A16_UNORM, 4, 16,
     false},
};
struct Resources {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> commands;
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12PipelineState> pipeline;
  ComPtr<ID3D12DescriptorHeap> heap;
  ComPtr<ID3D12Resource> image, output, readback;
  ComPtr<ID3D12Fence> fence;
  HANDLE event{};
  bool in_flight{};
  ~Resources() {
    if (event)
      CloseHandle(event);
  }
};
ComPtr<ID3D12Resource> Buffer(Resources &r, D3D12_HEAP_TYPE heap,
                              D3D12_RESOURCE_STATES state, bool uav) {
  D3D12_HEAP_PROPERTIES properties{};
  properties.Type = heap;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = 8192;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                   : D3D12_RESOURCE_FLAG_NONE;
  ComPtr<ID3D12Resource> result;
  Check(r.device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE,
                                          &desc, state, nullptr,
                                          IID_PPV_ARGS(&result)),
        "Create buffer");
  return result;
}
void Barrier(Resources &r) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  r.commands->ResourceBarrier(1, &barrier);
}
bool Execute(Resources &r, const Format &f,
             const std::filesystem::path &shaders, const Logger &log) {
  D3D12_COMMAND_QUEUE_DESC queue{};
  queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  Check(r.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&r.queue)),
        "Create queue");
  Check(
      r.device->CreateCommandAllocator(queue.Type, IID_PPV_ARGS(&r.allocator)),
      "Create allocator");
  Check(r.device->CreateCommandList(0, queue.Type, r.allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&r.commands)),
        "Create commands");
  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  range.NumDescriptors = 3;
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &range};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, 1};
  D3D12_ROOT_SIGNATURE_DESC signature{};
  signature.NumParameters = 2;
  signature.pParameters = params;
  ComPtr<ID3DBlob> blob, error;
  Check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1,
                                    &blob, &error),
        "Serialize root");
  Check(r.device->CreateRootSignature(0, blob->GetBufferPointer(),
                                      blob->GetBufferSize(),
                                      IID_PPV_ARGS(&r.root)),
        "Create root");
  std::ifstream file(shaders / (std::string(f.name) + "_alias.dxil"),
                     std::ios::binary);
  if (!file)
    throw std::runtime_error("Cannot read alias shader");
  std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
  D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
  pipeline.pRootSignature = r.root.Get();
  pipeline.CS = {bytes.data(), bytes.size()};
  Check(r.device->CreateComputePipelineState(&pipeline,
                                             IID_PPV_ARGS(&r.pipeline)),
        "Create alias pipeline");
  D3D12_HEAP_PROPERTIES properties{};
  properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC image{};
  image.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  image.Width = 16;
  image.Height = 16;
  image.DepthOrArraySize = 1;
  image.MipLevels = 1;
  image.Format = f.resource;
  image.SampleDesc.Count = 1;
  image.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  Check(r.device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE,
                                          &image,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                          nullptr, IID_PPV_ARGS(&r.image)),
        "Create typeless image");
  r.output = Buffer(r, D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  r.readback = Buffer(r, D3D12_HEAP_TYPE_READBACK,
                      D3D12_RESOURCE_STATE_COPY_DEST, false);
  D3D12_DESCRIPTOR_HEAP_DESC heap{};
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap.NumDescriptors = 3;
  heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  Check(r.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&r.heap)),
        "Create descriptors");
  auto handle = r.heap->GetCPUDescriptorHandleForHeapStart();
  unsigned stride = r.device->GetDescriptorHandleIncrementSize(heap.Type);
  D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
  view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  view.Format = f.integer;
  r.device->CreateUnorderedAccessView(r.image.Get(), nullptr, &view, handle);
  handle.ptr += stride;
  view.Format = f.normalized;
  r.device->CreateUnorderedAccessView(r.image.Get(), nullptr, &view, handle);
  handle.ptr += stride;
  view = {};
  view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  view.Buffer.NumElements = 512;
  view.Buffer.StructureByteStride = 16;
  r.device->CreateUnorderedAccessView(r.output.Get(), nullptr, &view, handle);
  ID3D12DescriptorHeap *heaps[] = {r.heap.Get()};
  r.commands->SetDescriptorHeaps(1, heaps);
  r.commands->SetComputeRootSignature(r.root.Get());
  r.commands->SetComputeRootDescriptorTable(
      0, r.heap->GetGPUDescriptorHandleForHeapStart());
  r.commands->SetPipelineState(r.pipeline.Get());
  for (unsigned phase = 0; phase < 3; phase++) {
    r.commands->SetComputeRoot32BitConstant(1, phase, 0);
    r.commands->Dispatch(2, 2, 1);
    Barrier(r);
  }
  D3D12_RESOURCE_BARRIER transition{};
  transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  transition.Transition = {
      r.output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
  r.commands->ResourceBarrier(1, &transition);
  r.commands->CopyResource(r.readback.Get(), r.output.Get());
  Check(r.commands->Close(), "Close commands");
  Check(r.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&r.fence)),
        "Create fence");
  r.event = CreateEventW(nullptr, false, false, nullptr);
  if (!r.event)
    throw std::runtime_error("CreateEvent failed");
  Check(r.fence->SetEventOnCompletion(1, r.event), "Arm fence");
  ID3D12CommandList *lists[] = {r.commands.Get()};
  r.in_flight = true;
  r.queue->ExecuteCommandLists(1, lists);
  Check(r.queue->Signal(r.fence.Get(), 1), "Signal fence");
  if (WaitForSingleObjectEx(r.event, 10000, false) != WAIT_OBJECT_0 ||
      r.fence->GetCompletedValue() != 1)
    throw std::runtime_error("Alias GPU timeout/device loss");
  r.in_flight = false;
  void *mapped{};
  D3D12_RANGE read{0, 8192};
  Check(r.readback->Map(0, &read, &mapped), "Map alias readback");
  bool passed = true;
  auto values = static_cast<const float *>(mapped);
  for (unsigned pixel = 0; pixel < 512 && passed; pixel++)
    for (unsigned channel = 0; channel < 4; channel++) {
      unsigned x = pixel % 16, y = (pixel % 256) / 16;
      float expected = float((channel == 0   ? x
                              : channel == 1 ? y
                              : channel == 2 ? x + y
                                             : x ^ y) %
                             5) *
                       0.25f;
      if (f.signed_value)
        expected = expected * 2 - 1;
      if (pixel >= 256)
        expected = f.signed_value ? -expected : 1 - expected;
      if (channel >= f.channels)
        expected = channel == 3 ? 1 : 0;
      float actual = values[pixel * 4 + channel];
      float tolerance =
          1.1f / float((1u << (f.signed_value ? f.bits - 1 : f.bits)) - 1);
      if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        log("NATIVE_ALIAS_MISMATCH pixel=" + std::to_string(pixel) +
            " channel=" + std::to_string(channel) + " expected=" +
            std::to_string(expected) + " actual=" + std::to_string(actual));
        passed = false;
        break;
      }
    }
  D3D12_RANGE written{0, 0};
  r.readback->Unmap(0, &written);
  return passed;
}
bool Run(const std::filesystem::path &shaders, const Logger &log,
         const VkPhysicalDeviceIDProperties &id) {
  unsigned passed = 0, failed = 0, unsupported = 0;
  log("NATIVE_ALIAS_BEGIN shader_model=6.4 probe_changes_capabilities=0");
  for (const auto &f : Formats) {
    auto r = std::make_unique<Resources>();
    try {
      if (!id.deviceLUIDValid)
        throw std::runtime_error("Vulkan adapter LUID unavailable");
      LUID luid{};
      static_assert(sizeof(luid) == VK_LUID_SIZE);
      std::memcpy(&luid, id.deviceLUID, sizeof(luid));
      ComPtr<IDXGIFactory4> factory;
      Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create DXGI factory");
      ComPtr<IDXGIAdapter1> adapter;
      Check(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)),
            "Match Vulkan adapter");
      Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                              IID_PPV_ARGS(&r->device)),
            "Create native alias device");
      log("NATIVE_ALIAS_ADAPTER matched_vulkan_luid=1 low=" +
          std::to_string(luid.LowPart) +
          " high=" + std::to_string(luid.HighPart));
      if (&f == &Formats[0]) {
        const DXGI_FORMAT optional[] = {
            DXGI_FORMAT_R8_SNORM,           DXGI_FORMAT_R8G8_UNORM,
            DXGI_FORMAT_R8G8_SNORM,         DXGI_FORMAT_R16_UNORM,
            DXGI_FORMAT_R16G16_FLOAT,       DXGI_FORMAT_R8G8B8A8_SNORM,
            DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R32G32_FLOAT,
            DXGI_FORMAT_R10G10B10A2_UNORM,  DXGI_FORMAT_R11G11B10_FLOAT};
        for (DXGI_FORMAT format : optional) {
          D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
          Check(r->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                               &support, sizeof(support)),
                "Query optional UAV support");
          log("NATIVE_OPTIONAL_CAPS dxgi=" + std::to_string(format) +
              " support1=" + std::to_string(support.Support1) +
              " support2=" + std::to_string(support.Support2));
        }
      }
      D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
      Check(r->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,
                                           &options, sizeof(options)),
            "Query native options");
      D3D12_FEATURE_DATA_FORMAT_SUPPORT normalized{f.normalized},
          integer{f.integer};
      Check(r->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                           &normalized, sizeof(normalized)),
            "Query normalized support");
      Check(r->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                           &integer, sizeof(integer)),
            "Query integer support");
      log("NATIVE_ALIAS_CAPS " + std::string(f.name) + " additional=" +
          std::to_string(options.TypedUAVLoadAdditionalFormats) +
          " normalized_support2=" + std::to_string(normalized.Support2) +
          " integer_support2=" + std::to_string(integer.Support2));
      bool integer_load =
          (integer.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) ||
          options.TypedUAVLoadAdditionalFormats;
      if (!integer_load ||
          !(integer.Support1 &
            D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) ||
          !(normalized.Support1 &
            D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) ||
          !(normalized.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) {
        unsupported++;
        log("NATIVE_ALIAS_UNSUPPORTED " + std::string(f.name));
        continue;
      }
      log("NATIVE_ALIAS_CASE " + std::string(f.name));
      bool ok = Execute(*r, f, shaders, log);
      (ok ? passed : failed)++;
      log(std::string(ok ? "NATIVE_ALIAS_PASS " : "NATIVE_ALIAS_FAIL ") +
          f.name +
          " checked_components=2048 "
          "phases=write,alias_read_write,alias_reread");
    } catch (const std::exception &error) {
      failed++;
      log("NATIVE_ALIAS_ERROR " + std::string(f.name) + " " + error.what());
      if (r->in_flight) {
        r.release();
        log("NATIVE_ALIAS_ABORT in_flight_resources_retained=1");
        return false;
      }
    }
  }
  log("NATIVE_ALIAS_END passed=" + std::to_string(passed) + " failed=" +
      std::to_string(failed) + " unsupported=" + std::to_string(unsupported));
  return !failed;
}
} // namespace NativeAlias
