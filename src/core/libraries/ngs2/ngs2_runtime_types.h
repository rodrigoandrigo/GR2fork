// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include "ngs2_runtime.h"
#include "ngs2_sampler.h"

extern "C" {
#include <libatrac9.h>
}

namespace Libraries::Ngs2::Engine {

constexpr u32 VoiceInUse = 1;
constexpr u32 VoicePlaying = 2;
constexpr u32 VoicePaused = 4;
constexpr u32 VoiceStopped = 8;
constexpr u32 VoiceError = 16;
constexpr u32 VoiceEmpty = 32;

struct System;
struct Rack;

struct WaveBlock {
    const u8* data;
    OrbisNgs2WaveformBlock info;
    u32 repeated{};
};

struct Voice {
    OrbisNgs2Handle handle{};
    std::weak_ptr<Rack> rack;
    u32 index{};
    u32 flags{VoiceEmpty};
    u32 channels{2};
    u32 input_channels{2};
    u32 output_id{};
    bool initialized{};
    bool exit_loop{};
    OrbisNgs2WaveformFormat format{};
    float pitch{1.0f};
    double position{};
    u64 decoded_samples{};
    u64 decoded_bytes{};
    uintptr_t block_user_data{};
    const void* waveform_data{};
    std::deque<WaveBlock> blocks;
    std::vector<OrbisNgs2VoicePortInfo> ports;
    std::vector<OrbisNgs2VoiceMatrixInfo> matrices;
    std::vector<float> audio;
    std::array<float, 8> input_peak{};
    std::array<float, 8> output_peak{};
    float gain{1.0f};
    float lfe_gain{1.0f};
    float limiter_threshold{1.0f};
    bool limiter_enabled{};
    float peak{};
    float limiter_pressure{};
    std::vector<OrbisNgs2EnvelopePoint> envelope;
    u32 forward_points{};
    u32 envelope_point{};
    u32 envelope_sample{};
    float envelope_start{1.0f};
    float envelope_height{1.0f};
    bool releasing{};
    OrbisNgs2VoiceCallbackHandler callback{};
    uintptr_t callback_data{};
    u32 callback_flags{};
    OrbisNgs2UserFxProcessHandler user_fx{};
    std::array<uintptr_t, 3> user_fx_data{};
    void* decoder{};
    Atrac9CodecInfo codec_info{};
    std::vector<float> decoded_frame;
    u32 frame_sample{};
    u32 encoded_offset{};
    u32 skip_samples{};

    ~Voice() {
        if (decoder) {
            Atrac9ReleaseHandle(decoder);
        }
    }
};

struct Rack {
    OrbisNgs2Handle handle{};
    std::weak_ptr<System> system;
    OrbisNgs2RackOption option{};
    OrbisNgs2ContextBufferInfo buffer{};
    OrbisNgs2BufferFreeHandler free_handler{};
    uintptr_t user_data{};
    u32 id{};
    bool alive{true};
    u32 max_channels{8};
    u32 max_blocks{64};
    std::vector<std::shared_ptr<Voice>> voices;
    OrbisNgs2CustomRackOption custom{};
};

struct System {
    OrbisNgs2Handle handle{};
    OrbisNgs2SystemOption option{};
    OrbisNgs2ContextBufferInfo buffer{};
    OrbisNgs2BufferFreeHandler free_handler{};
    uintptr_t user_data{};
    std::recursive_mutex mutex;
    u32 public_locks{};
    bool alive{true};
    bool rendering{};
    u64 render_count{};
    std::vector<std::shared_ptr<Rack>> racks;
};

s32 FrameInfo(const OrbisNgs2WaveformFormat& format, u32& bytes, u32& samples, u32& units,
              u32& delay);
s32 SetupSampler(Voice& voice, const OrbisNgs2WaveformFormat& format);
s32 RenderSampler(Voice& voice, u32 samples, u32 sample_rate);
float ReadPcm(const u8* data, u32 type);
u32 PcmSize(u32 type);
void WritePcm(u8* data, u32 type, float sample);

} // namespace Libraries::Ngs2::Engine
