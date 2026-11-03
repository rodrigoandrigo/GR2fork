#pragma once
#include <cstdint>
#include <cstring>
#include <directx/d3d12.h>
#include <directx/d3dx12_core.h>
#include <limits>
#include <stdexcept>
#include <vector>

namespace DznPsoCapture {
static const GUID RootBlobGuid = {
    0x618ce6d1, 0x15d0, 0x4a15, {0xa3, 0x51, 0xb2, 0xa9, 0x65, 0x73, 0x80, 0x14}};
static constexpr size_t MaxBytes = 16u << 20;
struct Header {
   char magic[8] = {'D', 'Z', 'N', 'P', 'S', 'O', '1', 0};
   uint32_t version = 1, pointer_bytes = sizeof(void *);
   uint64_t stream_bytes = 0, payload_bytes = 0, fixups = 0;
};
struct Fixup {
   uint64_t slot, target, bytes, root;
};
struct Archive {
   Header header;
   std::vector<uint8_t> data;
   std::vector<Fixup> fixups;
};
template <class T> inline size_t Layout(size_t &offset) {
   struct Item {
      D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
      T data;
   };
   offset = offsetof(Item, data);
   return (sizeof(Item) + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
}
inline size_t Layout(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, size_t &offset) {
   switch (type) {
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
      return Layout<ID3D12RootSignature *>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
      return Layout<D3D12_SHADER_BYTECODE>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
      return Layout<D3D12_SHADER_BYTECODE>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
      return Layout<D3D12_SHADER_BYTECODE>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
      return Layout<D3D12_SHADER_BYTECODE>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
      return Layout<D3D12_SHADER_BYTECODE>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
      return Layout<D3D12_SHADER_BYTECODE>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
      return Layout<D3D12_STREAM_OUTPUT_DESC>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
      return Layout<D3D12_BLEND_DESC>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
      return Layout<UINT>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
      return Layout<D3D12_RASTERIZER_DESC>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1:
      return Layout<D3D12_RASTERIZER_DESC1>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2:
      return Layout<D3D12_RASTERIZER_DESC2>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
      return Layout<D3D12_DEPTH_STENCIL_DESC>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
      return Layout<D3D12_DEPTH_STENCIL_DESC1>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2:
      return Layout<D3D12_DEPTH_STENCIL_DESC2>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
      return Layout<D3D12_INPUT_LAYOUT_DESC>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
      return Layout<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
      return Layout<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
      return Layout<D3D12_RT_FORMAT_ARRAY>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
      return Layout<DXGI_FORMAT>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
      return Layout<DXGI_SAMPLE_DESC>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
      return Layout<UINT>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
      return Layout<D3D12_CACHED_PIPELINE_STATE>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
      return Layout<D3D12_PIPELINE_STATE_FLAGS>(offset);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
      return Layout<D3D12_VIEW_INSTANCING_DESC>(offset);
   default:
      throw std::runtime_error("unsupported stream subobject");
   }
}
template <class F> inline void Walk(const void *stream, size_t bytes, F visit) {
   size_t cursor = 0;
   while (cursor < bytes) {
      if (bytes - cursor < sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE))
         throw std::runtime_error("short stream");
      D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
      std::memcpy(&type, static_cast<const uint8_t *>(stream) + cursor, sizeof(type));
      size_t offset = 0, size = Layout(type, offset);
      if (size > bytes - cursor)
         throw std::runtime_error("short subobject");
      visit(type, cursor + offset);
      cursor += size;
   }
}
inline void CheckRange(uint64_t offset, uint64_t bytes, size_t size) {
   if (offset > size || bytes > size - offset)
      throw std::runtime_error("archive range invalid");
}
inline size_t Append(Archive &a, const void *data, size_t bytes) {
   size_t offset = (a.data.size() + 7) & ~size_t(7);
   if (bytes > MaxBytes || offset > MaxBytes - bytes || (bytes && !data))
      throw std::runtime_error("capture size or pointer invalid");
   a.data.resize(offset + bytes);
   if (bytes)
      std::memcpy(a.data.data() + offset, data, bytes);
   return offset;
}
inline size_t Pointer(Archive &a, size_t slot, const void *data, size_t bytes, bool root = false) {
   CheckRange(slot, sizeof(void *), a.data.size());
   std::memset(a.data.data() + slot, 0, sizeof(void *));
   if (!bytes)
      return 0;
   size_t target = Append(a, data, bytes);
   a.fixups.push_back({slot, target, bytes, root ? 1u : 0u});
   return target;
}
inline void String(Archive &a, size_t slot, const char *s) {
   if (!s) {
      Pointer(a, slot, nullptr, 0);
      return;
   }
   size_t bytes = 0;
   while (bytes < 4096 && s[bytes])
      ++bytes;
   if (bytes == 4096)
      throw std::runtime_error("semantic name too long");
   Pointer(a, slot, s, bytes + 1);
}
inline Archive Capture(const D3D12_PIPELINE_STATE_STREAM_DESC &desc) {
   Archive a;
   a.header.stream_bytes = desc.SizeInBytes;
   Append(a, desc.pPipelineStateSubobjectStream, desc.SizeInBytes);
   Walk(desc.pPipelineStateSubobjectStream, desc.SizeInBytes,
        [&](D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t, size_t off) {
           const uint8_t *p =
               static_cast<const uint8_t *>(desc.pPipelineStateSubobjectStream) + off;
           if (t == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) {
              auto root = *reinterpret_cast<ID3D12RootSignature *const *>(p);
              if (!root) {
                 Pointer(a, off, nullptr, 0);
                 return;
              }
              UINT bytes = 0;
              root->GetPrivateData(RootBlobGuid, &bytes, nullptr);
              if (!bytes || bytes > MaxBytes)
                 throw std::runtime_error("serialized root signature unavailable");
              std::vector<uint8_t> blob(bytes);
              if (FAILED(root->GetPrivateData(RootBlobGuid, &bytes, blob.data())))
                 throw std::runtime_error("root signature read failed");
              Pointer(a, off, blob.data(), bytes, true);
           } else if (t >= D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS &&
                      t <= D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS) {
              auto &b = *reinterpret_cast<const D3D12_SHADER_BYTECODE *>(p);
              Pointer(a, off, b.pShaderBytecode, b.BytecodeLength);
           } else if (t == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT) {
              auto &l = *reinterpret_cast<const D3D12_INPUT_LAYOUT_DESC *>(p);
              if (l.NumElements > 32)
                 throw std::runtime_error("input layout count invalid");
              size_t base = Pointer(a, off, l.pInputElementDescs,
                                    size_t(l.NumElements) * sizeof(D3D12_INPUT_ELEMENT_DESC));
              for (UINT i = 0; i < l.NumElements; ++i)
                 String(a, base + i * sizeof(D3D12_INPUT_ELEMENT_DESC),
                        l.pInputElementDescs[i].SemanticName);
           } else if (t == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT) {
              auto &l = *reinterpret_cast<const D3D12_STREAM_OUTPUT_DESC *>(p);
              if (l.NumEntries > 512 || l.NumStrides > 4)
                 throw std::runtime_error("stream output count invalid");
              size_t base = Pointer(a, off, l.pSODeclaration,
                                    size_t(l.NumEntries) * sizeof(D3D12_SO_DECLARATION_ENTRY));
              Pointer(a, off + offsetof(D3D12_STREAM_OUTPUT_DESC, pBufferStrides), l.pBufferStrides,
                      size_t(l.NumStrides) * sizeof(UINT));
              for (UINT i = 0; i < l.NumEntries; ++i)
                 String(a,
                        base + i * sizeof(D3D12_SO_DECLARATION_ENTRY) +
                            offsetof(D3D12_SO_DECLARATION_ENTRY, SemanticName),
                        l.pSODeclaration[i].SemanticName);
           } else if (t == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING) {
              auto &l = *reinterpret_cast<const D3D12_VIEW_INSTANCING_DESC *>(p);
              if (l.ViewInstanceCount > 4)
                 throw std::runtime_error("view count invalid");
              Pointer(a, off + offsetof(D3D12_VIEW_INSTANCING_DESC, pViewInstanceLocations),
                      l.pViewInstanceLocations,
                      size_t(l.ViewInstanceCount) * sizeof(D3D12_VIEW_INSTANCE_LOCATION));
           } else if (t == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO) {
              auto &b = *reinterpret_cast<const D3D12_CACHED_PIPELINE_STATE *>(p);
              Pointer(a, off, b.pCachedBlob, b.CachedBlobSizeInBytes);
           }
        });
   a.header.payload_bytes = a.data.size();
   a.header.fixups = a.fixups.size();
   return a;
}
template <class T> inline T Read(const void *p) {
   T value;
   std::memcpy(&value, p, sizeof(value));
   return value;
}
inline HRESULT Legacy(ID3D12Device *device, const D3D12_PIPELINE_STATE_STREAM_DESC &desc,
                      ID3D12PipelineState **out) {
   D3D12_GRAPHICS_PIPELINE_STATE_DESC g = {};
   D3D12_COMPUTE_PIPELINE_STATE_DESC c = {};
   bool compute = false, seen_depth = false;
   g.SampleMask = UINT_MAX;
   g.SampleDesc = {1, 0};
   g.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
   g.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
   g.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
   g.DepthStencilState.DepthEnable = FALSE;
   g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
   HRESULT supported = S_OK;
   Walk(desc.pPipelineStateSubobjectStream, desc.SizeInBytes,
        [&](D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t, size_t off) {
           const void *p = static_cast<const uint8_t *>(desc.pPipelineStateSubobjectStream) + off;
           switch (t) {
           case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
              g.pRootSignature = Read<ID3D12RootSignature *>(p);
              break;
           case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
              g.VS = Read<D3D12_SHADER_BYTECODE>(p);
              break;
           case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
              g.PS = Read<D3D12_SHADER_BYTECODE>(p);
              break;
           case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
              g.DS = Read<D3D12_SHADER_BYTECODE>(p);
              break;
           case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
              g.HS = Read<D3D12_SHADER_BYTECODE>(p);
              break;
           case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
              g.GS = Read<D3D12_SHADER_BYTECODE>(p);
              break;
           case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
              g.StreamOutput = Read<D3D12_STREAM_OUTPUT_DESC>(p);
              break;
           case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
              g.BlendState = Read<D3D12_BLEND_DESC>(p);
              break;
           case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
              g.SampleMask = Read<UINT>(p);
              break;
           case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
              g.RasterizerState = Read<D3D12_RASTERIZER_DESC>(p);
              break;
           case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
              g.InputLayout = Read<D3D12_INPUT_LAYOUT_DESC>(p);
              break;
           case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
              g.IBStripCutValue = Read<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(p);
              break;
           case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
              g.PrimitiveTopologyType = Read<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(p);
              break;
           case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
              g.DSVFormat = Read<DXGI_FORMAT>(p);
              break;
           case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
              g.SampleDesc = Read<DXGI_SAMPLE_DESC>(p);
              break;
           case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
              g.NodeMask = Read<UINT>(p);
              break;
           case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
              g.CachedPSO = Read<D3D12_CACHED_PIPELINE_STATE>(p);
              break;
           case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
              g.Flags = Read<D3D12_PIPELINE_STATE_FLAGS>(p);
              break;
           case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
              g.DepthStencilState = Read<D3D12_DEPTH_STENCIL_DESC>(p);
              seen_depth = true;
              break;
           case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
              c.CS = *static_cast<const D3D12_SHADER_BYTECODE *>(p);
              compute = true;
              break;
           case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: {
              auto &r = *static_cast<const D3D12_RT_FORMAT_ARRAY *>(p);
              g.NumRenderTargets = r.NumRenderTargets;
              std::memcpy(g.RTVFormats, r.RTFormats, sizeof(g.RTVFormats));
              break;
           }
           case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: {
              seen_depth = true;
              auto &d = *static_cast<const D3D12_DEPTH_STENCIL_DESC1 *>(p);
              if (d.DepthBoundsTestEnable) {
                 supported = E_NOTIMPL;
                 break;
              }
              g.DepthStencilState = {d.DepthEnable,   d.DepthWriteMask,  d.DepthFunc,
                                     d.StencilEnable, d.StencilReadMask, d.StencilWriteMask,
                                     d.FrontFace,     d.BackFace};
              break;
           }
           case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
              auto &v = *static_cast<const D3D12_VIEW_INSTANCING_DESC *>(p);
              if (v.ViewInstanceCount || v.Flags)
                 supported = E_NOTIMPL;
              break;
           }
           default:
              supported = E_NOTIMPL;
              break;
           }
        });
   if (!seen_depth && g.DSVFormat != DXGI_FORMAT_UNKNOWN)
      g.DepthStencilState.DepthEnable = TRUE;
   if (FAILED(supported))
      return supported;
   if (compute) {
      c.pRootSignature = g.pRootSignature;
      c.NodeMask = g.NodeMask;
      c.CachedPSO = g.CachedPSO;
      c.Flags = g.Flags;
      return device->CreateComputePipelineState(&c, IID_PPV_ARGS(out));
   }
   return device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(out));
}
inline void Validate(const Archive &a) {
   const Header expected;
   if (std::memcmp(a.header.magic, expected.magic, 8) || a.header.version != 1 ||
       a.header.pointer_bytes != sizeof(void *) || a.header.payload_bytes != a.data.size() ||
       a.header.fixups != a.fixups.size() || a.data.size() > MaxBytes ||
       a.header.stream_bytes > a.data.size() || a.fixups.size() > 4096)
      throw std::runtime_error("invalid archive header");
   for (auto &f : a.fixups) {
      CheckRange(f.slot, sizeof(void *), a.data.size());
      CheckRange(f.target, f.bytes, a.data.size());
      if (f.root > 1)
         throw std::runtime_error("invalid fixup kind");
   }
   std::vector<bool> used(a.fixups.size());
   auto pointer = [&](size_t slot, uint64_t bytes, bool root = false) -> size_t {
      CheckRange(slot, sizeof(void *), a.data.size());
      if (Read<uintptr_t>(a.data.data() + slot))
         throw std::runtime_error("archive contains process pointer");
      for (size_t i = 0; i < a.fixups.size(); ++i)
         if (a.fixups[i].slot == slot) {
            auto &f = a.fixups[i];
            if (used[i] || f.root != unsigned(root) || !f.bytes ||
                f.target < a.header.stream_bytes || (bytes != UINT64_MAX && f.bytes != bytes))
               throw std::runtime_error("archive pointer mismatch");
            used[i] = true;
            return size_t(f.target);
         }
      if (bytes && bytes != UINT64_MAX)
         throw std::runtime_error("missing archive pointer");
      return 0;
   };
   auto string = [&](size_t slot) {
      size_t target = pointer(slot, UINT64_MAX);
      if (target) {
         for (auto &f : a.fixups)
            if (f.slot == slot)
               if (f.bytes > 4096 || a.data[f.target + f.bytes - 1])
                  throw std::runtime_error("invalid semantic string");
      }
   };
   Walk(a.data.data(), size_t(a.header.stream_bytes),
        [&](D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t, size_t off) {
           const auto *p = a.data.data() + off;
           if (t == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE)
              pointer(off, UINT64_MAX, true);
           else if (t >= D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS &&
                    t <= D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS)
              pointer(off, Read<D3D12_SHADER_BYTECODE>(p).BytecodeLength);
           else if (t == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT) {
              auto l = Read<D3D12_INPUT_LAYOUT_DESC>(p);
              if (l.NumElements > 32)
                 throw std::runtime_error("invalid input count");
              size_t base = pointer(off, size_t(l.NumElements) * sizeof(D3D12_INPUT_ELEMENT_DESC));
              for (UINT i = 0; i < l.NumElements; ++i)
                 string(base + i * sizeof(D3D12_INPUT_ELEMENT_DESC));
           } else if (t == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT) {
              auto l = Read<D3D12_STREAM_OUTPUT_DESC>(p);
              if (l.NumEntries > 512 || l.NumStrides > 4)
                 throw std::runtime_error("invalid SO count");
              size_t base = pointer(off, size_t(l.NumEntries) * sizeof(D3D12_SO_DECLARATION_ENTRY));
              pointer(off + offsetof(D3D12_STREAM_OUTPUT_DESC, pBufferStrides),
                      size_t(l.NumStrides) * sizeof(UINT));
              for (UINT i = 0; i < l.NumEntries; ++i)
                 string(base + i * sizeof(D3D12_SO_DECLARATION_ENTRY) +
                        offsetof(D3D12_SO_DECLARATION_ENTRY, SemanticName));
           } else if (t == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING) {
              auto l = Read<D3D12_VIEW_INSTANCING_DESC>(p);
              if (l.ViewInstanceCount > 4)
                 throw std::runtime_error("invalid view count");
              pointer(off + offsetof(D3D12_VIEW_INSTANCING_DESC, pViewInstanceLocations),
                      size_t(l.ViewInstanceCount) * sizeof(D3D12_VIEW_INSTANCE_LOCATION));
           } else if (t == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO)
              pointer(off, Read<D3D12_CACHED_PIPELINE_STATE>(p).CachedBlobSizeInBytes);
        });
   for (bool expected : used)
      if (!expected)
         throw std::runtime_error("unexpected archive fixup");
}
} // namespace DznPsoCapture
