// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "ngs2.h"
#include "ngs2_custom.h"

namespace Libraries::Ngs2::Engine {

void Reset();

s32 sceNgs2CalcWaveformBlock(const OrbisNgs2WaveformFormat* format, u32 samplePos, u32 numSamples,
                             OrbisNgs2WaveformBlock* outBlock);
s32 sceNgs2GetWaveformFrameInfo(const OrbisNgs2WaveformFormat* format, u32* outFrameSize,
                                u32* outNumFrameSamples, u32* outUnitsPerFrame,
                                u32* outNumDelaySamples);
s32 sceNgs2ParseWaveformData(const void* data, size_t dataSize, OrbisNgs2WaveformInfo* outInfo);
s32 sceNgs2ParseWaveformFile(const char* path, u64 offset, OrbisNgs2WaveformInfo* outInfo);
s32 sceNgs2ParseWaveformUser(OrbisNgs2ParseReadHandler handler, uintptr_t userData,
                             OrbisNgs2WaveformInfo* outInfo);
s32 sceNgs2RackCreate(OrbisNgs2Handle systemHandle, u32 rackId, const OrbisNgs2RackOption* option,
                      const OrbisNgs2ContextBufferInfo* bufferInfo, OrbisNgs2Handle* outHandle);
s32 sceNgs2RackCreateWithAllocator(OrbisNgs2Handle systemHandle, u32 rackId,
                                   const OrbisNgs2RackOption* option,
                                   const OrbisNgs2BufferAllocator* allocator,
                                   OrbisNgs2Handle* outHandle);
s32 sceNgs2RackDestroy(OrbisNgs2Handle rackHandle, OrbisNgs2ContextBufferInfo* outBufferInfo);
s32 sceNgs2RackGetInfo(OrbisNgs2Handle rackHandle, OrbisNgs2RackInfo* outInfo, size_t infoSize);
s32 sceNgs2RackGetUserData(OrbisNgs2Handle rackHandle, uintptr_t* outUserData);
s32 sceNgs2RackGetVoiceHandle(OrbisNgs2Handle rackHandle, u32 voiceIndex,
                              OrbisNgs2Handle* outHandle);
s32 sceNgs2RackLock(OrbisNgs2Handle rackHandle);
s32 sceNgs2RackQueryBufferSize(u32 rackId, const OrbisNgs2RackOption* option,
                               OrbisNgs2ContextBufferInfo* outBufferInfo);
s32 sceNgs2RackSetUserData(OrbisNgs2Handle rackHandle, uintptr_t userData);
s32 sceNgs2RackUnlock(OrbisNgs2Handle rackHandle);
s32 sceNgs2SystemCreate(const OrbisNgs2SystemOption* option,
                        const OrbisNgs2ContextBufferInfo* bufferInfo, OrbisNgs2Handle* outHandle);
s32 sceNgs2SystemCreateWithAllocator(const OrbisNgs2SystemOption* option,
                                     const OrbisNgs2BufferAllocator* allocator,
                                     OrbisNgs2Handle* outHandle);
s32 sceNgs2SystemDestroy(OrbisNgs2Handle systemHandle, OrbisNgs2ContextBufferInfo* outBufferInfo);
s32 sceNgs2SystemEnumHandles(OrbisNgs2Handle* aOutHandle, u32 maxHandles);
s32 sceNgs2SystemEnumRackHandles(OrbisNgs2Handle systemHandle, OrbisNgs2Handle* aOutHandle,
                                 u32 maxHandles);
s32 sceNgs2SystemGetInfo(OrbisNgs2Handle rackHandle, OrbisNgs2SystemInfo* outInfo, size_t infoSize);
s32 sceNgs2SystemGetUserData(OrbisNgs2Handle systemHandle, uintptr_t* outUserData);
s32 sceNgs2SystemLock(OrbisNgs2Handle systemHandle);
s32 sceNgs2SystemQueryBufferSize(const OrbisNgs2SystemOption* option,
                                 OrbisNgs2ContextBufferInfo* outBufferInfo);
s32 sceNgs2SystemRender(OrbisNgs2Handle systemHandle, const OrbisNgs2RenderBufferInfo* aBufferInfo,
                        u32 numBufferInfo);
s32 sceNgs2SystemResetOption(OrbisNgs2SystemOption* outOption);
s32 sceNgs2SystemSetGrainSamples(OrbisNgs2Handle systemHandle, u32 numSamples);
s32 sceNgs2SystemSetSampleRate(OrbisNgs2Handle systemHandle, u32 sampleRate);
s32 sceNgs2SystemSetUserData(OrbisNgs2Handle systemHandle, uintptr_t userData);
s32 sceNgs2SystemUnlock(OrbisNgs2Handle systemHandle);
s32 sceNgs2VoiceControl(OrbisNgs2Handle voiceHandle, const OrbisNgs2VoiceParamHeader* paramList);
s32 sceNgs2VoiceGetMatrixInfo(OrbisNgs2Handle voiceHandle, u32 matrixId,
                              OrbisNgs2VoiceMatrixInfo* outInfo, size_t outInfoSize);
s32 sceNgs2VoiceGetOwner(OrbisNgs2Handle voiceHandle, OrbisNgs2Handle* outRackHandle,
                         u32* outVoiceId);
s32 sceNgs2VoiceGetPortInfo(OrbisNgs2Handle voiceHandle, u32 port, OrbisNgs2VoicePortInfo* outInfo,
                            size_t outInfoSize);
s32 sceNgs2VoiceGetState(OrbisNgs2Handle voiceHandle, OrbisNgs2VoiceState* outState,
                         size_t stateSize);
s32 sceNgs2VoiceGetStateFlags(OrbisNgs2Handle voiceHandle, u32* outStateFlags);
s32 sceNgs2CustomRackGetModuleInfo(OrbisNgs2Handle rackHandle, u32 moduleIndex,
                                   OrbisNgs2CustomModuleInfo* outInfo, size_t infoSize);

} // namespace Libraries::Ngs2::Engine
