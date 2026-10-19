#pragma once

#include <string_view>

namespace Core::Uwp {
inline constexpr std::string_view ResidentConversion = R"(
ByteAddressBuffer source : register(t0);
ByteAddressBuffer components : register(t1);
RWTexture2D<float4> destination : register(u0);
cbuffer Parameters : register(b0) {
    uint width, height, pitch, mode;
    uint floating_point, hdr;
    float gamma_value, white_nits;
};
float degamma(float v) {
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}
float encode(float v) {
    return v > 0.0031308 ? 1.055 * pow(v, 1.0 / (3.4 - gamma_value)) - 0.055
                         : 12.92 * v / gamma_value;
}
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    uint offset = id.y * pitch + id.x * (mode == 5 ? 8 : 4);
    uint v = source.Load(offset);
    float4 rgba;
    if (mode == 5) {
        uint hi = source.Load(offset + 4);
        rgba = float4(f16tof32(hi & 65535), f16tof32(v >> 16),
                      f16tof32(v & 65535), f16tof32(hi >> 16));
    } else if (mode < 2) {
        rgba = float4((v >> 16) & 255, (v >> 8) & 255, v & 255, v >> 24) / 255.0;
        if (mode == 1) rgba = rgba.bgra;
    } else {
        rgba = float4(float3((v >> 20) & 1023, (v >> 10) & 1023, v & 1023) / 1023.0,
                      float(v >> 30) / 3.0);
    }
    for (uint c = 0; c < 4; ++c) if (!isfinite(rgba[c])) rgba[c] = 0;
    bool pq = mode == 4 || (hdr && (mode == 2 || mode == 3));
    for (uint c = 0; c < 3; ++c) {
        float v = rgba[c];
        if (mode != 5) {
            uint maximum = mode < 2 ? 255 : 1023;
            v = asfloat(components.Load(uint(v * maximum + 0.5) * 4));
        } else {
            if (!floating_point || (!hdr && gamma_value != 1.0))
                v = floating_point ? degamma(encode(v)) : encode(v);
        }
        rgba[c] = v;
    }
    if (pq) {
        precise float r = 1.660491 * rgba.r - 0.587641 * rgba.g - 0.072850 * rgba.b;
        precise float g = -0.124550 * rgba.r + 1.132900 * rgba.g - 0.008349 * rgba.b;
        precise float b = -0.018151 * rgba.r - 0.100579 * rgba.g + 1.118730 * rgba.b;
        rgba.rgb = float3(r,g,b);
    }
    else if (mode == 5 && !hdr) rgba.rgb *= white_nits / 80.0;
    destination[id.xy] = floating_point ? clamp(rgba,-65504.0,65504.0) : saturate(rgba);
}
)";
} // namespace Core::Uwp
