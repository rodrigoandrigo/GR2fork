globallycoherent RWByteAddressBuffer image : register(u0);
RWStructuredBuffer<float4> result : register(u1);
cbuffer Parameters : register(b0) { uint row_pitch; uint phase; };

uint round_shift(uint value, uint shift) {
    if (shift >= 32) return 0;
    uint truncated = value >> shift;
    uint remainder = value & ((1u << shift) - 1);
    uint halfway = 1u << (shift - 1);
    return truncated + (remainder > halfway || (remainder == halfway && (truncated & 1)));
}
uint pack_ufloat(float value, uint mantissa_bits) {
    uint bits = asuint(value);
    uint exponent = (bits >> 23) & 255;
    uint mantissa = bits & 0x7fffff;
    if (exponent == 255 && mantissa != 0) return (31u << mantissa_bits) | (1u << (mantissa_bits - 1));
    if (bits & 0x80000000) return 0;
    if (exponent == 255) return 31u << mantissa_bits;
    int unbiased = int(exponent) - 127;
    if (unbiased > 15) return 31u << mantissa_bits;
    uint significant = mantissa | 0x800000;
    if (unbiased < -14) return round_shift(significant, uint(-14 - unbiased) + 23 - mantissa_bits);
    return (uint(unbiased + 14) << mantissa_bits) + round_shift(significant, 23 - mantissa_bits);
}
float unpack_ufloat(uint value, uint mantissa_bits) {
    uint exponent = value >> mantissa_bits;
    uint mantissa = value & ((1u << mantissa_bits) - 1);
    if (exponent == 31) return asfloat(mantissa == 0 ? 0x7f800000u : 0x7fc00000u);
    if (exponent == 0) return ldexp(float(mantissa), -14 - int(mantissa_bits));
    return ldexp(1 + float(mantissa) / float(1u << mantissa_bits), int(exponent) - 15);
}
float4 load_pixel(uint address) {
#if RAW_FORMAT == 0 || RAW_FORMAT == 1
    uint bits = image.Load(address & ~3u) >> ((address & 2) * 8);
#if RAW_FORMAT == 0
    return float4(float2(bits & 255, (bits >> 8) & 255) / 255.0, 0, 1);
#else
    return float4(max(float2(int(bits << 24) >> 24, int(bits << 16) >> 24) / 127.0, -1.0), 0, 1);
#endif
#elif RAW_FORMAT == 2
    uint bits = image.Load(address);
    return float4(f16tof32(bits & 65535), f16tof32(bits >> 16), 0, 1);
#elif RAW_FORMAT == 3
    return float4(asfloat(image.Load2(address)), 0, 1);
#elif RAW_FORMAT == 4
    uint bits = image.Load(address);
    return float4(float3(bits & 1023, (bits >> 10) & 1023, (bits >> 20) & 1023) / 1023.0, float(bits >> 30) / 3.0);
#else
    uint bits = image.Load(address);
    return float4(unpack_ufloat(bits & 2047, 6), unpack_ufloat((bits >> 11) & 2047, 6), unpack_ufloat(bits >> 22, 5), 1);
#endif
}
void store_pixel(uint address, float4 value) {
#if RAW_FORMAT == 0 || RAW_FORMAT == 1
#if RAW_FORMAT == 0
    uint2 parts = uint2(round(saturate(value.xy) * 255));
#else
    uint2 parts = uint2(int2(round(clamp(value.xy, -1, 1) * 127))) & 255;
#endif
    uint bits = parts.x | (parts.y << 8);
    uint shift = (address & 2) * 8;
    uint aligned = address & ~3u;
    uint old = image.Load(aligned), observed;
    while (true) {
        uint next = (old & ~(65535u << shift)) | (bits << shift);
        image.InterlockedCompareExchange(aligned, old, next, observed);
        if (observed == old) break;
        old = observed;
    }
#elif RAW_FORMAT == 2
    image.Store(address, f32tof16(value.x) | (f32tof16(value.y) << 16));
#elif RAW_FORMAT == 3
    image.Store2(address, asuint(value.xy));
#elif RAW_FORMAT == 4
    uint4 parts = uint4(round(saturate(value) * float4(1023, 1023, 1023, 3)));
    image.Store(address, parts.x | (parts.y << 10) | (parts.z << 20) | (parts.w << 30));
#else
    image.Store(address, pack_ufloat(value.x, 6) | (pack_ufloat(value.y, 6) << 11) | (pack_ufloat(value.z, 5) << 22));
#endif
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint index = id.y * 16 + id.x;
    uint address = id.y * row_pitch + id.x * RAW_BYTES;
    float4 value = load_pixel(address);
    result[index + phase * 256] = value;
    if (phase == 0) {
#if RAW_FORMAT < 4
        store_pixel(address, value.yxzw);
#else
        store_pixel(address, value.zxyw);
#endif
    }
}
