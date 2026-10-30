// SPDX-License-Identifier: GPL-2.0-or-later
namespace NativeRaw {
using NativeAlias::Check;
using NativeAlias::ComPtr;
struct Format {
  const char *name;
  DXGI_FORMAT format;
  unsigned bytes;
};
const Format Formats[] = {
    {"R8G8_UNORM", DXGI_FORMAT_R8G8_UNORM, 2},
    {"R8G8_SNORM", DXGI_FORMAT_R8G8_SNORM, 2},
    {"R16G16_SFLOAT", DXGI_FORMAT_R16G16_FLOAT, 4},
    {"R32G32_SFLOAT", DXGI_FORMAT_R32G32_FLOAT, 8},
    {"A2B10G10R10_UNORM_PACK32", DXGI_FORMAT_R10G10B10A2_UNORM, 4},
    {"B10G11R11_UFLOAT_PACK32", DXGI_FORMAT_R11G11B10_FLOAT, 4},
};
using Pixel = std::array<float, 4>;
uint32_t PackUnsigned(float value, unsigned mantissa_bits) {
  if (std::isnan(value))
    return (31u << mantissa_bits) | (1u << (mantissa_bits - 1));
  if (value <= 0)
    return 0;
  if (std::isinf(value))
    return 31u << mantissa_bits;
  if (value < std::ldexp(1.f, -14))
    return static_cast<uint32_t>(
        std::nearbyint(std::ldexp(double(value), 14 + mantissa_bits)));
  int exponent;
  std::frexp(value, &exponent);
  --exponent;
  if (exponent > 15)
    return 31u << mantissa_bits;
  uint32_t fraction = static_cast<uint32_t>(std::nearbyint(
      (std::ldexp(double(value), -exponent) - 1) * (1u << mantissa_bits)));
  return (uint32_t(exponent + 15) << mantissa_bits) + fraction;
}
float UnpackUnsigned(uint32_t bits, unsigned mantissa_bits) {
  unsigned exponent = bits >> mantissa_bits;
  unsigned fraction = bits & ((1u << mantissa_bits) - 1);
  if (exponent == 31)
    return fraction ? std::numeric_limits<float>::quiet_NaN()
                    : std::numeric_limits<float>::infinity();
  if (!exponent)
    return std::ldexp(float(fraction), -14 - int(mantissa_bits));
  return std::ldexp(1.f + float(fraction) / float(1u << mantissa_bits),
                    int(exponent) - 15);
}
uint16_t PackHalf(float value) {
  return uint16_t(PackUnsigned(std::abs(value), 10) |
                  (std::signbit(value) ? 0x8000 : 0));
}
float UnpackHalf(uint16_t value) {
  float result = UnpackUnsigned(value & 0x7fff, 10);
  return value & 0x8000 ? -result : result;
}
std::array<uint32_t, 2> Pack(unsigned format, Pixel value) {
  std::array<uint32_t, 2> bits{};
  if (format == 0 || format == 1) {
    for (unsigned c = 0; c < 2; c++) {
      int channel =
          format == 0
              ? int(std::nearbyint(std::clamp(value[c], 0.f, 1.f) * 255))
              : int(std::nearbyint(std::clamp(value[c], -1.f, 1.f) * 127));
      bits[0] |= (uint32_t(channel) & 255) << (8 * c);
    }
  } else if (format == 2) {
    bits[0] = PackHalf(value[0]) | (uint32_t(PackHalf(value[1])) << 16);
  } else if (format == 3) {
    bits[0] = std::bit_cast<uint32_t>(value[0]);
    bits[1] = std::bit_cast<uint32_t>(value[1]);
  } else if (format == 4) {
    for (unsigned c = 0; c < 4; c++)
      bits[0] |= uint32_t(std::nearbyint(std::clamp(value[c], 0.f, 1.f) *
                                         (c == 3 ? 3 : 1023)))
                 << (c == 3 ? 30 : c * 10);
  } else {
    bits[0] = PackUnsigned(value[0], 6) | (PackUnsigned(value[1], 6) << 11) |
              (PackUnsigned(value[2], 5) << 22);
  }
  return bits;
}
Pixel Unpack(unsigned format, std::array<uint32_t, 2> bits) {
  Pixel value{0, 0, 0, 1};
  if (format == 0 || format == 1) {
    for (unsigned c = 0; c < 2; c++) {
      unsigned byte = (bits[0] >> (8 * c)) & 255;
      value[c] =
          format == 0
              ? float(byte) / 255.f
              : std::max(float(byte < 128 ? int(byte) : int(byte) - 256) /
                             127.f,
                         -1.f);
    }
  } else if (format == 2) {
    value[0] = UnpackHalf(uint16_t(bits[0]));
    value[1] = UnpackHalf(uint16_t(bits[0] >> 16));
  } else if (format == 3) {
    value[0] = std::bit_cast<float>(bits[0]);
    value[1] = std::bit_cast<float>(bits[1]);
  } else if (format == 4) {
    for (unsigned c = 0; c < 4; c++)
      value[c] =
          float((bits[0] >> (c == 3 ? 30 : c * 10)) & (c == 3 ? 3 : 1023)) /
          float(c == 3 ? 3 : 1023);
  } else {
    value[0] = UnpackUnsigned(bits[0] & 2047, 6);
    value[1] = UnpackUnsigned((bits[0] >> 11) & 2047, 6);
    value[2] = UnpackUnsigned(bits[0] >> 22, 5);
  }
  return value;
}
Pixel Initial(unsigned format, unsigned pixel) {
  static const float normalized[] = {0.f, 0.25f, 0.5f, 0.75f, 1.f};
  const float floating[] = {0.f,
                            -0.f,
                            0.25f,
                            -0.5f,
                            1.f,
                            2.f,
                            std::ldexp(1.f, -24),
                            65504.f,
                            std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::quiet_NaN()};
  const float packed[] = {0.f,
                          0.25f,
                          0.5f,
                          1.f,
                          2.f,
                          std::ldexp(1.f, -20),
                          std::ldexp(1.f, -19),
                          64512.f,
                          std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN()};
  Pixel value{};
  for (unsigned c = 0; c < 4; c++) {
    unsigned index = pixel + c * 3;
    value[c] = format == 2 || format == 3
                   ? floating[index % std::size(floating)]
               : format == 5 ? packed[index % std::size(packed)]
                             : normalized[index % std::size(normalized)];
    if (format == 1)
      value[c] = value[c] * 2 - 1;
  }
  return value;
}
struct Resources : NativeAlias::Resources {
  ComPtr<ID3D12Resource> raw, upload, raw_readback;
};
void Transition(Resources &r, ID3D12Resource *resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        before, after};
  r.commands->ResourceBarrier(1, &barrier);
}
bool Equal(float actual, float expected) {
  if (std::isnan(expected))
    return std::isnan(actual);
  if (std::isinf(expected))
    return actual == expected;
  if (!std::isfinite(actual))
    return false;
  if (expected == 0)
    return actual == 0 && std::signbit(actual) == std::signbit(expected);
  return std::abs(actual - expected) <= std::abs(expected) * 0.000001f;
}
bool EqualBits(unsigned format, const void *actual,
               const std::array<uint32_t, 2> &expected, unsigned bytes) {
  if (!std::memcmp(actual, expected.data(), bytes))
    return true;
  if (format != 2 && format != 3)
    return false;
  std::array<uint32_t, 2> value{};
  std::memcpy(value.data(), actual, bytes);
  for (unsigned channel = 0; channel < 2; channel++) {
    uint32_t a =
        format == 2 ? (value[0] >> (channel * 16)) & 65535 : value[channel];
    uint32_t b = format == 2 ? (expected[0] >> (channel * 16)) & 65535
                             : expected[channel];
    if (a == b)
      continue;
    float af = format == 2 ? UnpackHalf(uint16_t(a)) : std::bit_cast<float>(a);
    float bf = format == 2 ? UnpackHalf(uint16_t(b)) : std::bit_cast<float>(b);
    if (!std::isnan(af) || !std::isnan(bf))
      return false;
  }
  return true;
}
bool Execute(Resources &r, unsigned format,
             const std::filesystem::path &shaders, const Logger &log) {
  const auto &f = Formats[format];
  D3D12_COMMAND_QUEUE_DESC queue{};
  queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  Check(r.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&r.queue)),
        "Raw queue");
  Check(
      r.device->CreateCommandAllocator(queue.Type, IID_PPV_ARGS(&r.allocator)),
      "Raw allocator");
  Check(r.device->CreateCommandList(0, queue.Type, r.allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&r.commands)),
        "Raw command list");
  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  range.NumDescriptors = 2;
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &range};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, 2};
  D3D12_ROOT_SIGNATURE_DESC signature{};
  signature.NumParameters = 2;
  signature.pParameters = params;
  ComPtr<ID3DBlob> blob, error;
  Check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1,
                                    &blob, &error),
        "Raw serialize root");
  Check(r.device->CreateRootSignature(0, blob->GetBufferPointer(),
                                      blob->GetBufferSize(),
                                      IID_PPV_ARGS(&r.root)),
        "Raw root");
  std::ifstream shader(shaders / (std::string(f.name) + "_raw.dxil"),
                       std::ios::binary);
  if (!shader)
    throw std::runtime_error("Raw shader unavailable");
  std::vector<char> bytes((std::istreambuf_iterator<char>(shader)), {});
  D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
  pipeline.pRootSignature = r.root.Get();
  pipeline.CS = {bytes.data(), bytes.size()};
  Check(r.device->CreateComputePipelineState(&pipeline,
                                             IID_PPV_ARGS(&r.pipeline)),
        "Raw pipeline");
  D3D12_HEAP_PROPERTIES properties{};
  properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC image{};
  image.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  image.Width = 16;
  image.Height = 16;
  image.DepthOrArraySize = 1;
  image.MipLevels = 1;
  image.Format = f.format;
  image.SampleDesc.Count = 1;
  Check(r.device->CreateCommittedResource(
            &properties, D3D12_HEAP_FLAG_NONE, &image,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r.image)),
        "Raw texture");
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 total{};
  r.device->GetCopyableFootprints(&image, 0, 1, 0, &footprint, nullptr, nullptr,
                                  &total);
  if (total > 8192)
    throw std::runtime_error("Raw footprint too large");
  r.raw = NativeAlias::Buffer(r, D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_STATE_COPY_DEST, true);
  r.upload = NativeAlias::Buffer(r, D3D12_HEAP_TYPE_UPLOAD,
                                 D3D12_RESOURCE_STATE_GENERIC_READ, false);
  r.output = NativeAlias::Buffer(r, D3D12_HEAP_TYPE_DEFAULT,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  r.readback = NativeAlias::Buffer(r, D3D12_HEAP_TYPE_READBACK,
                                   D3D12_RESOURCE_STATE_COPY_DEST, false);
  r.raw_readback = NativeAlias::Buffer(r, D3D12_HEAP_TYPE_READBACK,
                                       D3D12_RESOURCE_STATE_COPY_DEST, false);
  std::array<std::array<uint32_t, 2>, 256> initial{};
  D3D12_RANGE empty{0, 0};
  void *mapped{};
  Check(r.upload->Map(0, &empty, &mapped), "Map raw upload");
  std::memset(mapped, 0xcd, 8192);
  for (unsigned pixel = 0; pixel < 256; pixel++) {
    initial[pixel] = Pack(format, Initial(format, pixel));
    if (format == 1 && pixel == 0)
      initial[pixel][0] = 0x7f80;
    std::memcpy(static_cast<char *>(mapped) +
                    (pixel / 16) * footprint.Footprint.RowPitch +
                    (pixel % 16) * f.bytes,
                initial[pixel].data(), f.bytes);
  }
  D3D12_RANGE written{0, 8192};
  r.upload->Unmap(0, &written);
  D3D12_TEXTURE_COPY_LOCATION texture{}, buffer{};
  texture.pResource = r.image.Get();
  texture.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  buffer.pResource = r.upload.Get();
  buffer.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  buffer.PlacedFootprint = footprint;
  r.commands->CopyTextureRegion(&texture, 0, 0, 0, &buffer, nullptr);
  Transition(r, r.image.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  buffer.pResource = r.raw.Get();
  r.commands->CopyTextureRegion(&buffer, 0, 0, 0, &texture, nullptr);
  Transition(r, r.raw.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  D3D12_DESCRIPTOR_HEAP_DESC heap{};
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap.NumDescriptors = 2;
  heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  Check(r.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&r.heap)),
        "Raw descriptors");
  auto handle = r.heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
  view.Format = DXGI_FORMAT_R32_TYPELESS;
  view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  view.Buffer.NumElements = 8192 / 4;
  view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  r.device->CreateUnorderedAccessView(r.raw.Get(), nullptr, &view, handle);
  handle.ptr += r.device->GetDescriptorHandleIncrementSize(heap.Type);
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
  for (unsigned phase = 0; phase < 2; phase++) {
    unsigned constants[] = {footprint.Footprint.RowPitch, phase};
    r.commands->SetComputeRoot32BitConstants(1, 2, constants, 0);
    r.commands->Dispatch(2, 2, 1);
    NativeAlias::Barrier(r);
  }
  Transition(r, r.raw.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  Transition(r, r.image.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
             D3D12_RESOURCE_STATE_COPY_DEST);
  r.commands->CopyTextureRegion(&texture, 0, 0, 0, &buffer, nullptr);
  Transition(r, r.image.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  buffer.pResource = r.raw_readback.Get();
  r.commands->CopyTextureRegion(&buffer, 0, 0, 0, &texture, nullptr);
  Transition(r, r.output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  r.commands->CopyResource(r.readback.Get(), r.output.Get());
  Check(r.commands->Close(), "Raw close");
  Check(r.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&r.fence)),
        "Raw fence");
  r.event = CreateEventW(nullptr, false, false, nullptr);
  if (!r.event)
    throw std::runtime_error("Raw event failed");
  Check(r.fence->SetEventOnCompletion(1, r.event), "Raw fence event");
  ID3D12CommandList *lists[] = {r.commands.Get()};
  r.in_flight = true;
  r.queue->ExecuteCommandLists(1, lists);
  Check(r.queue->Signal(r.fence.Get(), 1), "Raw signal");
  if (WaitForSingleObjectEx(r.event, 10000, false) != WAIT_OBJECT_0 ||
      r.fence->GetCompletedValue() != 1)
    throw std::runtime_error("Raw GPU timeout/device loss");
  r.in_flight = false;
  D3D12_RANGE read{0, 8192};
  Check(r.readback->Map(0, &read, &mapped), "Raw result map");
  const float *values = static_cast<const float *>(mapped);
  bool passed = true;
  std::array<std::array<uint32_t, 2>, 256> expected_bits{};
  for (unsigned pixel = 0; pixel < 256; pixel++) {
    Pixel expected = Unpack(format, initial[pixel]);
    Pixel transformed =
        format < 4 ? Pixel{expected[1], expected[0], expected[2], expected[3]}
                   : Pixel{expected[2], expected[0], expected[1], expected[3]};
    expected_bits[pixel] = Pack(format, transformed);
    Pixel reread = Unpack(format, expected_bits[pixel]);
    for (unsigned phase = 0; phase < 2; phase++)
      for (unsigned channel = 0; channel < 4; channel++) {
        float target = phase ? reread[channel] : expected[channel];
        float actual = values[(pixel + phase * 256) * 4 + channel];
        if (!Equal(actual, target)) {
          if (passed)
            log("NATIVE_RAW_MISMATCH pixel=" + std::to_string(pixel) +
                " phase=" + std::to_string(phase) +
                " channel=" + std::to_string(channel) + " expected=" +
                std::to_string(target) + " actual=" + std::to_string(actual));
          passed = false;
        }
      }
  }
  r.readback->Unmap(0, &empty);
  Check(r.raw_readback->Map(0, &read, &mapped), "Raw texture return map");
  for (unsigned pixel = 0; pixel < 256; pixel++) {
    const char *actual = static_cast<const char *>(mapped) +
                         (pixel / 16) * footprint.Footprint.RowPitch +
                         (pixel % 16) * f.bytes;
    if (!EqualBits(format, actual, expected_bits[pixel], f.bytes)) {
      if (passed)
        log("NATIVE_RAW_BITS_MISMATCH pixel=" + std::to_string(pixel));
      passed = false;
    }
  }
  r.raw_readback->Unmap(0, &empty);
  return passed;
}
bool Run(const std::filesystem::path &shaders, const Logger &log,
         const VkPhysicalDeviceIDProperties &id) {
  log("NATIVE_RAW_BEGIN shader_model=6.4 production_support_unchanged=1");
  unsigned passed = 0, failed = 0;
  for (unsigned format = 0; format < std::size(Formats); format++) {
    auto r = std::make_unique<Resources>();
    const auto &f = Formats[format];
    try {
      if (!id.deviceLUIDValid)
        throw std::runtime_error("Raw Vulkan adapter LUID unavailable");
      LUID luid{};
      std::memcpy(&luid, id.deviceLUID, sizeof(luid));
      ComPtr<IDXGIFactory4> factory;
      Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Raw DXGI factory");
      ComPtr<IDXGIAdapter1> adapter;
      Check(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)),
            "Raw match adapter");
      Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                              IID_PPV_ARGS(&r->device)),
            "Raw device");
      log("NATIVE_RAW_CASE " + std::string(f.name) + " matched_vulkan_luid=1");
      bool ok = Execute(*r, format, shaders, log);
      (ok ? passed : failed)++;
      log(std::string(ok ? "NATIVE_RAW_PASS " : "NATIVE_RAW_FAIL ") + f.name +
          " checked_components=2048 checked_texels_bits=256 "
          "phases=texture_to_raw,read_modify_write,reread,raw_to_texture");
    } catch (const std::exception &error) {
      failed++;
      log("NATIVE_RAW_ERROR " + std::string(f.name) + " " + error.what());
      if (r->in_flight) {
        r.release();
        log("NATIVE_RAW_ABORT in_flight_resources_retained=1");
        return false;
      }
    }
  }
  log("NATIVE_RAW_END passed=" + std::to_string(passed) +
      " failed=" + std::to_string(failed));
  return !failed;
}
} // namespace NativeRaw
