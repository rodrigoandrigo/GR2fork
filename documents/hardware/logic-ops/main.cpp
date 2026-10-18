// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <orbis/GnmDriver.h>
#include <orbis/libkernel.h>

namespace {
struct BinaryInfo {
    char signature[7];
    uint8_t version;
    uint32_t flags_length, usage;
    uint64_t hash;
    uint32_t crc;
};
struct Commands {
    uint32_t* begin;
    uint32_t* current;
    void Packet(uint32_t op, std::initializer_list<uint32_t> body) {
        *current++ = 0xc0000000u | ((uint32_t(body.size()) - 1) << 16) | (op << 8);
        for (auto word : body)
            *current++ = word;
    }
    void Context(uint32_t reg, uint32_t value) { Packet(0x69, {reg - 0xa000, value}); }
    void Shader(uint32_t reg, uint32_t value) { Packet(0x76, {reg - 0x2c00, value}); }
};
uint32_t Vop1(uint32_t op, uint32_t dst, uint32_t src) {
    return 0x7e000000 | (dst << 17) | (op << 9) | src;
}
uint32_t Vop2(uint32_t op, uint32_t dst, uint32_t a, uint32_t b) {
    return (op << 25) | (dst << 17) | (b << 9) | a;
}
uint32_t Logic(uint32_t rop, uint32_t s, uint32_t d) {
    uint32_t result{};
    for (uint32_t bit = 0; bit < 32; ++bit) {
        const uint32_t index = ((s >> bit) & 1) * 2 + ((d >> bit) & 1);
        result |= ((rop >> index) & 1) << bit;
    }
    return result;
}
}

int main() {
    constexpr size_t Size = 1024 * 1024;
    off_t physical{};
    void* memory{};
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), Size,
                                         0x10000, 3, &physical);
    if (rc < 0)
        return 1;
    rc = sceKernelMapDirectMemory(&memory, Size, 0x33, 0, physical, 0x10000);
    if (rc < 0)
        return 2;
    auto* bytes = static_cast<uint8_t*>(memory);
    auto* vs = reinterpret_cast<uint32_t*>(bytes + 0x10000);
    auto* ps = reinterpret_cast<uint32_t*>(bytes + 0x11000);
    const uint32_t vertex[]{
        Vop2(26, 1, 129, 0), Vop2(27, 1, 130, 1), Vop2(27, 2, 130, 0),
        Vop1(6, 1, 257), Vop1(6, 2, 258), Vop2(8, 1, 244, 1), Vop2(8, 2, 244, 2),
        Vop2(3, 1, 243, 1), Vop2(3, 2, 243, 2), Vop1(1, 3, 128), Vop1(1, 4, 242),
        0xf80008cf, 0x04030201, 0xbf810000};
    const uint32_t fragment[]{Vop1(1, 0, 255), 0x3f000000, Vop1(1, 1, 255), 0x3e800000,
                              Vop1(1, 2, 255), 0x3f400000, Vop1(1, 3, 255), 0x3f800000,
                              0xf800180f, 0x03020100, 0xbf810000};
    std::memcpy(vs, vertex, sizeof(vertex));
    std::memcpy(ps, fragment, sizeof(fragment));
    for (auto shader : {vs, ps}) {
        const bool is_vs = shader == vs;
        const BinaryInfo metadata{{'O', 'r', 'b', 'S', 'h', 'd', 'r'}, 1,
                                  uint32_t(is_vs ? sizeof(vertex) : sizeof(fragment)) << 8,
                                  0, is_vs ? 1u : 2u, 0};
        std::memcpy(reinterpret_cast<uint8_t*>(shader) + 0x40, &metadata, sizeof(metadata));
    }
    struct Format {
        uint32_t data, number, bytes, bits;
        std::array<uint32_t, 4> source;
    };
    const std::array<Format, 5> formats{{
        {10, 0, 4, 8, {128, 64, 191, 255}},
        {10, 1, 4, 8, {64, 32, 95, 127}},
        {10, 6, 4, 8, {188, 137, 225, 255}},
        {12, 7, 8, 16, {0x3800, 0x3400, 0x3a00, 0x3c00}},
        {14, 7, 16, 32, {0x3f000000, 0x3e800000, 0x3f400000, 0x3f800000}}}};
    bool all_match = true;
    for (const auto& format : formats)
        for (uint32_t operation = 0; operation < 16; ++operation) {
            const uint32_t rop = operation * 0x11;
            const uint32_t mask = operation & 1 ? 5 : 15;
            const uint32_t instances = rop == 0x66 ? 2 : 1;
            auto* pixels = bytes + 0x40000;
            std::memset(pixels, 0x35, 64 * 16 * format.bytes);
            auto* fence = reinterpret_cast<volatile uint32_t*>(bytes + 0x30000);
            *fence = 0;
            auto* words = reinterpret_cast<uint32_t*>(bytes);
            const int initialized = sceGnmDrawInitDefaultHardwareState350(words, 0x1000);
            if (initialized <= 0 || initialized > 0x1000)
                return 3;
            Commands commands{words, words + initialized};
            for (auto shader : {vs, ps}) {
                const uint32_t base = shader == vs ? 0x2c48 : 0x2c08;
                const auto address = uintptr_t(shader);
                commands.Shader(base, uint32_t(address >> 8));
                commands.Shader(base + 1, uint32_t(address >> 40));
                commands.Shader(base + 2, (1u << 21) | (shader == vs ? 1 : 0));
                commands.Shader(base + 3, 0);
            }
            commands.Context(0xa318, uint32_t(uintptr_t(pixels) >> 8));
            commands.Context(0xa319, 7);
            commands.Context(0xa31a, 15);
            commands.Context(0xa31b, 0);
            commands.Context(0xa31c, (format.data << 2) | (format.number << 8) | (1 << 15));
            commands.Context(0xa31d, 8);
            commands.Context(0xa08e, mask);
            commands.Context(0xa08f, 15);
            commands.Context(0xa202, (rop << 16) | (1 << 4));
            commands.Context(0xa1e0, 0);
            commands.Context(0xa200, 0);
            commands.Context(0xa203, 0);
            commands.Context(0xa204, 0x90000);
            commands.Context(0xa205, 0);
            commands.Context(0xa206, 0x3f);
            commands.Context(0xa207, 0);
            commands.Context(0xa1b1, 0);
            commands.Context(0xa1b3, 0);
            commands.Context(0xa1b4, 0);
            commands.Context(0xa1b6, 0);
            commands.Context(0xa1c3, 4);
            commands.Context(0xa1c5, 9);
            commands.Context(0xa2d5, 0);
            commands.Context(0xa2f8, 0);
            const uint32_t viewport[]{0x41000000, 0x41000000, 0x41000000,
                                       0x41000000, 0x3f800000, 0};
            for (uint32_t i = 0; i < 6; ++i)
                commands.Context(0xa10f + i, viewport[i]);
            for (uint32_t reg : {0xa00cu, 0xa081u, 0xa090u, 0xa094u}) {
                commands.Context(reg, reg == 0xa00c ? 0 : 0x80000000u);
                commands.Context(reg + 1, 16 | (16 << 16));
            }
            commands.Packet(0x79, {0x242, 4});
            commands.Packet(0x2f, {instances});
            commands.Packet(0x2d, {3, 2});
            const auto completion = uintptr_t(fence);
            commands.Packet(0x47, {20u | (5u << 8), uint32_t(completion),
                                   uint32_t(completion >> 32) | (1u << 29), 2, 0});
            void* addresses[]{words};
            uint32_t sizes[]{uint32_t((commands.current - commands.begin) * 4)};
            rc = sceGnmSubmitCommandBuffers(1, addresses, sizes, nullptr, nullptr);
            if (rc < 0)
                return 4;
            sceGnmSubmitDone();
            for (uint32_t wait = 0; *fence != 2; ++wait) {
                if (wait == 10000)
                    std::puts("GPU_FENCE TIMEOUT; retaining submitted resources");
                sceKernelUsleep(1000);
            }
            const auto* center = pixels + (8 * 64 + 8) * format.bytes;
            bool match = true;
            std::printf("LOGIC fmt=%u number=%u rop=%02x mask=%x instances=%u raw=",
                        format.data, format.number, rop, mask, instances);
            for (uint32_t c = 0; c < 4; ++c) {
                uint32_t actual{}, expected{};
                std::memcpy(&actual, center + c * format.bits / 8, format.bits / 8);
                std::memset(&expected, 0x35, format.bits / 8);
                if (mask & (1u << c))
                    for (uint32_t instance = 0; instance < instances; ++instance)
                        expected = Logic(rop, format.source[c], expected);
                const uint32_t limit = format.bits == 32 ? ~0u : (1u << format.bits) - 1;
                match &= actual == (expected & limit);
                std::printf("%08x,", actual);
            }
            std::printf(" predicted_bitwise=%s\n", match ? "MATCH" : "MISMATCH");
            all_match &= match;
        }
    sceKernelMunmap(memory, Size);
    sceKernelReleaseDirectMemory(physical, Size);
    return all_match ? 0 : 5;
}
