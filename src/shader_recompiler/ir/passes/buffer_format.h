#pragma once

#include "shader_recompiler/ir/ir_emitter.h"
#include "video_core/amdgpu/resource.h"

namespace Shader::Optimization {
struct FormatInfo {
    AmdGpu::DataFormat data_format;
    AmdGpu::NumberFormat num_format;
    AmdGpu::CompMapping swizzle;
    AmdGpu::NumberConversion num_conversion;
    u32 num_components;
};

IR::Value LoadBufferFormat(IR::IREmitter& ir, IR::Value handle, IR::U32 address,
                           IR::BufferInstInfo info, const FormatInfo& format_info);
void StoreBufferFormat(IR::IREmitter& ir, IR::Value handle, IR::U32 address, const IR::Value& value,
                       IR::BufferInstInfo info, const FormatInfo& format_info);
} // namespace Shader::Optimization
