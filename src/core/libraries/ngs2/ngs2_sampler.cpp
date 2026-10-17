// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "core/libraries/kernel/file_system.h"
#include "ngs2_error.h"
#include "ngs2_runtime_types.h"

namespace Libraries::Ngs2::Engine {

u32 PcmSize(u32 type) {
    if (type < 0x10 || type > 0x1b)
        return 0;
    constexpr std::array<u32, 6> sizes{1, 2, 3, 4, 4, 8};
    return sizes[(type - 0x10) / 2];
}

float ReadPcm(const u8* data, u32 type) {
    const u32 size = PcmSize(type);
    u64 bits{};
    for (u32 index = 0; index < size; ++index) {
        const u32 shift = (type > 0x11 && (type & 1)) ? size - index - 1 : index;
        bits |= u64(data[index]) << (shift * 8);
    }
    if (type == 0x11)
        return (float(bits) - 128.0f) / 128.0f;
    if (type == 0x18 || type == 0x19)
        return std::bit_cast<float>(static_cast<u32>(bits));
    if (type == 0x1a || type == 0x1b)
        return static_cast<float>(std::bit_cast<double>(bits));
    const u32 shift = 64 - size * 8;
    return static_cast<float>(static_cast<s64>(bits << shift) >> shift) /
           std::ldexp(1.0f, size * 8 - 1);
}

void WritePcm(u8* data, u32 type, float sample) {
    if (!std::isfinite(sample))
        sample = 0;
    const u32 size = PcmSize(type);
    u64 bits;
    if (type == 0x18 || type == 0x19)
        bits = std::bit_cast<u32>(sample);
    else if (type == 0x1a || type == 0x1b)
        bits = std::bit_cast<u64>(double(sample));
    else if (type == 0x11)
        bits = static_cast<u8>(std::clamp(std::lround(sample * 128.0f + 128.0f), 0l, 255l));
    else {
        const double scale = std::ldexp(1.0, size * 8 - 1);
        bits = static_cast<u64>(
            static_cast<s64>(std::clamp(std::round(double(sample) * scale), -scale, scale - 1)));
    }
    for (u32 index = 0; index < size; ++index) {
        const u32 shift = (type > 0x11 && (type & 1)) ? size - index - 1 : index;
        data[index] = static_cast<u8>(bits >> (shift * 8));
    }
}

s32 FrameInfo(const OrbisNgs2WaveformFormat& format, u32& bytes, u32& samples, u32& units,
              u32& delay) {
    if (format.numChannels != 1 && format.numChannels != 2 && format.numChannels != 6 &&
        format.numChannels != 8)
        return ORBIS_NGS2_ERROR_INVALID_NUM_CHANNELS;
    if (!format.sampleRate || format.sampleRate > 192000)
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SAMPLE_RATE;
    if (const u32 size = PcmSize(format.waveformType)) {
        bytes = size * format.numChannels;
        samples = units = 1;
        delay = 0;
        return 0;
    }
    if (format.waveformType != 0x40)
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_TYPE;
    auto* decoder = Atrac9GetHandle();
    if (!decoder)
        return ORBIS_NGS2_ERROR_CODEC_EMPTY_INSTANCE;
    SCOPE_EXIT {
        Atrac9ReleaseHandle(decoder);
    };
    u8 config[4];
    const u32 config_word = std::byteswap(format.configData);
    std::memcpy(config, &config_word, sizeof(config));
    if (Atrac9InitDecoder(decoder, config) != 0)
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_CONFIG;
    Atrac9CodecInfo info{};
    Atrac9GetCodecInfo(decoder, &info);
    if (info.channels != format.numChannels || info.samplingRate != format.sampleRate)
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
    bytes = info.superframeSize;
    samples = info.frameSamples * info.framesInSuperframe;
    units = info.framesInSuperframe;
    delay = info.frameSamples;
    return 0;
}

s32 sceNgs2GetWaveformFrameInfo(const OrbisNgs2WaveformFormat* format, u32* bytes, u32* samples,
                                u32* units, u32* delay) {
    if (!format)
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
    u32 frame_bytes{}, frame_samples{}, frame_units{}, frame_delay{};
    if (const auto error = FrameInfo(*format, frame_bytes, frame_samples, frame_units, frame_delay);
        error < 0)
        return error;
    if (bytes)
        *bytes = frame_bytes;
    if (samples)
        *samples = frame_samples;
    if (units)
        *units = frame_units;
    if (delay)
        *delay = frame_delay;
    return 0;
}

s32 sceNgs2CalcWaveformBlock(const OrbisNgs2WaveformFormat* format, u32 position, u32 count,
                             OrbisNgs2WaveformBlock* output) {
    if (!format)
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
    if (!output)
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    u32 bytes{}, samples{}, units{}, delay{};
    if (const auto error = FrameInfo(*format, bytes, samples, units, delay); error < 0)
        return error;
    const u64 start = u64(position) + format->frameOffset;
    const u64 offset = start / samples * bytes;
    const u64 skip = start % samples;
    const u64 size = (skip + count + samples - 1) / samples * bytes;
    if (offset > UINT32_MAX || size > UINT32_MAX)
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE;
    *output = {
        static_cast<u32>(offset), static_cast<u32>(size), 0, static_cast<u32>(skip), count, 0, 0};
    return 0;
}

s32 SetupSampler(Voice& voice, const OrbisNgs2WaveformFormat& format) {
    LOG_DEBUG(Lib_Ngs2, "Sampler format {:#x}, channels {}, rate {}, config {:#x}",
              format.waveformType, format.numChannels, format.sampleRate, format.configData);
    u32 bytes{}, samples{}, units{}, delay{};
    if (const auto error = FrameInfo(format, bytes, samples, units, delay); error < 0)
        return error;
    if (voice.decoder) {
        Atrac9ReleaseHandle(voice.decoder);
        voice.decoder = nullptr;
    }
    if (format.waveformType == 0x40) {
        voice.decoder = Atrac9GetHandle();
        if (!voice.decoder)
            return ORBIS_NGS2_ERROR_CODEC_EMPTY_INSTANCE;
        u8 config[4];
        const u32 config_word = std::byteswap(format.configData);
        std::memcpy(config, &config_word, sizeof(config));
        if (Atrac9InitDecoder(voice.decoder, config) != 0)
            return ORBIS_NGS2_ERROR_CODEC_SETUP_FAIL;
        Atrac9GetCodecInfo(voice.decoder, &voice.codec_info);
    }
    voice.format = format;
    voice.channels = format.numChannels;
    voice.initialized = true;
    voice.flags = VoiceEmpty;
    voice.blocks.clear();
    voice.decoded_frame.clear();
    voice.position = 0;
    voice.decoded_samples = voice.decoded_bytes = 0;
    voice.encoded_offset = voice.frame_sample = voice.skip_samples = 0;
    voice.waveform_data = nullptr;
    voice.block_user_data = 0;
    voice.exit_loop = false;
    return 0;
}

namespace {
void AdvanceBlock(Voice& voice) {
    auto& block = voice.blocks.front();
    if (!voice.exit_loop &&
        (block.info.numRepeats == UINT32_MAX || block.repeated < block.info.numRepeats)) {
        ++block.repeated;
    } else {
        voice.blocks.pop_front();
        voice.exit_loop = false;
    }
    voice.position = 0;
    voice.encoded_offset = voice.frame_sample = voice.skip_samples = 0;
    voice.decoded_frame.clear();
    if (voice.blocks.empty())
        voice.flags = VoiceInUse | VoiceStopped | VoiceEmpty;
}
} // namespace

s32 RenderSampler(Voice& voice, u32 samples, u32 sample_rate) {
    const double step = double(voice.format.sampleRate) * voice.pitch / sample_rate;
    const u32 pcm_size = PcmSize(voice.format.waveformType);
    for (u32 output = 0; output < samples; ++output) {
        while (!voice.blocks.empty()) {
            const auto& block = voice.blocks.front();
            const u64 available =
                pcm_size ? block.info.dataSize / (pcm_size * voice.channels)
                         : u64(block.info.dataSize) / voice.codec_info.superframeSize *
                               voice.codec_info.frameSamples * voice.codec_info.framesInSuperframe;
            const u64 count = block.info.numSamples
                                  ? block.info.numSamples
                                  : available - std::min<u64>(available, block.info.numSkipSamples);
            if (!count || count + block.info.numSkipSamples > available)
                return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE;
            if (voice.position < count)
                break;
            const double remainder = voice.position - count;
            AdvanceBlock(voice);
            voice.position = remainder;
        }
        if (voice.blocks.empty())
            break;
        const auto& block = voice.blocks.front();
        const auto* data = reinterpret_cast<const u8*>(reinterpret_cast<uintptr_t>(block.data) +
                                                       block.info.dataOffset);
        voice.waveform_data = data;
        voice.block_user_data = block.info.userData;
        const u32 position = static_cast<u32>(voice.position) + block.info.numSkipSamples;
        if (pcm_size) {
            const u32 count = block.info.dataSize / (pcm_size * voice.channels);
            const u32 last_sample =
                block.info.numSamples
                    ? std::min(count, block.info.numSkipSamples + block.info.numSamples)
                    : count;
            const u32 next = std::min(position + 1, last_sample - 1);
            const float fraction = static_cast<float>(voice.position - std::floor(voice.position));
            for (u32 channel = 0; channel < voice.channels; ++channel) {
                const float a = ReadPcm(data + (position * voice.channels + channel) * pcm_size,
                                        voice.format.waveformType);
                const float b = ReadPcm(data + (next * voice.channels + channel) * pcm_size,
                                        voice.format.waveformType);
                voice.audio[output * voice.channels + channel] = a + (b - a) * fraction;
            }
            voice.decoded_bytes +=
                static_cast<u64>(std::floor(voice.position + step) - std::floor(voice.position)) *
                pcm_size * voice.channels;
        } else {
            const u32 frame = position / voice.codec_info.frameSamples;
            while (voice.decoded_frame.empty() || voice.frame_sample <= frame) {
                if (voice.encoded_offset >= block.info.dataSize)
                    return ORBIS_NGS2_ERROR_CODEC_DECODE_FAIL;
                voice.decoded_frame.resize(voice.codec_info.frameSamples * voice.channels);
                int consumed{};
                const auto result = Atrac9DecodeF32(
                    voice.decoder, data + voice.encoded_offset,
                    static_cast<int>(std::min<u32>(block.info.dataSize - voice.encoded_offset,
                                                   voice.codec_info.superframeSize)),
                    voice.decoded_frame.data(), &consumed, 0);
                if (result || consumed <= 0)
                    return ORBIS_NGS2_ERROR_CODEC_DECODE_FAIL;
                voice.encoded_offset += consumed;
                voice.decoded_bytes += consumed;
                ++voice.frame_sample;
                if (voice.frame_sample % voice.codec_info.framesInSuperframe == 0) {
                    voice.encoded_offset = voice.frame_sample /
                                           voice.codec_info.framesInSuperframe *
                                           voice.codec_info.superframeSize;
                }
            }
            for (u32 channel = 0; channel < voice.channels; ++channel)
                voice.audio[output * voice.channels + channel] =
                    voice
                        .decoded_frame[(position % voice.codec_info.frameSamples) * voice.channels +
                                       channel];
        }
        voice.decoded_samples +=
            static_cast<u64>(std::floor(voice.position + step) - std::floor(voice.position));
        voice.position += step;
    }
    return 0;
}

namespace {
using Reader = std::function<s32(u32, void*, size_t)>;

s32 ParseWaveform(const Reader& read, OrbisNgs2WaveformInfo* output) {
    if (!output)
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::array<u32, 3> riff{};
    if (read(0, riff.data(), sizeof(riff)) < 0 || riff[0] != 0x46464952 || riff[2] != 0x45564157)
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA;
    OrbisNgs2WaveformInfo info{};
    bool has_format{}, has_data{};
    const u64 end = u64(riff[1]) + 8;
    if (end < sizeof(riff) || end > UINT32_MAX)
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE;
    u8 last_byte{};
    if (read(static_cast<u32>(end - 1), &last_byte, 1) < 0)
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE;
    for (u64 offset = 12; offset + 8 <= end;) {
        std::array<u32, 2> chunk{};
        if (read(static_cast<u32>(offset), chunk.data(), sizeof(chunk)) < 0)
            return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA;
        const u64 body = offset + 8;
        if (body + chunk[1] > end || body > UINT32_MAX)
            return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE;
        if (chunk[0] == 0x20746d66) {
            std::array<u8, 52> fmt{};
            if (chunk[1] < 16 || read(static_cast<u32>(body), fmt.data(),
                                      std::min<size_t>(fmt.size(), chunk[1])) < 0)
                return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA;
            u16 tag{}, channels{}, bits{};
            std::memcpy(&tag, fmt.data(), 2);
            std::memcpy(&channels, fmt.data() + 2, 2);
            std::memcpy(&info.format.sampleRate, fmt.data() + 4, 4);
            std::memcpy(&bits, fmt.data() + 14, 2);
            info.format.numChannels = channels;
            if (tag == 0xfffe && chunk[1] >= 40) {
                if (fmt[24] == 0xd2 && fmt[25] == 0x42 && chunk[1] >= 52) {
                    info.format.waveformType = 0x40;
                    std::memcpy(&info.format.configData, fmt.data() + 44, 4);
                    info.format.configData = std::byteswap(info.format.configData);
                } else
                    std::memcpy(&tag, fmt.data() + 24, 2);
            }
            if (!info.format.waveformType) {
                if (tag == 1 && bits == 8)
                    info.format.waveformType = 0x11;
                else if (tag == 1 && (bits == 16 || bits == 24 || bits == 32))
                    info.format.waveformType = 0x12 + (bits - 16) / 4;
                else if (tag == 3 && (bits == 32 || bits == 64))
                    info.format.waveformType = bits == 32 ? 0x18 : 0x1a;
                else
                    return ORBIS_NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
            }
            has_format = true;
        } else if (chunk[0] == 0x61746164) {
            info.dataOffset = static_cast<u32>(body);
            info.dataSize = chunk[1];
            has_data = true;
        } else if (chunk[0] == 0x74636166 && chunk[1] >= 8) {
            std::array<u32, 2> fact{};
            if (read(static_cast<u32>(body), fact.data(), sizeof(fact)) < 0)
                return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA;
            info.numSamples = fact[0];
            info.format.frameOffset = fact[1];
        }
        offset = body + chunk[1] + (chunk[1] & 1);
    }
    if (!has_format || !has_data)
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA;
    if (const auto error = FrameInfo(info.format, info.audioFrameSize, info.numAudioFrameSamples,
                                     info.numAudioUnitPerFrame, info.numDelaySamples);
        error < 0)
        return error;
    info.audioUnitSize = info.audioFrameSize / info.numAudioUnitPerFrame;
    info.numAudioUnitSamples = info.numAudioFrameSamples / info.numAudioUnitPerFrame;
    if (!info.numSamples)
        info.numSamples = info.dataSize / info.audioFrameSize * info.numAudioFrameSamples;
    info.numBlocks = 1;
    info.aBlock[0] = {
        info.dataOffset, info.dataSize, 0, info.format.frameOffset, info.numSamples, 0, 0};
    *output = info;
    return 0;
}
} // namespace

s32 sceNgs2ParseWaveformData(const void* data, size_t size, OrbisNgs2WaveformInfo* output) {
    if (!data)
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_ADDRESS;
    return ParseWaveform(
        [&](u32 offset, void* buffer, size_t bytes) -> s32 {
            if (offset > size || bytes > size - offset)
                return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE;
            std::memcpy(buffer, static_cast<const u8*>(data) + offset, bytes);
            return 0;
        },
        output);
}

s32 sceNgs2ParseWaveformUser(OrbisNgs2ParseReadHandler handler, uintptr_t data,
                             OrbisNgs2WaveformInfo* output) {
    if (!handler)
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_ADDRESS;
    return ParseWaveform(
        [&](u32 offset, void* buffer, size_t size) { return handler(data, offset, buffer, size); },
        output);
}

s32 sceNgs2ParseWaveformFile(const char* path, u64 offset, OrbisNgs2WaveformInfo* output) {
    if (!path)
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_ADDRESS;
    const auto fd = Kernel::posix_open(path, Kernel::ORBIS_KERNEL_O_RDONLY, 0);
    if (fd < 0)
        return fd;
    SCOPE_EXIT {
        Kernel::posix_close(fd);
    };
    return ParseWaveform(
        [&](u32 position, void* buffer, size_t size) -> s32 {
            if (offset > INT64_MAX - position)
                return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE;
            const auto result =
                Kernel::sceKernelPread(fd, buffer, size, static_cast<s64>(offset + position));
            return result == static_cast<s64>(size) ? 0 : ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA;
        },
        output);
}

} // namespace Libraries::Ngs2::Engine
