// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "ngs2_error.h"
#include "ngs2_mastering.h"
#include "ngs2_runtime_types.h"
#include "ngs2_submixer.h"

namespace Libraries::Ngs2::Engine {
namespace {
std::mutex registry_mutex;
OrbisNgs2Handle next_handle{1};
std::unordered_map<OrbisNgs2Handle, std::shared_ptr<System>> systems;
std::unordered_map<OrbisNgs2Handle, std::shared_ptr<Rack>> racks;
std::unordered_map<OrbisNgs2Handle, std::shared_ptr<Voice>> voices;
thread_local std::unordered_map<OrbisNgs2Handle, u32> public_locks;

bool ValidRate(u32 rate) {
    constexpr std::array rates{11025u, 12000u, 22050u, 24000u,  44100u,
                               48000u, 88200u, 96000u, 176400u, 192000u};
    return std::ranges::find(rates, rate) != rates.end();
}

bool ValidGrain(u32 grain) {
    return grain >= 64 && grain <= 1024 && grain % 64 == 0;
}

bool ValidChannels(u32 channels) {
    return channels == 1 || channels == 2 || channels == 6 || channels == 8;
}

template <class T>
std::shared_ptr<T> Lookup(const std::unordered_map<OrbisNgs2Handle, std::shared_ptr<T>>& map,
                          OrbisNgs2Handle handle) {
    std::scoped_lock lock(registry_mutex);
    const auto it = map.find(handle);
    return it == map.end() ? nullptr : it->second;
}

s32 CheckBuffer(const OrbisNgs2ContextBufferInfo* buffer, size_t required) {
    if (!buffer)
        return ORBIS_NGS2_ERROR_INVALID_BUFFER_INFO;
    if (!buffer->hostBuffer)
        return ORBIS_NGS2_ERROR_INVALID_BUFFER_ADDRESS;
    if (reinterpret_cast<uintptr_t>(buffer->hostBuffer) % 8)
        return ORBIS_NGS2_ERROR_INVALID_BUFFER_ALIGN;
    if (buffer->hostBufferSize < required)
        return ORBIS_NGS2_ERROR_INVALID_BUFFER_SIZE;
    return 0;
}

s32 SystemOption(const OrbisNgs2SystemOption* input, OrbisNgs2SystemOption& option) {
    option = {sizeof(option), "", 0, 512, 256, 48000, {}};
    if (input) {
        if (input->size != sizeof(option))
            return ORBIS_NGS2_ERROR_INVALID_OPTION_SIZE;
        option = *input;
    }
    if (!ValidGrain(option.maxGrainSamples))
        return ORBIS_NGS2_ERROR_INVALID_MAX_GRAIN_SAMPLES;
    if (!ValidGrain(option.numGrainSamples) || option.numGrainSamples > option.maxGrainSamples)
        return ORBIS_NGS2_ERROR_INVALID_NUM_GRAIN_SAMPLES;
    if (!ValidRate(option.sampleRate))
        return ORBIS_NGS2_ERROR_INVALID_SAMPLE_RATE;
    return 0;
}

s32 RackOption(u32 id, const OrbisNgs2RackOption* input, OrbisNgs2RackOption& option) {
    if (id != 0x1000 && id != 0x2000 && id != 0x2001 && id != 0x2002 && id != 0x3000 &&
        id != 0x4001 && id != 0x4002 && id != 0x4003)
        return ORBIS_NGS2_ERROR_INVALID_RACK_ID;
    option = {sizeof(option), "", 0, 512, 1, 0, 1, 1, {}};
    if (input) {
        if (input->size < sizeof(option))
            return ORBIS_NGS2_ERROR_INVALID_OPTION_SIZE;
        option = *input;
    }
    if (!ValidGrain(option.maxGrainSamples))
        return ORBIS_NGS2_ERROR_INVALID_MAX_GRAIN_SAMPLES;
    if (option.maxVoices > 4096)
        return ORBIS_NGS2_ERROR_INVALID_MAX_VOICES;
    if (option.maxPorts > 16)
        return ORBIS_NGS2_ERROR_INVALID_MAX_PORTS;
    if (option.maxMatrices > 64)
        return ORBIS_NGS2_ERROR_INVALID_MAX_MATRICES;
    return 0;
}

s32 CreateSystem(const OrbisNgs2SystemOption* option, const OrbisNgs2ContextBufferInfo* buffer,
                 OrbisNgs2BufferFreeHandler free_handler, OrbisNgs2Handle* output) {
    if (!output)
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    *output = 0;
    OrbisNgs2SystemOption checked;
    if (const auto error = SystemOption(option, checked); error < 0)
        return error;
    if (const auto error = CheckBuffer(buffer, sizeof(System)); error < 0)
        return error;
    auto system = std::make_shared<System>();
    system->option = checked;
    system->buffer = *buffer;
    system->free_handler = free_handler;
    {
        std::scoped_lock lock(registry_mutex);
        system->handle = next_handle++;
        systems.emplace(system->handle, system);
    }
    *output = system->handle;
    return 0;
}

s32 CreateRack(OrbisNgs2Handle system_handle, u32 id, const OrbisNgs2RackOption* option,
               const OrbisNgs2ContextBufferInfo* buffer, OrbisNgs2BufferFreeHandler free_handler,
               OrbisNgs2Handle* output) {
    if (!output)
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    *output = 0;
    const auto system = Lookup(systems, system_handle);
    if (!system)
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    OrbisNgs2RackOption checked;
    if (const auto error = RackOption(id, option, checked); error < 0)
        return error;
    if (const auto error = CheckBuffer(buffer, sizeof(Rack) + checked.maxVoices * sizeof(Voice));
        error < 0)
        return error;
    std::scoped_lock lock(system->mutex);
    if (!system->alive)
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    if (system->rendering)
        return ORBIS_NGS2_ERROR_INVALID_OPERATION;
    if (checked.maxGrainSamples < system->option.numGrainSamples)
        return ORBIS_NGS2_ERROR_INVALID_MAX_GRAIN_SAMPLES;
    auto rack = std::make_shared<Rack>();
    rack->id = id;
    rack->option = checked;
    rack->buffer = *buffer;
    rack->free_handler = free_handler;
    rack->system = system;
    if (option && id >= 0x4001 && option->size >= sizeof(OrbisNgs2CustomRackOption)) {
        rack->custom = *reinterpret_cast<const OrbisNgs2CustomRackOption*>(option);
        if (rack->custom.numModules > ORBIS_NGS2_CUSTOM_MAX_MODULES)
            return ORBIS_NGS2_ERROR_INVALID_NUM_MODULES;
    }
    if (option && id == 0x1000 && option->size >= sizeof(OrbisNgs2SamplerRackOption)) {
        rack->max_blocks =
            reinterpret_cast<const OrbisNgs2SamplerRackOption*>(option)->maxWaveformBlocks;
    }
    rack->voices.reserve(checked.maxVoices);
    for (u32 index = 0; index < checked.maxVoices; ++index) {
        auto voice = std::make_shared<Voice>();
        voice->rack = rack;
        voice->index = index;
        voice->ports.resize(checked.maxPorts);
        for (auto& port : voice->ports) {
            port.matrixId = -1;
            port.volume = 1.0f;
        }
        voice->matrices.resize(checked.maxMatrices);
        voice->audio.resize(checked.maxGrainSamples * 8);
        rack->voices.push_back(std::move(voice));
    }
    {
        std::scoped_lock registry_lock(registry_mutex);
        rack->handle = next_handle++;
        racks.emplace(rack->handle, rack);
        for (auto& voice : rack->voices) {
            voice->handle = next_handle++;
            voices.emplace(voice->handle, voice);
        }
    }
    system->racks.push_back(rack);
    *output = rack->handle;
    LOG_INFO(Lib_Ngs2, "Created rack {:#x}: {} voices", id, checked.maxVoices);
    return 0;
}

void RemoveRack(const std::shared_ptr<Rack>& rack) {
    rack->alive = false;
    std::scoped_lock lock(registry_mutex);
    for (const auto& voice : rack->voices)
        voices.erase(voice->handle);
    racks.erase(rack->handle);
}

template <class T>
const T* Parameter(const OrbisNgs2VoiceParamHeader* header) {
    return header->size >= sizeof(T) ? reinterpret_cast<const T*>(header) : nullptr;
}

s32 Event(Voice& voice, u32 event) {
    if (event > 5)
        return ORBIS_NGS2_ERROR_INVALID_EVENT_TYPE;
    if (!voice.initialized)
        return ORBIS_NGS2_ERROR_UNINIT_VOICE;
    switch (event) {
    case 0:
        voice.flags = VoiceInUse | VoicePlaying | (voice.blocks.empty() ? VoiceEmpty : 0);
        voice.releasing = false;
        voice.envelope_point = voice.envelope_sample = 0;
        voice.envelope_height = voice.envelope.empty() ? 1.0f : 0.0f;
        voice.envelope_start = voice.envelope_height;
        break;
    case 1:
        if (voice.forward_points < voice.envelope.size()) {
            voice.releasing = true;
            voice.envelope_point = voice.forward_points;
            voice.envelope_sample = 0;
            voice.envelope_start = voice.envelope_height;
        } else {
            voice.flags = VoiceInUse | VoiceStopped;
        }
        break;
    case 2:
        voice.flags = VoiceInUse | VoiceStopped;
        break;
    case 3:
        voice.flags = VoiceEmpty;
        voice.blocks.clear();
        voice.position = 0;
        voice.decoded_frame.clear();
        break;
    case 4:
        voice.flags |= VoicePaused;
        break;
    case 5:
        voice.flags &= ~VoicePaused;
        break;
    }
    return 0;
}

s32 ControlCommon(Voice& voice, const OrbisNgs2VoiceParamHeader* header) {
    const u32 id = header->id & 0xffff;
    if (id == 1) {
        const auto* param = Parameter<OrbisNgs2VoiceMatrixLevelsParam>(header);
        if (!param)
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        if (param->matrixId >= voice.matrices.size())
            return ORBIS_NGS2_ERROR_INVALID_MATRIX_INDEX;
        if (param->numLevels > 64)
            return ORBIS_NGS2_ERROR_INVALID_NUM_MATRIX_LEVELS;
        if (param->numLevels && !param->aLevel)
            return ORBIS_NGS2_ERROR_INVALID_MATRIX_LEVEL_ADDRESS;
        auto& matrix = voice.matrices[param->matrixId];
        matrix = {};
        matrix.numLevels = param->numLevels;
        if (param->numLevels)
            std::copy_n(param->aLevel, param->numLevels, matrix.aLevel);
        return 0;
    }
    if (id == 6) {
        const auto* param = Parameter<OrbisNgs2VoiceEventParam>(header);
        return param ? Event(voice, param->eventId) : ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
    }
    if (id == 7) {
        const auto* param = Parameter<OrbisNgs2VoiceCallbackParam>(header);
        if (!param)
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        voice.callback = param->callbackHandler;
        voice.callback_data = param->callbackData;
        voice.callback_flags = param->flags;
        return 0;
    }
    if (id < 2 || id > 5)
        return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ID;
    if (header->size < sizeof(OrbisNgs2VoicePortVolumeParam))
        return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
    const auto port_index = reinterpret_cast<const OrbisNgs2VoicePortVolumeParam*>(header)->port;
    if (port_index >= voice.ports.size())
        return ORBIS_NGS2_ERROR_INVALID_PORT_INDEX;
    auto& port = voice.ports[port_index];
    switch (id) {
    case 2: {
        const auto* param = Parameter<OrbisNgs2VoicePortVolumeParam>(header);
        if (!std::isfinite(param->level))
            return ORBIS_NGS2_ERROR_INVALID_OPERATION;
        port.volume = param->level;
        return 0;
    }
    case 3: {
        const auto* param = Parameter<OrbisNgs2VoicePortMatrixParam>(header);
        if (param->matrixId < -1 || param->matrixId >= static_cast<s32>(voice.matrices.size()))
            return ORBIS_NGS2_ERROR_INVALID_MATRIX_INDEX;
        port.matrixId = param->matrixId;
        return 0;
    }
    case 4: {
        const auto* param = Parameter<OrbisNgs2VoicePortDelayParam>(header);
        if (param->numSamples)
            return ORBIS_NGS2_ERROR_INVALID_OPERATION;
        port.numDelaySamples = 0;
        return 0;
    }
    case 5: {
        const auto* param = Parameter<OrbisNgs2VoicePatchParam>(header);
        if (!param)
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        if (param->destHandle) {
            const auto destination = Lookup(voices, param->destHandle);
            const auto source_rack = voice.rack.lock();
            const auto dest_rack = destination ? destination->rack.lock() : nullptr;
            if (!dest_rack || source_rack->system.lock() != dest_rack->system.lock() ||
                destination->handle == voice.handle)
                return ORBIS_NGS2_ERROR_INVALID_PATCH;
        }
        port.destHandle = param->destHandle;
        port.destInputId = param->destInputId;
        return 0;
    }
    }
    return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ID;
}

s32 SetEnvelope(Voice& voice, const OrbisNgs2VoiceParamHeader* header) {
    const auto* param = Parameter<OrbisNgs2SamplerVoiceEnvelopeParam>(header);
    if (!param)
        return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
    const u64 count = u64(param->numForwardPoints) + param->numReleasePoints;
    if (count > 4096)
        return ORBIS_NGS2_ERROR_INVALID_NUM_ENVELOPE_POINTS;
    if (count && !param->aPoint)
        return ORBIS_NGS2_ERROR_INVALID_ENVELOPE_POINT_ADDRESS;
    for (u32 index = 0; index < count; ++index) {
        if (param->aPoint[index].curve > 1)
            return ORBIS_NGS2_ERROR_INVALID_ENVELOPE_CURVE;
        if (!std::isfinite(param->aPoint[index].height))
            return ORBIS_NGS2_ERROR_INVALID_OPERATION;
    }
    voice.envelope.clear();
    if (count)
        voice.envelope.assign(param->aPoint, param->aPoint + count);
    voice.forward_points = param->numForwardPoints;
    voice.envelope_point = voice.envelope_sample = 0;
    voice.envelope_start = voice.envelope_height;
    return 0;
}

s32 SetUserFx(Voice& voice, const OrbisNgs2VoiceParamHeader* header) {
    const auto* param = Parameter<OrbisNgs2SamplerVoiceUserFxParam>(header);
    if (!param)
        return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
    voice.user_fx = param->handler;
    voice.user_fx_data = {param->userData0, param->userData1, param->userData2};
    return 0;
}

s32 ControlSampler(Voice& voice, const OrbisNgs2VoiceParamHeader* header) {
    switch (header->id & 0xffff) {
    case 0: {
        const auto* param = Parameter<OrbisNgs2SamplerVoiceSetupParam>(header);
        return param ? SetupSampler(voice, param->format)
                     : ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
    }
    case 1: {
        const auto* param = Parameter<OrbisNgs2SamplerVoiceWaveformBlocksParam>(header);
        if (!param)
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        const auto rack = voice.rack.lock();
        if (param->numBlocks > rack->max_blocks ||
            voice.blocks.size() + param->numBlocks > rack->max_blocks)
            return ORBIS_NGS2_ERROR_INVALID_NUM_WAVEFORM_BLOCKS;
        if (param->numBlocks && !param->aBlock)
            return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_BLOCK_ADDRESS;
        for (u32 index = 0; index < param->numBlocks; ++index) {
            const auto& block = param->aBlock[index];
            if (!param->data && !block.dataOffset)
                return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_ADDRESS;
            if (!block.dataSize)
                return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE;
            voice.blocks.push_back({static_cast<const u8*>(param->data), block});
        }
        voice.flags &= ~VoiceEmpty;
        return 0;
    }
    case 2: {
        const auto* param = Parameter<OrbisNgs2SamplerVoiceWaveformAddressParam>(header);
        if (!param)
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        for (auto& block : voice.blocks) {
            if (block.data == param->from)
                block.data = static_cast<const u8*>(param->to);
        }
        return 0;
    }
    case 3: {
        const auto* param = Parameter<OrbisNgs2SamplerVoiceWaveformFrameOffsetParam>(header);
        if (!param)
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        voice.format.frameOffset = param->frameOffset;
        return 0;
    }
    case 4:
        voice.exit_loop = true;
        return 0;
    case 5: {
        const auto* param = Parameter<OrbisNgs2SamplerVoicePitchParam>(header);
        if (!param)
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        if (!std::isfinite(param->ratio) || param->ratio <= 0 || param->ratio > 16)
            return ORBIS_NGS2_ERROR_INVALID_OPERATION;
        voice.pitch = param->ratio;
        return 0;
    }
    case 6:
        return SetEnvelope(voice, header);
    case 8:
        return SetUserFx(voice, header);
    case 9: {
        const auto* param = Parameter<OrbisNgs2SamplerVoicePeakMeterParam>(header);
        if (!param)
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        return param->enableFlag <= 1 ? 0 : ORBIS_NGS2_ERROR_INVALID_PEAKMETER_FLAG;
    }
    default:
        return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ID;
    }
}

s32 ControlMixer(Voice& voice, const OrbisNgs2VoiceParamHeader* header, bool mastering) {
    switch (header->id & 0xffff) {
    case 0: {
        const auto* param = Parameter<OrbisNgs2SubmixerVoiceSetupParam>(header);
        if (!param)
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        if (!ValidChannels(param->numIoChannels))
            return ORBIS_NGS2_ERROR_INVALID_NUM_CHANNELS;
        voice.channels = voice.input_channels = param->numIoChannels;
        voice.initialized = true;
        return 0;
    }
    case 1:
        if (!mastering)
            return SetEnvelope(voice, header);
        return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ID;
    case 3:
        if (mastering) {
            const auto* param = Parameter<OrbisNgs2MasteringVoiceLimiterParam>(header);
            if (!param)
                return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
            if (!std::isfinite(param->threshold) || param->threshold <= 0)
                return ORBIS_NGS2_ERROR_INVALID_OPERATION;
            voice.limiter_enabled = param->enableFlag != 0;
            voice.limiter_threshold = param->threshold;
            return 0;
        }
        return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ID;
    case 4:
        if (!mastering)
            return SetUserFx(voice, header);
        if (const auto* param = Parameter<OrbisNgs2MasteringVoiceGainParam>(header)) {
            if (!std::isfinite(param->fbwLevel) || !std::isfinite(param->lfeLevel))
                return ORBIS_NGS2_ERROR_INVALID_OPERATION;
            voice.gain = param->fbwLevel;
            voice.lfe_gain = param->lfeLevel;
            return 0;
        }
        return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
    case 5:
        if (mastering) {
            const auto* param = Parameter<OrbisNgs2MasteringVoiceOutputParam>(header);
            if (!param)
                return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
            voice.output_id = param->outputId;
            return 0;
        }
        [[fallthrough]];
    case 6: {
        const auto* param = Parameter<OrbisNgs2SamplerVoicePeakMeterParam>(header);
        if (!param)
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        return param->enableFlag <= 1 ? 0 : ORBIS_NGS2_ERROR_INVALID_PEAKMETER_FLAG;
    }
    default:
        return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ID;
    }
}

void ApplyEnvelope(Voice& voice, u32 samples) {
    for (u32 sample = 0; sample < samples; ++sample) {
        const u32 end =
            voice.releasing ? static_cast<u32>(voice.envelope.size()) : voice.forward_points;
        if (voice.envelope_point < end) {
            const auto& point = voice.envelope[voice.envelope_point];
            const float ratio =
                point.duration ? std::min(1.0f, float(++voice.envelope_sample) / point.duration)
                               : 1.0f;
            voice.envelope_height =
                point.curve == 0
                    ? point.height
                    : voice.envelope_start + (point.height - voice.envelope_start) * ratio;
            if (ratio == 1.0f) {
                voice.envelope_start = voice.envelope_height;
                voice.envelope_sample = 0;
                ++voice.envelope_point;
                if (voice.releasing && voice.envelope_point == end)
                    voice.flags = VoiceInUse | VoiceStopped;
            }
        }
        for (u32 channel = 0; channel < voice.channels; ++channel)
            voice.audio[sample * voice.channels + channel] *= voice.envelope_height;
    }
}

void Mix(const Voice& source, Voice& destination, const OrbisNgs2VoicePortInfo& port, u32 samples) {
    const OrbisNgs2VoiceMatrixInfo* matrix =
        port.matrixId >= 0 ? &source.matrices[port.matrixId] : nullptr;
    for (u32 frame = 0; frame < samples; ++frame) {
        for (u32 out = 0; out < destination.channels; ++out) {
            float value{};
            for (u32 in = 0; in < source.channels; ++in) {
                float level = in == out || source.channels == 1 ? 1.0f : 0.0f;
                if (matrix) {
                    const u32 index = in * destination.channels + out;
                    level = index < matrix->numLevels ? matrix->aLevel[index] : 0.0f;
                }
                value += source.audio[frame * source.channels + in] * level;
            }
            destination.audio[frame * destination.channels + out] += value * port.volume;
        }
    }
}

} // namespace

s32 sceNgs2SystemResetOption(OrbisNgs2SystemOption* option) {
    if (!option)
        return ORBIS_NGS2_ERROR_INVALID_OPTION_ADDRESS;
    return SystemOption(nullptr, *option);
}

s32 sceNgs2SystemQueryBufferSize(const OrbisNgs2SystemOption* option,
                                 OrbisNgs2ContextBufferInfo* output) {
    if (!output)
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    OrbisNgs2SystemOption checked;
    if (const auto error = SystemOption(option, checked); error < 0)
        return error;
    *output = {};
    output->hostBufferSize = sizeof(System);
    return 0;
}

s32 sceNgs2SystemCreate(const OrbisNgs2SystemOption* option,
                        const OrbisNgs2ContextBufferInfo* buffer, OrbisNgs2Handle* output) {
    return CreateSystem(option, buffer, nullptr, output);
}

s32 sceNgs2SystemCreateWithAllocator(const OrbisNgs2SystemOption* option,
                                     const OrbisNgs2BufferAllocator* allocator,
                                     OrbisNgs2Handle* output) {
    if (!allocator || !allocator->allocHandler)
        return ORBIS_NGS2_ERROR_INVALID_BUFFER_ALLOCATOR;
    if (!output)
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    OrbisNgs2ContextBufferInfo buffer{};
    if (const auto error = sceNgs2SystemQueryBufferSize(option, &buffer); error < 0)
        return error;
    buffer.userData = allocator->userData;
    if (const auto error = allocator->allocHandler(&buffer); error < 0)
        return error;
    const auto result = CreateSystem(option, &buffer, allocator->freeHandler, output);
    if (result < 0 && allocator->freeHandler)
        allocator->freeHandler(&buffer);
    return result;
}

s32 sceNgs2RackQueryBufferSize(u32 id, const OrbisNgs2RackOption* option,
                               OrbisNgs2ContextBufferInfo* output) {
    if (!output)
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    OrbisNgs2RackOption checked;
    if (const auto error = RackOption(id, option, checked); error < 0)
        return error;
    *output = {};
    output->hostBufferSize = sizeof(Rack) + checked.maxVoices * sizeof(Voice);
    return 0;
}

s32 sceNgs2RackCreate(OrbisNgs2Handle system, u32 id, const OrbisNgs2RackOption* option,
                      const OrbisNgs2ContextBufferInfo* buffer, OrbisNgs2Handle* output) {
    return CreateRack(system, id, option, buffer, nullptr, output);
}

s32 sceNgs2RackCreateWithAllocator(OrbisNgs2Handle system, u32 id,
                                   const OrbisNgs2RackOption* option,
                                   const OrbisNgs2BufferAllocator* allocator,
                                   OrbisNgs2Handle* output) {
    if (!Lookup(systems, system))
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    if (!allocator || !allocator->allocHandler)
        return ORBIS_NGS2_ERROR_INVALID_BUFFER_ALLOCATOR;
    if (!output)
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    OrbisNgs2ContextBufferInfo buffer{};
    if (const auto error = sceNgs2RackQueryBufferSize(id, option, &buffer); error < 0)
        return error;
    buffer.userData = allocator->userData;
    if (const auto error = allocator->allocHandler(&buffer); error < 0)
        return error;
    const auto result = CreateRack(system, id, option, &buffer, allocator->freeHandler, output);
    if (result < 0 && allocator->freeHandler)
        allocator->freeHandler(&buffer);
    return result;
}

s32 sceNgs2RackDestroy(OrbisNgs2Handle handle, OrbisNgs2ContextBufferInfo* output) {
    const auto rack = Lookup(racks, handle);
    const auto system = rack ? rack->system.lock() : nullptr;
    if (!system)
        return ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;
    {
        std::scoped_lock lock(system->mutex);
        if (!rack->alive)
            return ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;
        if (system->rendering || system->public_locks)
            return ORBIS_NGS2_ERROR_INVALID_OPERATION;
        RemoveRack(rack);
        std::erase(system->racks, rack);
        if (output)
            *output = rack->buffer;
    }
    return rack->free_handler ? rack->free_handler(&rack->buffer) : 0;
}

s32 sceNgs2SystemDestroy(OrbisNgs2Handle handle, OrbisNgs2ContextBufferInfo* output) {
    const auto system = Lookup(systems, handle);
    if (!system)
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    std::vector<std::shared_ptr<Rack>> removed;
    {
        std::scoped_lock lock(system->mutex);
        if (!system->alive)
            return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
        if (system->rendering || system->public_locks)
            return ORBIS_NGS2_ERROR_INVALID_OPERATION;
        system->alive = false;
        removed.swap(system->racks);
        for (const auto& rack : removed)
            RemoveRack(rack);
        if (output)
            *output = system->buffer;
        std::scoped_lock registry_lock(registry_mutex);
        systems.erase(handle);
    }
    for (auto& rack : removed)
        if (rack->free_handler)
            rack->free_handler(&rack->buffer);
    return system->free_handler ? system->free_handler(&system->buffer) : 0;
}

s32 sceNgs2RackGetVoiceHandle(OrbisNgs2Handle handle, u32 index, OrbisNgs2Handle* output) {
    if (!output)
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    const auto rack = Lookup(racks, handle);
    const auto system = rack ? rack->system.lock() : nullptr;
    if (!system)
        return ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;
    std::scoped_lock lock(system->mutex);
    if (!rack->alive)
        return ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;
    if (index >= rack->voices.size())
        return ORBIS_NGS2_ERROR_INVALID_VOICE_INDEX;
    *output = rack->voices[index]->handle;
    return 0;
}

s32 sceNgs2VoiceControl(OrbisNgs2Handle handle, const OrbisNgs2VoiceParamHeader* header) {
    const auto voice = Lookup(voices, handle);
    const auto rack = voice ? voice->rack.lock() : nullptr;
    const auto system = rack ? rack->system.lock() : nullptr;
    if (!system)
        return ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
    if (!header)
        return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ADDRESS;
    std::scoped_lock lock(system->mutex);
    if (!rack->alive)
        return ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
    std::unordered_set<const OrbisNgs2VoiceParamHeader*> visited;
    while (header) {
        if (visited.size() >= 4096 || !visited.insert(header).second)
            return ORBIS_NGS2_ERROR_DETECTED_CIRCULAR_VOICE_CONTROL;
        if (header->size < sizeof(*header))
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        const u32 type = header->id >> 16;
        s32 result;
        if (type == 0)
            result = ControlCommon(*voice, header);
        else if (type != rack->id)
            result = ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ID;
        else if (type == 0x1000 || type == 0x4001)
            result = ControlSampler(*voice, header);
        else if (type == 0x2000 || type == 0x3000)
            result = ControlMixer(*voice, header, type == 0x3000);
        else
            result = ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ID;
        if (result < 0) {
            LOG_ERROR(Lib_Ngs2, "Voice control {:#x}, size {}: {:#x}", header->id, header->size,
                      u32(result));
            return result;
        }
        if (!header->next)
            break;
        header = reinterpret_cast<const OrbisNgs2VoiceParamHeader*>(
            reinterpret_cast<const u8*>(header) + header->next);
    }
    return 0;
}

s32 sceNgs2VoiceGetState(OrbisNgs2Handle handle, OrbisNgs2VoiceState* output, size_t size) {
    if (!output)
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    const auto voice = Lookup(voices, handle);
    const auto rack = voice ? voice->rack.lock() : nullptr;
    const auto system = rack ? rack->system.lock() : nullptr;
    if (!system)
        return ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
    std::scoped_lock lock(system->mutex);
    if (!rack->alive)
        return ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
    if (size == sizeof(OrbisNgs2VoiceState)) {
        output->stateFlags = voice->flags;
    } else if (rack->id == 0x1000 && size == sizeof(OrbisNgs2SamplerVoiceState)) {
        OrbisNgs2SamplerVoiceState state{};
        state.voiceState.stateFlags = voice->flags;
        state.envelopeHeight = voice->envelope_height;
        state.peakHeight = voice->peak;
        state.numDecodedSamples = voice->decoded_samples;
        state.decodedDataSize = voice->decoded_bytes;
        state.userData = voice->block_user_data;
        state.waveformData = voice->waveform_data;
        std::memcpy(output, &state, size);
    } else if (rack->id == 0x3000 && size == sizeof(OrbisNgs2MasteringVoiceState)) {
        OrbisNgs2MasteringVoiceState state{};
        state.voiceState.stateFlags = voice->flags;
        state.limiterPeakLevel = voice->peak;
        state.limiterPressLevel = voice->limiter_pressure;
        std::ranges::copy(voice->input_peak, state.aInputPeakHeight);
        std::ranges::copy(voice->output_peak, state.aOutputPeakHeight);
        std::memcpy(output, &state, size);
    } else if (rack->id == 0x2000 && size == sizeof(OrbisNgs2SubmixerVoiceState)) {
        OrbisNgs2SubmixerVoiceState state{{voice->flags}, voice->envelope_height, voice->peak, 0};
        std::memcpy(output, &state, size);
    } else {
        return ORBIS_NGS2_ERROR_INVALID_VOICE_STATE_SIZE;
    }
    return 0;
}

s32 sceNgs2VoiceGetStateFlags(OrbisNgs2Handle handle, u32* output) {
    return sceNgs2VoiceGetState(handle, reinterpret_cast<OrbisNgs2VoiceState*>(output),
                                sizeof(u32));
}

s32 sceNgs2VoiceGetOwner(OrbisNgs2Handle handle, OrbisNgs2Handle* output, u32* index) {
    const auto voice = Lookup(voices, handle);
    const auto rack = voice ? voice->rack.lock() : nullptr;
    if (!rack)
        return ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
    if (output)
        *output = rack->handle;
    if (index)
        *index = voice->index;
    return 0;
}

s32 sceNgs2VoiceGetPortInfo(OrbisNgs2Handle handle, u32 index, OrbisNgs2VoicePortInfo* output,
                            size_t size) {
    if (!output)
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (size != sizeof(*output))
        return ORBIS_NGS2_ERROR_INVALID_OUT_SIZE;
    const auto voice = Lookup(voices, handle);
    const auto rack = voice ? voice->rack.lock() : nullptr;
    const auto system = rack ? rack->system.lock() : nullptr;
    if (!system)
        return ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
    std::scoped_lock lock(system->mutex);
    if (index >= voice->ports.size())
        return ORBIS_NGS2_ERROR_INVALID_PORT_INDEX;
    *output = voice->ports[index];
    return 0;
}

s32 sceNgs2VoiceGetMatrixInfo(OrbisNgs2Handle handle, u32 index, OrbisNgs2VoiceMatrixInfo* output,
                              size_t size) {
    if (!output)
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (size != sizeof(*output))
        return ORBIS_NGS2_ERROR_INVALID_OUT_SIZE;
    const auto voice = Lookup(voices, handle);
    const auto rack = voice ? voice->rack.lock() : nullptr;
    const auto system = rack ? rack->system.lock() : nullptr;
    if (!system)
        return ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
    std::scoped_lock lock(system->mutex);
    if (index >= voice->matrices.size())
        return ORBIS_NGS2_ERROR_INVALID_MATRIX_INDEX;
    *output = voice->matrices[index];
    return 0;
}

s32 sceNgs2SystemEnumHandles(OrbisNgs2Handle* output, u32 count) {
    if (count && !output)
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::scoped_lock lock(registry_mutex);
    u32 index{};
    for (const auto& [handle, system] : systems)
        if (index < count)
            output[index++] = handle;
    return static_cast<s32>(index);
}

s32 sceNgs2SystemEnumRackHandles(OrbisNgs2Handle handle, OrbisNgs2Handle* output, u32 count) {
    const auto system = Lookup(systems, handle);
    if (!system)
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    if (count && !output)
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::scoped_lock lock(system->mutex);
    u32 index{};
    for (const auto& rack : system->racks)
        if (index < count)
            output[index++] = rack->handle;
    return static_cast<s32>(index);
}

s32 sceNgs2SystemGetInfo(OrbisNgs2Handle handle, OrbisNgs2SystemInfo* output, size_t size) {
    if (!output)
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (size != sizeof(*output))
        return ORBIS_NGS2_ERROR_INVALID_OUT_SIZE;
    const auto system = Lookup(systems, handle);
    if (!system)
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    std::scoped_lock lock(system->mutex);
    *output = {};
    std::memcpy(output->name, system->option.name, sizeof(output->name));
    output->systemHandle = handle;
    output->bufferInfo = system->buffer;
    output->uid = static_cast<u32>(handle);
    output->minGrainSamples = 64;
    output->maxGrainSamples = system->option.maxGrainSamples;
    output->rackCount = static_cast<u32>(system->racks.size());
    output->sampleRate = system->option.sampleRate;
    output->numGrainSamples = system->option.numGrainSamples;
    output->renderCount = system->render_count;
    return 0;
}

s32 sceNgs2RackGetInfo(OrbisNgs2Handle handle, OrbisNgs2RackInfo* output, size_t size) {
    if (!output)
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (size < sizeof(*output))
        return ORBIS_NGS2_ERROR_INVALID_OUT_SIZE;
    const auto rack = Lookup(racks, handle);
    const auto system = rack ? rack->system.lock() : nullptr;
    if (!system)
        return ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;
    std::scoped_lock lock(system->mutex);
    *output = {};
    std::memcpy(output->name, rack->option.name, sizeof(output->name));
    output->rackHandle = handle;
    output->bufferInfo = rack->buffer;
    output->ownerSystemHandle = system->handle;
    output->type = rack->id >> 12;
    output->rackId = rack->id;
    output->uid = static_cast<u32>(handle);
    output->minGrainSamples = 64;
    output->maxGrainSamples = rack->option.maxGrainSamples;
    output->maxVoices = rack->option.maxVoices;
    output->maxPorts = rack->option.maxPorts;
    output->maxMatrices = rack->option.maxMatrices;
    output->renderCount = system->render_count;
    output->activeVoiceCount = static_cast<u32>(std::ranges::count_if(
        rack->voices, [](const auto& voice) { return (voice->flags & VoicePlaying) != 0; }));
    return 0;
}

s32 sceNgs2SystemSetGrainSamples(OrbisNgs2Handle handle, u32 samples) {
    const auto system = Lookup(systems, handle);
    if (!system)
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    std::scoped_lock lock(system->mutex);
    if (!ValidGrain(samples) || samples > system->option.maxGrainSamples)
        return ORBIS_NGS2_ERROR_INVALID_NUM_GRAIN_SAMPLES;
    for (const auto& rack : system->racks)
        if (samples > rack->option.maxGrainSamples)
            return ORBIS_NGS2_ERROR_INVALID_NUM_GRAIN_SAMPLES;
    system->option.numGrainSamples = samples;
    return 0;
}

s32 sceNgs2SystemSetSampleRate(OrbisNgs2Handle handle, u32 rate) {
    const auto system = Lookup(systems, handle);
    if (!system)
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    if (!ValidRate(rate))
        return ORBIS_NGS2_ERROR_INVALID_SAMPLE_RATE;
    std::scoped_lock lock(system->mutex);
    system->option.sampleRate = rate;
    return 0;
}

s32 sceNgs2SystemGetUserData(OrbisNgs2Handle handle, uintptr_t* output) {
    if (!output)
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    const auto system = Lookup(systems, handle);
    if (!system)
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    std::scoped_lock lock(system->mutex);
    *output = system->user_data;
    return 0;
}

s32 sceNgs2SystemSetUserData(OrbisNgs2Handle handle, uintptr_t data) {
    const auto system = Lookup(systems, handle);
    if (!system)
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    std::scoped_lock lock(system->mutex);
    system->user_data = data;
    return 0;
}

s32 sceNgs2RackGetUserData(OrbisNgs2Handle handle, uintptr_t* output) {
    if (!output)
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    const auto rack = Lookup(racks, handle);
    const auto system = rack ? rack->system.lock() : nullptr;
    if (!system)
        return ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;
    std::scoped_lock lock(system->mutex);
    *output = rack->user_data;
    return 0;
}

s32 sceNgs2RackSetUserData(OrbisNgs2Handle handle, uintptr_t data) {
    const auto rack = Lookup(racks, handle);
    const auto system = rack ? rack->system.lock() : nullptr;
    if (!system)
        return ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;
    std::scoped_lock lock(system->mutex);
    rack->user_data = data;
    return 0;
}

s32 sceNgs2SystemLock(OrbisNgs2Handle handle) {
    const auto system = Lookup(systems, handle);
    if (!system)
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    system->mutex.lock();
    if (!system->alive) {
        system->mutex.unlock();
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    ++public_locks[handle];
    ++system->public_locks;
    return 0;
}

s32 sceNgs2SystemUnlock(OrbisNgs2Handle handle) {
    const auto system = Lookup(systems, handle);
    if (!system)
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    const auto found = public_locks.find(handle);
    if (found == public_locks.end())
        return ORBIS_NGS2_ERROR_INVALID_OPERATION;
    if (--found->second == 0)
        public_locks.erase(found);
    --system->public_locks;
    system->mutex.unlock();
    return 0;
}

s32 sceNgs2RackLock(OrbisNgs2Handle handle) {
    const auto rack = Lookup(racks, handle);
    const auto system = rack ? rack->system.lock() : nullptr;
    return system ? sceNgs2SystemLock(system->handle) : ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;
}

s32 sceNgs2RackUnlock(OrbisNgs2Handle handle) {
    const auto rack = Lookup(racks, handle);
    const auto system = rack ? rack->system.lock() : nullptr;
    return system ? sceNgs2SystemUnlock(system->handle) : ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;
}

s32 sceNgs2CustomRackGetModuleInfo(OrbisNgs2Handle handle, u32 index,
                                   OrbisNgs2CustomModuleInfo* output, size_t size) {
    if (!output)
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (size != sizeof(*output))
        return ORBIS_NGS2_ERROR_INVALID_MODULE_INFO_SIZE;
    const auto rack = Lookup(racks, handle);
    if (!rack)
        return ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;
    if (index >= rack->custom.numModules)
        return ORBIS_NGS2_ERROR_INVALID_MODULE_INDEX;
    const auto& module = rack->custom.aModule[index];
    *output = {module.moduleId,
               module.sourceBufferId,
               module.extraBufferId,
               module.destBufferId,
               module.stateOffset,
               module.stateSize,
               0,
               0};
    return 0;
}

s32 sceNgs2SystemRender(OrbisNgs2Handle handle, const OrbisNgs2RenderBufferInfo* buffers,
                        u32 count) {
    const auto system = Lookup(systems, handle);
    if (!system)
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    if (count && !buffers)
        return ORBIS_NGS2_ERROR_INVALID_BUFFER_INFO;
    std::scoped_lock lock(system->mutex);
    if (!system->alive)
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    if (system->rendering)
        return ORBIS_NGS2_ERROR_INVALID_OPERATION;
    const u32 samples = system->option.numGrainSamples;
    for (u32 index = 0; index < count; ++index) {
        const auto& buffer = buffers[index];
        if (!ValidChannels(buffer.numChannels))
            return ORBIS_NGS2_ERROR_INVALID_NUM_CHANNELS;
        const auto bytes = PcmSize(buffer.waveformType);
        if (!bytes)
            return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_TYPE;
        if (!buffer.buffer)
            return ORBIS_NGS2_ERROR_INVALID_BUFFER_ADDRESS;
        if (buffer.bufferSize < size_t(samples) * buffer.numChannels * bytes)
            return ORBIS_NGS2_ERROR_INVALID_BUFFER_SIZE;
    }
    std::vector<std::shared_ptr<Voice>> order;
    std::unordered_map<OrbisNgs2Handle, u32> incoming;
    std::unordered_map<OrbisNgs2Handle, std::shared_ptr<Voice>> active;
    for (const auto& rack : system->racks) {
        for (const auto& voice : rack->voices) {
            std::ranges::fill(voice->audio, 0.0f);
            voice->input_peak.fill(0);
            voice->output_peak.fill(0);
            voice->peak = voice->limiter_pressure = 0;
            if ((voice->flags & VoicePlaying) && !(voice->flags & VoicePaused)) {
                active.emplace(voice->handle, voice);
                incoming.emplace(voice->handle, 0);
            }
        }
    }
    for (const auto& [voice_handle, voice] : active) {
        for (const auto& port : voice->ports)
            if (active.contains(port.destHandle))
                ++incoming[port.destHandle];
    }
    for (const auto& [voice_handle, voice] : active)
        if (!incoming[voice_handle])
            order.push_back(voice);
    for (size_t index = 0; index < order.size(); ++index) {
        for (const auto& port : order[index]->ports) {
            if (active.contains(port.destHandle) && --incoming[port.destHandle] == 0)
                order.push_back(active.at(port.destHandle));
        }
    }
    if (order.size() != active.size())
        return ORBIS_NGS2_ERROR_INVALID_PATCH;
    system->rendering = true;
    SCOPE_EXIT {
        system->rendering = false;
    };
    for (const auto& voice : order) {
        const auto rack = voice->rack.lock();
        if (rack->id == 0x1000 || rack->id == 0x4001) {
            if (const auto error = RenderSampler(*voice, samples, system->option.sampleRate);
                error < 0) {
                voice->flags |= VoiceError;
                return error;
            }
        }
        ApplyEnvelope(*voice, samples);
        if (voice->user_fx) {
            std::array<std::vector<float>, 8> planes;
            std::array<float*, 8> pointers{};
            for (u32 channel = 0; channel < voice->channels; ++channel) {
                planes[channel].resize(samples);
                pointers[channel] = planes[channel].data();
                for (u32 sample = 0; sample < samples; ++sample)
                    planes[channel][sample] = voice->audio[sample * voice->channels + channel];
            }
            OrbisNgs2UserFxProcessContext context{pointers.data(),
                                                  voice->user_fx_data[0],
                                                  voice->user_fx_data[1],
                                                  voice->user_fx_data[2],
                                                  0,
                                                  voice->channels,
                                                  samples,
                                                  system->option.sampleRate};
            const auto error = voice->user_fx(&context);
            if (error < 0) {
                return error;
            }
            for (u32 channel = 0; channel < voice->channels; ++channel)
                for (u32 sample = 0; sample < samples; ++sample)
                    voice->audio[sample * voice->channels + channel] = planes[channel][sample];
        }
        for (u32 sample = 0; sample < samples; ++sample) {
            for (u32 channel = 0; channel < voice->channels; ++channel) {
                auto& value = voice->audio[sample * voice->channels + channel];
                voice->input_peak[channel] = std::max(voice->input_peak[channel], std::abs(value));
                value *= channel == 3 && voice->channels >= 6 ? voice->lfe_gain : voice->gain;
                voice->peak = std::max(voice->peak, std::abs(value));
                if (voice->limiter_enabled) {
                    const float limited =
                        std::clamp(value, -voice->limiter_threshold, voice->limiter_threshold);
                    voice->limiter_pressure =
                        std::max(voice->limiter_pressure, std::abs(value - limited));
                    value = limited;
                }
                voice->output_peak[channel] =
                    std::max(voice->output_peak[channel], std::abs(value));
            }
        }
        for (const auto& port : voice->ports) {
            if (active.contains(port.destHandle))
                Mix(*voice, *active.at(port.destHandle), port, samples);
        }
    }
    for (u32 index = 0; index < count; ++index) {
        const auto& buffer = buffers[index];
        const auto bytes = PcmSize(buffer.waveformType);
        for (u32 sample = 0; sample < samples; ++sample) {
            for (u32 channel = 0; channel < buffer.numChannels; ++channel) {
                float value{};
                for (const auto& voice : order) {
                    const auto rack = voice->rack.lock();
                    if ((rack->id == 0x3000 || rack->id == 0x4003) && voice->output_id == index &&
                        channel < voice->channels)
                        value += voice->audio[sample * voice->channels + channel];
                }
                WritePcm(static_cast<u8*>(buffer.buffer) +
                             (sample * buffer.numChannels + channel) * bytes,
                         buffer.waveformType, value);
            }
        }
    }
    ++system->render_count;
    return 0;
}

void Reset() {
    std::scoped_lock lock(registry_mutex);
    for (auto& [handle, system] : systems)
        system->alive = false;
    for (auto& [handle, rack] : racks)
        rack->alive = false;
    voices.clear();
    racks.clear();
    systems.clear();
    public_locks.clear();
}

} // namespace Libraries::Ngs2::Engine
