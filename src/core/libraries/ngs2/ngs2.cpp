// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "core/libraries/ngs2/ngs2.h"
#include "core/libraries/ngs2/ngs2_custom.h"
#include "core/libraries/ngs2/ngs2_error.h"
#include "core/libraries/ngs2/ngs2_geom.h"
#include "core/libraries/ngs2/ngs2_impl.h"
#include "core/libraries/ngs2/ngs2_pan.h"
#include "core/libraries/ngs2/ngs2_report.h"
#include "core/libraries/ngs2/ngs2_runtime.h"

namespace Libraries::Ngs2 {

namespace {
const bool trace_calls = getenv("GAME_NGS2_TRACE") != nullptr;
}

#define NGS2_TRACE(...)                   \
    do {                                  \
        if (trace_calls) {                \
            LOG_INFO(Lib_Ngs2, __VA_ARGS__); \
        }                                 \
    } while (0)

// Ngs2

s32 PS4_SYSV_ABI sceNgs2CalcWaveformBlock(const OrbisNgs2WaveformFormat* format, u32 samplePos,
                                          u32 numSamples, OrbisNgs2WaveformBlock* outBlock) {
    const auto result = Engine::sceNgs2CalcWaveformBlock(format, samplePos, numSamples, outBlock);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2CalcWaveformBlock: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2GetWaveformFrameInfo(const OrbisNgs2WaveformFormat* format,
                                             u32* outFrameSize, u32* outNumFrameSamples,
                                             u32* outUnitsPerFrame, u32* outNumDelaySamples) {
    const auto result = Engine::sceNgs2GetWaveformFrameInfo(
        format, outFrameSize, outNumFrameSamples, outUnitsPerFrame, outNumDelaySamples);
    NGS2_TRACE("GetWaveformFrameInfo type={} -> {:#x} frame={} samples={} units={} delay={}",
               format ? format->waveformType : -1, u32(result),
               outFrameSize ? *outFrameSize : 0, outNumFrameSamples ? *outNumFrameSamples : 0,
               outUnitsPerFrame ? *outUnitsPerFrame : 0,
               outNumDelaySamples ? *outNumDelaySamples : 0);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2GetWaveformFrameInfo: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2ParseWaveformData(const void* data, size_t dataSize,
                                          OrbisNgs2WaveformInfo* outInfo) {
    const auto result = Engine::sceNgs2ParseWaveformData(data, dataSize, outInfo);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2ParseWaveformData: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2ParseWaveformFile(const char* path, u64 offset,
                                          OrbisNgs2WaveformInfo* outInfo) {
    const auto result = Engine::sceNgs2ParseWaveformFile(path, offset, outInfo);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2ParseWaveformFile: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2ParseWaveformUser(OrbisNgs2ParseReadHandler handler, uintptr_t userData,
                                          OrbisNgs2WaveformInfo* outInfo) {
    const auto result = Engine::sceNgs2ParseWaveformUser(handler, userData, outInfo);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2ParseWaveformUser: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2RackCreate(OrbisNgs2Handle systemHandle, u32 rackId,
                                   const OrbisNgs2RackOption* option,
                                   const OrbisNgs2ContextBufferInfo* bufferInfo,
                                   OrbisNgs2Handle* outHandle) {
    const auto result =
        Engine::sceNgs2RackCreate(systemHandle, rackId, option, bufferInfo, outHandle);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2RackCreate: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2RackCreateWithAllocator(OrbisNgs2Handle systemHandle, u32 rackId,
                                                const OrbisNgs2RackOption* option,
                                                const OrbisNgs2BufferAllocator* allocator,
                                                OrbisNgs2Handle* outHandle) {
    const auto result =
        Engine::sceNgs2RackCreateWithAllocator(systemHandle, rackId, option, allocator, outHandle);
    NGS2_TRACE("RackCreateWithAllocator sys={:#x} rack={:#x} -> {:#x} out={:#x}", systemHandle,
               rackId, u32(result), outHandle ? u64(*outHandle) : 0);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "Rack {:#x} creation failed: {:#x}", rackId, u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2RackDestroy(OrbisNgs2Handle rackHandle,
                                    OrbisNgs2ContextBufferInfo* outBufferInfo) {
    const auto result = Engine::sceNgs2RackDestroy(rackHandle, outBufferInfo);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2RackDestroy: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2RackGetInfo(OrbisNgs2Handle rackHandle, OrbisNgs2RackInfo* outInfo,
                                    size_t infoSize) {
    const auto result = Engine::sceNgs2RackGetInfo(rackHandle, outInfo, infoSize);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2RackGetInfo: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2RackGetUserData(OrbisNgs2Handle rackHandle, uintptr_t* outUserData) {
    const auto result = Engine::sceNgs2RackGetUserData(rackHandle, outUserData);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2RackGetUserData: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2RackGetVoiceHandle(OrbisNgs2Handle rackHandle, u32 voiceIndex,
                                           OrbisNgs2Handle* outHandle) {
    const auto result = Engine::sceNgs2RackGetVoiceHandle(rackHandle, voiceIndex, outHandle);
    NGS2_TRACE("RackGetVoiceHandle rack={:#x}[{}] -> {:#x} out={:#x}", rackHandle, voiceIndex,
               u32(result), outHandle ? u64(*outHandle) : 0);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2RackGetVoiceHandle: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2RackLock(OrbisNgs2Handle rackHandle) {
    const auto result = Engine::sceNgs2RackLock(rackHandle);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2RackLock: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2RackQueryBufferSize(u32 rackId, const OrbisNgs2RackOption* option,
                                            OrbisNgs2ContextBufferInfo* outBufferInfo) {
    const auto result = Engine::sceNgs2RackQueryBufferSize(rackId, option, outBufferInfo);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2RackQueryBufferSize: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2RackSetUserData(OrbisNgs2Handle rackHandle, uintptr_t userData) {
    const auto result = Engine::sceNgs2RackSetUserData(rackHandle, userData);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2RackSetUserData: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2RackUnlock(OrbisNgs2Handle rackHandle) {
    const auto result = Engine::sceNgs2RackUnlock(rackHandle);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2RackUnlock: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2SystemCreate(const OrbisNgs2SystemOption* option,
                                     const OrbisNgs2ContextBufferInfo* bufferInfo,
                                     OrbisNgs2Handle* outHandle) {
    const auto result = Engine::sceNgs2SystemCreate(option, bufferInfo, outHandle);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2SystemCreate: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2SystemCreateWithAllocator(const OrbisNgs2SystemOption* option,
                                                  const OrbisNgs2BufferAllocator* allocator,
                                                  OrbisNgs2Handle* outHandle) {
    const auto result = Engine::sceNgs2SystemCreateWithAllocator(option, allocator, outHandle);
    NGS2_TRACE("SystemCreateWithAllocator -> {:#x} out={:#x}", u32(result),
               outHandle ? u64(*outHandle) : 0);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2SystemCreateWithAllocator: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2SystemDestroy(OrbisNgs2Handle systemHandle,
                                      OrbisNgs2ContextBufferInfo* outBufferInfo) {
    const auto result = Engine::sceNgs2SystemDestroy(systemHandle, outBufferInfo);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2SystemDestroy: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2SystemEnumHandles(OrbisNgs2Handle* aOutHandle, u32 maxHandles) {
    const auto result = Engine::sceNgs2SystemEnumHandles(aOutHandle, maxHandles);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2SystemEnumHandles: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2SystemEnumRackHandles(OrbisNgs2Handle systemHandle,
                                              OrbisNgs2Handle* aOutHandle, u32 maxHandles) {
    const auto result = Engine::sceNgs2SystemEnumRackHandles(systemHandle, aOutHandle, maxHandles);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2SystemEnumRackHandles: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2SystemGetInfo(OrbisNgs2Handle rackHandle, OrbisNgs2SystemInfo* outInfo,
                                      size_t infoSize) {
    const auto result = Engine::sceNgs2SystemGetInfo(rackHandle, outInfo, infoSize);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2SystemGetInfo: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2SystemGetUserData(OrbisNgs2Handle systemHandle, uintptr_t* outUserData) {
    const auto result = Engine::sceNgs2SystemGetUserData(systemHandle, outUserData);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2SystemGetUserData: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2SystemLock(OrbisNgs2Handle systemHandle) {
    const auto result = Engine::sceNgs2SystemLock(systemHandle);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2SystemLock: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2SystemQueryBufferSize(const OrbisNgs2SystemOption* option,
                                              OrbisNgs2ContextBufferInfo* outBufferInfo) {
    const auto result = Engine::sceNgs2SystemQueryBufferSize(option, outBufferInfo);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2SystemQueryBufferSize: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2SystemRender(OrbisNgs2Handle systemHandle,
                                     const OrbisNgs2RenderBufferInfo* aBufferInfo,
                                     u32 numBufferInfo) {
    NGS2_TRACE("SystemRender h={:#x} n={}", systemHandle, numBufferInfo);
    const auto result = Engine::sceNgs2SystemRender(systemHandle, aBufferInfo, numBufferInfo);
    NGS2_TRACE("SystemRender -> {:#x}", u32(result));
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "Render failed: {:#x}", u32(result));
    return result;
}

static s32 PS4_SYSV_ABI sceNgs2SystemResetOption(OrbisNgs2SystemOption* outOption) {
    const auto result = Engine::sceNgs2SystemResetOption(outOption);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2SystemResetOption: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2SystemSetGrainSamples(OrbisNgs2Handle systemHandle, u32 numSamples) {
    const auto result = Engine::sceNgs2SystemSetGrainSamples(systemHandle, numSamples);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2SystemSetGrainSamples: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2SystemSetSampleRate(OrbisNgs2Handle systemHandle, u32 sampleRate) {
    const auto result = Engine::sceNgs2SystemSetSampleRate(systemHandle, sampleRate);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2SystemSetSampleRate: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2SystemSetUserData(OrbisNgs2Handle systemHandle, uintptr_t userData) {
    const auto result = Engine::sceNgs2SystemSetUserData(systemHandle, userData);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2SystemSetUserData: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2SystemUnlock(OrbisNgs2Handle systemHandle) {
    const auto result = Engine::sceNgs2SystemUnlock(systemHandle);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2SystemUnlock: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2VoiceControl(OrbisNgs2Handle voiceHandle,
                                     const OrbisNgs2VoiceParamHeader* paramList) {
    NGS2_TRACE("VoiceControl h={:#x} id={} size={}", voiceHandle, paramList ? paramList->id : -1,
               paramList ? paramList->size : 0);
    if (trace_calls && paramList) {
        const u32 id = paramList->id & 0xffff;
        if (id == 5) {
            const auto* patch =
                reinterpret_cast<const OrbisNgs2VoicePatchParam*>(paramList);
            NGS2_TRACE("VoiceControl h={:#x} patch dest={:#x} input={}", voiceHandle,
                       patch->destHandle, patch->destInputId);
        } else if (id == 6 && paramList->size >= 12) {
            const auto* event =
                reinterpret_cast<const OrbisNgs2VoiceEventParam*>(paramList);
            NGS2_TRACE("VoiceControl h={:#x} eventId={}", voiceHandle, event->eventId);
        } else if (id == 1 && paramList->size >= 24) {
            const auto* matrix =
                reinterpret_cast<const OrbisNgs2VoiceMatrixLevelsParam*>(paramList);
            NGS2_TRACE("VoiceControl h={:#x} matrix id={} n={} levels={:#x}", voiceHandle,
                       matrix->matrixId, matrix->numLevels,
                       reinterpret_cast<uintptr_t>(matrix->aLevel));
        }
    }
    const auto result = Engine::sceNgs2VoiceControl(voiceHandle, paramList);
    NGS2_TRACE("VoiceControl -> {:#x}", u32(result));
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2VoiceControl: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2VoiceGetMatrixInfo(OrbisNgs2Handle voiceHandle, u32 matrixId,
                                           OrbisNgs2VoiceMatrixInfo* outInfo, size_t outInfoSize) {
    const auto result =
        Engine::sceNgs2VoiceGetMatrixInfo(voiceHandle, matrixId, outInfo, outInfoSize);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2VoiceGetMatrixInfo: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2VoiceGetOwner(OrbisNgs2Handle voiceHandle, OrbisNgs2Handle* outRackHandle,
                                      u32* outVoiceId) {
    const auto result = Engine::sceNgs2VoiceGetOwner(voiceHandle, outRackHandle, outVoiceId);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2VoiceGetOwner: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2VoiceGetPortInfo(OrbisNgs2Handle voiceHandle, u32 port,
                                         OrbisNgs2VoicePortInfo* outInfo, size_t outInfoSize) {
    const auto result = Engine::sceNgs2VoiceGetPortInfo(voiceHandle, port, outInfo, outInfoSize);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2VoiceGetPortInfo: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2VoiceGetState(OrbisNgs2Handle voiceHandle, OrbisNgs2VoiceState* outState,
                                      size_t stateSize) {
    const auto result = Engine::sceNgs2VoiceGetState(voiceHandle, outState, stateSize);
    NGS2_TRACE("VoiceGetState h={:#x} -> {:#x} state={:#x}", voiceHandle, u32(result),
               outState ? u32(outState->stateFlags) : 0);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2VoiceGetState: {:#x}", u32(result));
    return result;
}

s32 PS4_SYSV_ABI sceNgs2VoiceGetStateFlags(OrbisNgs2Handle voiceHandle, u32* outStateFlags) {
    const auto result = Engine::sceNgs2VoiceGetStateFlags(voiceHandle, outStateFlags);
    NGS2_TRACE("VoiceGetStateFlags h={:#x} -> {:#x} flags={:#x}", voiceHandle, u32(result),
               outStateFlags ? *outStateFlags : 0);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2VoiceGetStateFlags: {:#x}", u32(result));
    return result;
}

// Ngs2Custom

s32 PS4_SYSV_ABI sceNgs2CustomRackGetModuleInfo(OrbisNgs2Handle rackHandle, u32 moduleIndex,
                                                OrbisNgs2CustomModuleInfo* outInfo,
                                                size_t infoSize) {
    const auto result =
        Engine::sceNgs2CustomRackGetModuleInfo(rackHandle, moduleIndex, outInfo, infoSize);
    if (result < 0)
        LOG_ERROR(Lib_Ngs2, "sceNgs2CustomRackGetModuleInfo: {:#x}", u32(result));
    return result;
}

// Ngs2Geom

s32 PS4_SYSV_ABI sceNgs2GeomResetListenerParam(OrbisNgs2GeomListenerParam* outListenerParam) {
    LOG_ERROR(Lib_Ngs2, "called");
    return ORBIS_NGS2_ERROR_FAIL;
}

s32 PS4_SYSV_ABI sceNgs2GeomResetSourceParam(OrbisNgs2GeomSourceParam* outSourceParam) {
    LOG_ERROR(Lib_Ngs2, "called");
    return ORBIS_NGS2_ERROR_FAIL;
}

s32 PS4_SYSV_ABI sceNgs2GeomCalcListener(const OrbisNgs2GeomListenerParam* param,
                                         OrbisNgs2GeomListenerWork* outWork, u32 flags) {
    LOG_ERROR(Lib_Ngs2, "flags = {}", flags);
    return ORBIS_NGS2_ERROR_FAIL;
}

s32 PS4_SYSV_ABI sceNgs2GeomApply(const OrbisNgs2GeomListenerWork* listener,
                                  const OrbisNgs2GeomSourceParam* source,
                                  OrbisNgs2GeomAttribute* outAttrib, u32 flags) {
    LOG_ERROR(Lib_Ngs2, "flags = {}", flags);
    return ORBIS_NGS2_ERROR_FAIL;
}

// Ngs2Pan

s32 PS4_SYSV_ABI sceNgs2PanInit(OrbisNgs2PanWork* work, const float* aSpeakerAngle, float unitAngle,
                                u32 numSpeakers) {
    LOG_ERROR(Lib_Ngs2, "unitAngle = {}, numSpeakers = {}", unitAngle, numSpeakers);
    return ORBIS_NGS2_ERROR_FAIL;
}

s32 PS4_SYSV_ABI sceNgs2PanGetVolumeMatrix(OrbisNgs2PanWork* work, const OrbisNgs2PanParam* aParam,
                                           u32 numParams, u32 matrixFormat,
                                           float* outVolumeMatrix) {
    LOG_ERROR(Lib_Ngs2, "numParams = {}, matrixFormat = {}", numParams, matrixFormat);
    return ORBIS_NGS2_ERROR_FAIL;
}

// Ngs2Report

s32 PS4_SYSV_ABI sceNgs2ReportRegisterHandler(u32 reportType, OrbisNgs2ReportHandler handler,
                                              uintptr_t userData, OrbisNgs2Handle* outHandle) {
    LOG_INFO(Lib_Ngs2, "reportType = {}, userData = {}", reportType, userData);
    if (!handler) {
        LOG_ERROR(Lib_Ngs2, "handler is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_REPORT_HANDLE;
    }
    return ORBIS_NGS2_ERROR_FAIL;
}

s32 PS4_SYSV_ABI sceNgs2ReportUnregisterHandler(OrbisNgs2Handle reportHandle) {
    if (!reportHandle) {
        LOG_ERROR(Lib_Ngs2, "reportHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_REPORT_HANDLE;
    }
    LOG_INFO(Lib_Ngs2, "called");
    return ORBIS_NGS2_ERROR_FAIL;
}

// Unknown

int PS4_SYSV_ABI sceNgs2FftInit() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_NGS2_ERROR_FAIL;
}

int PS4_SYSV_ABI sceNgs2FftProcess() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_NGS2_ERROR_FAIL;
}

int PS4_SYSV_ABI sceNgs2FftQuerySize() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_NGS2_ERROR_FAIL;
}

int PS4_SYSV_ABI sceNgs2JobSchedulerResetOption() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_NGS2_ERROR_FAIL;
}

int PS4_SYSV_ABI sceNgs2ModuleArrayEnumItems() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_NGS2_ERROR_FAIL;
}

int PS4_SYSV_ABI sceNgs2ModuleEnumConfigs() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_NGS2_ERROR_FAIL;
}

int PS4_SYSV_ABI sceNgs2ModuleQueueEnumItems() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_NGS2_ERROR_FAIL;
}

int PS4_SYSV_ABI sceNgs2RackQueryInfo() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_NGS2_ERROR_FAIL;
}

int PS4_SYSV_ABI sceNgs2RackRunCommands() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_NGS2_ERROR_FAIL;
}

int PS4_SYSV_ABI sceNgs2SystemQueryInfo() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_NGS2_ERROR_FAIL;
}

int PS4_SYSV_ABI sceNgs2SystemRunCommands() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_NGS2_ERROR_FAIL;
}

int PS4_SYSV_ABI sceNgs2SystemSetLoudThreshold() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_NGS2_ERROR_FAIL;
}

int PS4_SYSV_ABI sceNgs2StreamCreate() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_NGS2_ERROR_FAIL;
}

int PS4_SYSV_ABI sceNgs2StreamCreateWithAllocator() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_NGS2_ERROR_FAIL;
}

int PS4_SYSV_ABI sceNgs2StreamDestroy() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_NGS2_ERROR_FAIL;
}

int PS4_SYSV_ABI sceNgs2StreamQueryBufferSize() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_NGS2_ERROR_FAIL;
}

int PS4_SYSV_ABI sceNgs2StreamQueryInfo() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_NGS2_ERROR_FAIL;
}

int PS4_SYSV_ABI sceNgs2StreamResetOption() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_NGS2_ERROR_FAIL;
}

int PS4_SYSV_ABI sceNgs2StreamRunCommands() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_NGS2_ERROR_FAIL;
}

int PS4_SYSV_ABI sceNgs2VoiceQueryInfo() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_NGS2_ERROR_FAIL;
}

int PS4_SYSV_ABI sceNgs2VoiceRunCommands() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_NGS2_ERROR_FAIL;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("3pCNbVM11UA", "libSceNgs2", 1, "libSceNgs2", sceNgs2CalcWaveformBlock);
    LIB_FUNCTION("6qN1zaEZuN0", "libSceNgs2", 1, "libSceNgs2", sceNgs2CustomRackGetModuleInfo);
    LIB_FUNCTION("Kg1MA5j7KFk", "libSceNgs2", 1, "libSceNgs2", sceNgs2FftInit);
    LIB_FUNCTION("D8eCqBxSojA", "libSceNgs2", 1, "libSceNgs2", sceNgs2FftProcess);
    LIB_FUNCTION("-YNfTO6KOMY", "libSceNgs2", 1, "libSceNgs2", sceNgs2FftQuerySize);
    LIB_FUNCTION("eF8yRCC6W64", "libSceNgs2", 1, "libSceNgs2", sceNgs2GeomApply);
    LIB_FUNCTION("1WsleK-MTkE", "libSceNgs2", 1, "libSceNgs2", sceNgs2GeomCalcListener);
    LIB_FUNCTION("7Lcfo8SmpsU", "libSceNgs2", 1, "libSceNgs2", sceNgs2GeomResetListenerParam);
    LIB_FUNCTION("0lbbayqDNoE", "libSceNgs2", 1, "libSceNgs2", sceNgs2GeomResetSourceParam);
    LIB_FUNCTION("ekGJmmoc8j4", "libSceNgs2", 1, "libSceNgs2", sceNgs2GetWaveformFrameInfo);
    LIB_FUNCTION("BcoPfWfpvVI", "libSceNgs2", 1, "libSceNgs2", sceNgs2JobSchedulerResetOption);
    LIB_FUNCTION("EEemGEQCjO8", "libSceNgs2", 1, "libSceNgs2", sceNgs2ModuleArrayEnumItems);
    LIB_FUNCTION("TaoNtmMKkXQ", "libSceNgs2", 1, "libSceNgs2", sceNgs2ModuleEnumConfigs);
    LIB_FUNCTION("ve6bZi+1sYQ", "libSceNgs2", 1, "libSceNgs2", sceNgs2ModuleQueueEnumItems);
    LIB_FUNCTION("gbMKV+8Enuo", "libSceNgs2", 1, "libSceNgs2", sceNgs2PanGetVolumeMatrix);
    LIB_FUNCTION("xa8oL9dmXkM", "libSceNgs2", 1, "libSceNgs2", sceNgs2PanInit);
    LIB_FUNCTION("hyVLT2VlOYk", "libSceNgs2", 1, "libSceNgs2", sceNgs2ParseWaveformData);
    LIB_FUNCTION("iprCTXPVWMI", "libSceNgs2", 1, "libSceNgs2", sceNgs2ParseWaveformFile);
    LIB_FUNCTION("t9T0QM17Kvo", "libSceNgs2", 1, "libSceNgs2", sceNgs2ParseWaveformUser);
    LIB_FUNCTION("cLV4aiT9JpA", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackCreate);
    LIB_FUNCTION("U546k6orxQo", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackCreateWithAllocator);
    LIB_FUNCTION("lCqD7oycmIM", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackDestroy);
    LIB_FUNCTION("M4LYATRhRUE", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackGetInfo);
    LIB_FUNCTION("Mn4XNDg03XY", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackGetUserData);
    LIB_FUNCTION("MwmHz8pAdAo", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackGetVoiceHandle);
    LIB_FUNCTION("MzTa7VLjogY", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackLock);
    LIB_FUNCTION("0eFLVCfWVds", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackQueryBufferSize);
    LIB_FUNCTION("TZqb8E-j3dY", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackQueryInfo);
    LIB_FUNCTION("MI2VmBx2RbM", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackRunCommands);
    LIB_FUNCTION("JNTMIaBIbV4", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackSetUserData);
    LIB_FUNCTION("++YZ7P9e87U", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackUnlock);
    LIB_FUNCTION("uBIN24Tv2MI", "libSceNgs2", 1, "libSceNgs2", sceNgs2ReportRegisterHandler);
    LIB_FUNCTION("nPzb7Ly-VjE", "libSceNgs2", 1, "libSceNgs2", sceNgs2ReportUnregisterHandler);
    LIB_FUNCTION("koBbCMvOKWw", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemCreate);
    LIB_FUNCTION("mPYgU4oYpuY", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemCreateWithAllocator);
    LIB_FUNCTION("u-WrYDaJA3k", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemDestroy);
    LIB_FUNCTION("vubFP0T6MP0", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemEnumHandles);
    LIB_FUNCTION("U-+7HsswcIs", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemEnumRackHandles);
    LIB_FUNCTION("vU7TQ62pItw", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemGetInfo);
    LIB_FUNCTION("4lFaRxd-aLs", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemGetUserData);
    LIB_FUNCTION("gThZqM5PYlQ", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemLock);
    LIB_FUNCTION("pgFAiLR5qT4", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemQueryBufferSize);
    LIB_FUNCTION("3oIK7y7O4k0", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemQueryInfo);
    LIB_FUNCTION("i0VnXM-C9fc", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemRender);
    LIB_FUNCTION("AQkj7C0f3PY", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemResetOption);
    LIB_FUNCTION("gXiormHoZZ4", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemRunCommands);
    LIB_FUNCTION("l4Q2dWEH6UM", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemSetGrainSamples);
    LIB_FUNCTION("Wdlx0ZFTV9s", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemSetLoudThreshold);
    LIB_FUNCTION("-tbc2SxQD60", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemSetSampleRate);
    LIB_FUNCTION("GZB2v0XnG0k", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemSetUserData);
    LIB_FUNCTION("JXRC5n0RQls", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemUnlock);
    LIB_FUNCTION("sU2St3agdjg", "libSceNgs2", 1, "libSceNgs2", sceNgs2StreamCreate);
    LIB_FUNCTION("I+RLwaauggA", "libSceNgs2", 1, "libSceNgs2", sceNgs2StreamCreateWithAllocator);
    LIB_FUNCTION("bfoMXnTRtwE", "libSceNgs2", 1, "libSceNgs2", sceNgs2StreamDestroy);
    LIB_FUNCTION("dxulc33msHM", "libSceNgs2", 1, "libSceNgs2", sceNgs2StreamQueryBufferSize);
    LIB_FUNCTION("rfw6ufRsmow", "libSceNgs2", 1, "libSceNgs2", sceNgs2StreamQueryInfo);
    LIB_FUNCTION("q+2W8YdK0F8", "libSceNgs2", 1, "libSceNgs2", sceNgs2StreamResetOption);
    LIB_FUNCTION("qQHCi9pjDps", "libSceNgs2", 1, "libSceNgs2", sceNgs2StreamRunCommands);
    LIB_FUNCTION("uu94irFOGpA", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceControl);
    LIB_FUNCTION("jjBVvPN9964", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceGetMatrixInfo);
    LIB_FUNCTION("W-Z8wWMBnhk", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceGetOwner);
    LIB_FUNCTION("WCayTgob7-o", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceGetPortInfo);
    LIB_FUNCTION("-TOuuAQ-buE", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceGetState);
    LIB_FUNCTION("rEh728kXk3w", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceGetStateFlags);
    LIB_FUNCTION("9eic4AmjGVI", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceQueryInfo);
    LIB_FUNCTION("AbYvTOZ8Pts", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceRunCommands);
};

} // namespace Libraries::Ngs2
