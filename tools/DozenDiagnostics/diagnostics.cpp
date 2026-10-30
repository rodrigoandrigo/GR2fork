// SPDX-License-Identifier: GPL-2.0-or-later
#define VK_NO_PROTOTYPES
#include "../../externals/mesa-uwp/src/microsoft/vulkan/dzn_raw_image.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include <windows.h>
#include <wrl/client.h>

namespace DozenDiagnostics {
using Logger = std::function<void(const std::string &)>;
struct ApiError : std::runtime_error {
  VkResult result;
  ApiError(const char *operation, VkResult value)
      : std::runtime_error(std::string(operation) +
                           " VkResult=" + std::to_string(value)),
        result(value) {}
};
void Check(VkResult result, const char *operation) {
  if (result != VK_SUCCESS)
    throw ApiError(operation, result);
}
struct Format {
  VkFormat value;
  const char *name;
  char kind;
  unsigned channels;
  bool extended;
};
const Format Formats[] = {
#define FORMAT(name, qualifier, kind, channels, extended)                      \
  {VK_FORMAT_##name, #name, kind, channels, bool(extended)},
#include "formats.inc"
#undef FORMAT
};
struct Context {
  Logger log;
  std::filesystem::path shaders;
  HMODULE library{};
  std::vector<HMODULE> bootstrap;
  VkInstance instance{};
  VkPhysicalDevice physical{};
  VkDevice device{};
  VkQueue queue{};
  uint32_t family{};
  VkPhysicalDeviceFeatures features{};
  VkPhysicalDeviceMemoryProperties memory{};
  unsigned passed{}, failed{}, unsupported{}, errors{};
  bool in_flight{};
#define FUNCTION(name) PFN_##name name{};
  FUNCTION(vkDestroyInstance);
  FUNCTION(vkEnumeratePhysicalDevices);
  FUNCTION(vkGetPhysicalDeviceProperties);
  FUNCTION(vkGetPhysicalDeviceProperties2);
  FUNCTION(vkGetPhysicalDeviceFeatures);
  FUNCTION(vkGetPhysicalDeviceMemoryProperties);
  FUNCTION(vkGetPhysicalDeviceQueueFamilyProperties);
  FUNCTION(vkGetPhysicalDeviceFormatProperties2);
  FUNCTION(vkGetPhysicalDeviceImageFormatProperties);
  FUNCTION(vkEnumerateDeviceExtensionProperties);
  FUNCTION(vkCreateDevice);
  FUNCTION(vkGetDeviceProcAddr);
  FUNCTION(vkDestroyDevice);
  FUNCTION(vkGetDeviceQueue);
  FUNCTION(vkCreateBuffer);
  FUNCTION(vkDestroyBuffer);
  FUNCTION(vkGetBufferMemoryRequirements);
  FUNCTION(vkCreateImage);
  FUNCTION(vkDestroyImage);
  FUNCTION(vkGetImageMemoryRequirements);
  FUNCTION(vkAllocateMemory);
  FUNCTION(vkFreeMemory);
  FUNCTION(vkBindBufferMemory);
  FUNCTION(vkBindImageMemory);
  FUNCTION(vkMapMemory);
  FUNCTION(vkUnmapMemory);
  FUNCTION(vkFlushMappedMemoryRanges);
  FUNCTION(vkInvalidateMappedMemoryRanges);
  FUNCTION(vkCreateImageView);
  FUNCTION(vkDestroyImageView);
  FUNCTION(vkCreateSampler);
  FUNCTION(vkDestroySampler);
  FUNCTION(vkCreateDescriptorSetLayout);
  FUNCTION(vkDestroyDescriptorSetLayout);
  FUNCTION(vkCreateDescriptorPool);
  FUNCTION(vkDestroyDescriptorPool);
  FUNCTION(vkAllocateDescriptorSets);
  FUNCTION(vkUpdateDescriptorSets);
  FUNCTION(vkCreatePipelineLayout);
  FUNCTION(vkDestroyPipelineLayout);
  FUNCTION(vkCreateShaderModule);
  FUNCTION(vkDestroyShaderModule);
  FUNCTION(vkCreateComputePipelines);
  FUNCTION(vkCreatePipelineCache);
  FUNCTION(vkDestroyPipelineCache);
  FUNCTION(vkGetPipelineCacheData);
  FUNCTION(vkDestroyPipeline);
  FUNCTION(vkCreateCommandPool);
  FUNCTION(vkDestroyCommandPool);
  FUNCTION(vkAllocateCommandBuffers);
  FUNCTION(vkBeginCommandBuffer);
  FUNCTION(vkEndCommandBuffer);
  FUNCTION(vkCmdPipelineBarrier);
  FUNCTION(vkCmdClearColorImage);
  FUNCTION(vkCmdBindPipeline);
  FUNCTION(vkCmdBindDescriptorSets);
  FUNCTION(vkCmdDispatch);
  FUNCTION(vkCreateFence);
  FUNCTION(vkDestroyFence);
  FUNCTION(vkQueueSubmit);
  FUNCTION(vkWaitForFences);
  FUNCTION(vkCreateRenderPass);
  FUNCTION(vkDestroyRenderPass);
  FUNCTION(vkCreateFramebuffer);
  FUNCTION(vkDestroyFramebuffer);
  FUNCTION(vkCreateGraphicsPipelines);
  FUNCTION(vkCmdBeginRenderPass);
  FUNCTION(vkCmdEndRenderPass);
  FUNCTION(vkCmdDraw);
  FUNCTION(vkCmdPushConstants);
  FUNCTION(vkCmdCopyImageToBuffer);
  FUNCTION(vkCmdCopyBufferToImage);
#undef FUNCTION
  bool automatic_raw = true;
  void Init(const std::filesystem::path &driver) {
    for (const auto *name : {L"dxil.dll", L"dxcompiler.dll"}) {
      auto path = driver.parent_path() / name;
      auto module = LoadLibraryExW(path.c_str(), nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                       LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
      if (!module)
        throw std::runtime_error("Load compiler dependency Win32=" +
                                 std::to_string(GetLastError()));
      bootstrap.push_back(module);
    }
    auto d3d12 =
        LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!d3d12)
      throw std::runtime_error("Load D3D12 Win32=" +
                               std::to_string(GetLastError()));
    bootstrap.push_back(d3d12);
    library = LoadLibraryExW(driver.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                 LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!library)
      throw std::runtime_error("Load Dozen Win32=" +
                               std::to_string(GetLastError()));
    auto logger =
        reinterpret_cast<void (*)(void (*)(void *, const char *), void *)>(
            GetProcAddress(library, "mesa_uwp_set_log_callback"));
    if (logger)
      logger(
          [](void *data, const char *message) {
            static_cast<Context *>(data)->log(std::string("DRIVER ") + message);
          },
          this);
    auto limit = reinterpret_cast<void (*)(unsigned)>(
        GetProcAddress(library, "mesa_uwp_set_shader_model_limit"));
    if (!limit)
      throw std::runtime_error("Explicit shader model limit API unavailable");
    limit(0x64);
    auto legacy = reinterpret_cast<void (*)(int)>(
        GetProcAddress(library, "mesa_uwp_force_legacy_barriers"));
    if (!legacy)
      throw std::runtime_error("Explicit legacy barrier API unavailable");
    legacy(1);
    auto raw_lowering = reinterpret_cast<void (*)(int)>(
        GetProcAddress(library, "mesa_uwp_set_raw_image_lowering"));
    if (!raw_lowering)
      throw std::runtime_error("Automatic raw image lowering API unavailable");
    auto get_raw_lowering = reinterpret_cast<int (*)()>(
        GetProcAddress(library, "mesa_uwp_get_raw_image_lowering"));
    if (!get_raw_lowering)
      throw std::runtime_error("Raw image profile query API unavailable");
    int production_default = get_raw_lowering();
    if (!production_default)
      throw std::runtime_error("Production raw image profile is disabled");
    if (!automatic_raw)
      raw_lowering(0);
    if (get_raw_lowering() != int(automatic_raw))
      throw std::runtime_error("Unexpected raw image profile");
    log("PROFILE requested_shader_model=6.4 explicit_driver_limit=1 "
        "force_legacy_barriers=1 automatic_raw_images=" + std::to_string(automatic_raw) +
        " production_default=" + std::to_string(production_default) +
        " explicit_raw_override=" + std::to_string(!automatic_raw));
    auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(library, "vk_icdGetInstanceProcAddr"));
    if (!get)
      throw std::runtime_error("vk_icdGetInstanceProcAddr missing");
    auto create = reinterpret_cast<PFN_vkCreateInstance>(
        get(nullptr, "vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Dozen Diagnostics";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    Check(create(&ci, nullptr, &instance), "create instance");
#define LOAD(name)                                                             \
  name = reinterpret_cast<PFN_##name>(get(instance, #name));                   \
  if (!name)                                                                   \
    throw std::runtime_error(#name " missing");
    LOAD(vkDestroyInstance);
    LOAD(vkEnumeratePhysicalDevices);
    LOAD(vkGetPhysicalDeviceProperties);
    LOAD(vkGetPhysicalDeviceProperties2);
    LOAD(vkGetPhysicalDeviceFeatures);
    LOAD(vkGetPhysicalDeviceMemoryProperties);
    LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD(vkGetPhysicalDeviceFormatProperties2);
    LOAD(vkGetPhysicalDeviceImageFormatProperties);
    LOAD(vkEnumerateDeviceExtensionProperties);
    LOAD(vkCreateDevice);
    LOAD(vkGetDeviceProcAddr);
#undef LOAD
    uint32_t count{};
    Check(vkEnumeratePhysicalDevices(instance, &count, nullptr),
          "device count");
    std::vector<VkPhysicalDevice> devices(count);
    Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()),
          "devices");
    int best = -1;
    for (auto candidate : devices) {
      VkPhysicalDeviceProperties props{};
      vkGetPhysicalDeviceProperties(candidate, &props);
      int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                  : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
                      ? 2
                      : 0;
      log("ADAPTER " + std::string(props.deviceName) +
          " type=" + std::to_string(props.deviceType));
      if (score > best) {
        best = score;
        physical = candidate;
      }
    }
    if (!physical)
      throw std::runtime_error("No Vulkan device");
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical, &props);
    log("SELECTED " + std::string(props.deviceName) +
        " api=" + std::to_string(props.apiVersion) +
        " driver=" + std::to_string(props.driverVersion));
    vkGetPhysicalDeviceFeatures(physical, &features);
    log("FEATURE robustBufferAccess=" +
        std::to_string(features.robustBufferAccess));
    log("FEATURE fullDrawIndexUint32=" +
        std::to_string(features.fullDrawIndexUint32));
    log("FEATURE independentBlend=" +
        std::to_string(features.independentBlend));
    log("FEATURE geometryShader=" + std::to_string(features.geometryShader));
    log("FEATURE tessellationShader=" +
        std::to_string(features.tessellationShader));
    log("FEATURE sampleRateShading=" +
        std::to_string(features.sampleRateShading));
    log("FEATURE dualSrcBlend=" + std::to_string(features.dualSrcBlend));
    log("FEATURE logicOp=" + std::to_string(features.logicOp));
    log("FEATURE multiDrawIndirect=" +
        std::to_string(features.multiDrawIndirect));
    log("FEATURE drawIndirectFirstInstance=" +
        std::to_string(features.drawIndirectFirstInstance));
    log("FEATURE depthClamp=" + std::to_string(features.depthClamp));
    log("FEATURE depthBiasClamp=" + std::to_string(features.depthBiasClamp));
    log("FEATURE fillModeNonSolid=" +
        std::to_string(features.fillModeNonSolid));
    log("FEATURE depthBounds=" + std::to_string(features.depthBounds));
    log("FEATURE wideLines=" + std::to_string(features.wideLines));
    log("FEATURE largePoints=" + std::to_string(features.largePoints));
    log("FEATURE multiViewport=" + std::to_string(features.multiViewport));
    log("FEATURE samplerAnisotropy=" +
        std::to_string(features.samplerAnisotropy));
    log("FEATURE vertexPipelineStoresAndAtomics=" +
        std::to_string(features.vertexPipelineStoresAndAtomics));
    log("FEATURE fragmentStoresAndAtomics=" +
        std::to_string(features.fragmentStoresAndAtomics));
    log("FEATURE shaderFloat64=" + std::to_string(features.shaderFloat64));
    log("FEATURE shaderInt64=" + std::to_string(features.shaderInt64));
    log("FEATURE shaderInt16=" + std::to_string(features.shaderInt16));
    log("FEATURE shaderStorageImageMultisample=" +
        std::to_string(features.shaderStorageImageMultisample));
    log("FEATURE sparseBinding=" + std::to_string(features.sparseBinding));
    log("FEATURE sparseResidencyBuffer=" +
        std::to_string(features.sparseResidencyBuffer));

    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    log("FEATURE storage_extended=" +
        std::to_string(features.shaderStorageImageExtendedFormats) +
        " read_without_format=" +
        std::to_string(features.shaderStorageImageReadWithoutFormat) +
        " write_without_format=" +
        std::to_string(features.shaderStorageImageWriteWithoutFormat));
    Check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count,
                                               nullptr),
          "extension count");
    std::vector<VkExtensionProperties> extensions(count);
    Check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count,
                                               extensions.data()),
          "extensions");
    bool flags2 = false;
    for (auto &e : extensions) {
      log("EXTENSION " + std::string(e.extensionName));
      flags2 |= std::strcmp(e.extensionName,
                            VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME) == 0;
    }
    if (!flags2)
      throw std::runtime_error("VK_KHR_format_feature_flags2 required for "
                               "per-format read/write queries");
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
    while (family < count && (queues[family].queueFlags &
                              (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT)) !=
                                 (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT))
      ++family;
    if (family == count)
      throw std::runtime_error("No compute queue");
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures enabled{};
    enabled.robustBufferAccess = features.robustBufferAccess;
    enabled.imageCubeArray = features.imageCubeArray;
    enabled.vertexPipelineStoresAndAtomics =
        features.vertexPipelineStoresAndAtomics;
    enabled.fragmentStoresAndAtomics = features.fragmentStoresAndAtomics;
    enabled.shaderStorageImageExtendedFormats =
        features.shaderStorageImageExtendedFormats;
    enabled.shaderStorageImageReadWithoutFormat =
        features.shaderStorageImageReadWithoutFormat;
    enabled.shaderStorageImageWriteWithoutFormat =
        features.shaderStorageImageWriteWithoutFormat;
    const char *ext = VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.pEnabledFeatures = &enabled;
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = &ext;
    Check(vkCreateDevice(physical, &di, nullptr, &device), "create device");
#define LOAD(name)                                                             \
  name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));     \
  if (!name)                                                                   \
    throw std::runtime_error(#name " missing");
    LOAD(vkDestroyDevice);
    LOAD(vkGetDeviceQueue);
    LOAD(vkCreateBuffer);
    LOAD(vkDestroyBuffer);
    LOAD(vkGetBufferMemoryRequirements);
    LOAD(vkCreateImage);
    LOAD(vkDestroyImage);
    LOAD(vkGetImageMemoryRequirements);
    LOAD(vkAllocateMemory);
    LOAD(vkFreeMemory);
    LOAD(vkBindBufferMemory);
    LOAD(vkBindImageMemory);
    LOAD(vkMapMemory);
    LOAD(vkUnmapMemory);
    LOAD(vkFlushMappedMemoryRanges);
    LOAD(vkInvalidateMappedMemoryRanges);
    LOAD(vkCreateImageView);
    LOAD(vkDestroyImageView);
    LOAD(vkCreateSampler);
    LOAD(vkDestroySampler);
    LOAD(vkCreateDescriptorSetLayout);
    LOAD(vkDestroyDescriptorSetLayout);
    LOAD(vkCreateDescriptorPool);
    LOAD(vkDestroyDescriptorPool);
    LOAD(vkAllocateDescriptorSets);
    LOAD(vkUpdateDescriptorSets);
    LOAD(vkCreatePipelineLayout);
    LOAD(vkDestroyPipelineLayout);
    LOAD(vkCreateShaderModule);
    LOAD(vkDestroyShaderModule);
    LOAD(vkCreateComputePipelines);
    LOAD(vkCreatePipelineCache);
    LOAD(vkDestroyPipelineCache);
    LOAD(vkGetPipelineCacheData);
    LOAD(vkDestroyPipeline);
    LOAD(vkCreateCommandPool);
    LOAD(vkDestroyCommandPool);
    LOAD(vkAllocateCommandBuffers);
    LOAD(vkBeginCommandBuffer);
    LOAD(vkEndCommandBuffer);
    LOAD(vkCmdPipelineBarrier);
    LOAD(vkCmdClearColorImage);
    LOAD(vkCmdBindPipeline);
    LOAD(vkCmdBindDescriptorSets);
    LOAD(vkCmdDispatch);
    LOAD(vkCreateFence);
    LOAD(vkDestroyFence);
    LOAD(vkQueueSubmit);
    LOAD(vkWaitForFences);
    LOAD(vkCreateRenderPass);
    LOAD(vkDestroyRenderPass);
    LOAD(vkCreateFramebuffer);
    LOAD(vkDestroyFramebuffer);
    LOAD(vkCreateGraphicsPipelines);
    LOAD(vkCmdBeginRenderPass);
    LOAD(vkCmdEndRenderPass);
    LOAD(vkCmdDraw);
    LOAD(vkCmdPushConstants);
    LOAD(vkCmdCopyImageToBuffer);
    LOAD(vkCmdCopyBufferToImage);
#undef LOAD
    vkGetDeviceQueue(device, family, 0, &queue);
  }
  uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
      if ((bits & (1u << i)) &&
          (memory.memoryTypes[i].propertyFlags & flags) == flags)
        return i;
    throw std::runtime_error("No compatible memory type");
  }
  ~Context() {
    if (!in_flight && device && vkDestroyDevice)
      vkDestroyDevice(device, nullptr);
    if (!in_flight && instance && vkDestroyInstance)
      vkDestroyInstance(instance, nullptr);
    if (library) {
      auto logger =
          reinterpret_cast<void (*)(void (*)(void *, const char *), void *)>(
              GetProcAddress(library, "mesa_uwp_set_log_callback"));
      if (logger)
        logger(nullptr, nullptr);
      if (!in_flight)
        FreeLibrary(library);
    }
    if (!in_flight)
      for (auto module : bootstrap)
        FreeLibrary(module);
  }
};
struct Resources {
  Context &c;
  VkBuffer output{};
  VkDeviceMemory output_memory{}, image_memory{};
  void *mapped{};
  VkImage image{};
  VkImageView view{};
  VkSampler sampler{};
  VkRenderPass render_pass{};
  VkFramebuffer framebuffer{};
  VkImage depth{};
  VkDeviceMemory depth_memory{};
  VkImageView depth_view{};
  std::vector<VkShaderModule> shaders;
  VkDescriptorSetLayout set_layout{};
  VkDescriptorPool descriptors{};
  VkPipelineLayout layout{};
  VkCommandPool commands{};
  VkFence fence{};
  std::vector<VkPipeline> pipelines;
  std::vector<VkPipelineCache> pipeline_caches;
  ~Resources() {
    if (c.in_flight)
      return;
    auto d = c.device;
    if (fence)
      c.vkDestroyFence(d, fence, nullptr);
    if (commands)
      c.vkDestroyCommandPool(d, commands, nullptr);
    for (auto p : pipelines)
      c.vkDestroyPipeline(d, p, nullptr);
    for (auto cache : pipeline_caches)
      c.vkDestroyPipelineCache(d, cache, nullptr);
    for (auto module : shaders)
      c.vkDestroyShaderModule(d, module, nullptr);
    if (framebuffer)
      c.vkDestroyFramebuffer(d, framebuffer, nullptr);
    if (render_pass)
      c.vkDestroyRenderPass(d, render_pass, nullptr);
    if (depth_view)
      c.vkDestroyImageView(d, depth_view, nullptr);
    if (depth)
      c.vkDestroyImage(d, depth, nullptr);
    if (depth_memory)
      c.vkFreeMemory(d, depth_memory, nullptr);
    if (descriptors)
      c.vkDestroyDescriptorPool(d, descriptors, nullptr);
    if (layout)
      c.vkDestroyPipelineLayout(d, layout, nullptr);
    if (set_layout)
      c.vkDestroyDescriptorSetLayout(d, set_layout, nullptr);
    if (sampler)
      c.vkDestroySampler(d, sampler, nullptr);
    if (view)
      c.vkDestroyImageView(d, view, nullptr);
    if (image)
      c.vkDestroyImage(d, image, nullptr);
    if (image_memory)
      c.vkFreeMemory(d, image_memory, nullptr);
    if (mapped)
      c.vkUnmapMemory(d, output_memory);
    if (output)
      c.vkDestroyBuffer(d, output, nullptr);
    if (output_memory)
      c.vkFreeMemory(d, output_memory, nullptr);
  }
};
VkPipeline Pipeline(Context &c, Resources &r, const Format &format,
                    const char *mode) {
  auto path = c.shaders / (std::string(format.name) + "_" + mode + ".spv");
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    throw std::runtime_error("Missing shader " + path.string());
  auto bytes = static_cast<size_t>(file.tellg());
  if (!bytes || bytes % 4)
    throw std::runtime_error("Invalid SPIR-V size");
  std::vector<uint32_t> code(bytes / 4);
  file.seekg(0);
  file.read(reinterpret_cast<char *>(code.data()), bytes);
  VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  sm.codeSize = bytes;
  sm.pCode = code.data();
  VkShaderModule module{};
  Check(c.vkCreateShaderModule(c.device, &sm, nullptr, &module),
        "shader module");
  r.shaders.push_back(module);
  VkComputePipelineCreateInfo pi{
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pi.layout = r.layout;
  pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pi.stage.module = module;
  pi.stage.pName = "main";
  VkPipeline pipeline{};
  bool warm = c.automatic_raw && std::strcmp(mode, "automatic") == 0;
  VkPipelineCache cache{};
  VkPipelineCacheCreateInfo cache_info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  if (warm) {
    Check(c.vkCreatePipelineCache(c.device, &cache_info, nullptr, &cache), "Automatic pipeline cache");
    r.pipeline_caches.push_back(cache);
  }
  auto result = c.vkCreateComputePipelines(c.device, cache, 1, &pi, nullptr, &pipeline);
  if (pipeline)
    r.pipelines.push_back(pipeline);
  if (result == VK_SUCCESS && warm) {
    size_t size = 0;
    Check(c.vkGetPipelineCacheData(c.device, cache, &size, nullptr), "Automatic cache size");
    std::vector<unsigned char> serialized(size);
    Check(c.vkGetPipelineCacheData(c.device, cache, &size, serialized.data()), "Automatic cache serialization");
    cache_info.initialDataSize = size;
    cache_info.pInitialData = serialized.data();
    Check(c.vkCreatePipelineCache(c.device, &cache_info, nullptr, &cache), "Automatic cache restore");
    r.pipeline_caches.push_back(cache);
    VkPipelineCreationFeedback feedback{};
    VkPipelineCreationFeedbackCreateInfo feedback_info{VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO};
    feedback_info.pPipelineCreationFeedback = &feedback;
    pi.pNext = &feedback_info;
    pipeline = VK_NULL_HANDLE;
    result = c.vkCreateComputePipelines(c.device, cache, 1, &pi, nullptr, &pipeline);
    if (pipeline)
      r.pipelines.push_back(pipeline);
    c.log("AUTOMATIC_IMAGE_PIPELINE_CACHE serialized=1 recreated=1 hit=" +
          std::to_string(bool(feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT)));
    if (result == VK_SUCCESS && !(feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT))
      throw std::runtime_error("Automatic pipeline cache did not hit");
  }
  Check(result, "compute pipeline");
  return pipeline;
}
bool Execute(Context &c, const Format &f, const std::string &mode,
             bool descriptor_alias = false) {
  Resources r{c};
  bool sampled = mode == "sample" || mode == "write_sample";
  VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            (sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : 0) |
                            (mode != "sample" ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
  VkImageFormatProperties properties{};
  Check(c.vkGetPhysicalDeviceImageFormatProperties(
            c.physical, f.value, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
            usage, 0, &properties),
        "image format usage");
  VkImageCreateInfo image_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_ci.imageType = VK_IMAGE_TYPE_2D;
  image_ci.format = f.value;
  image_ci.extent = {16, 16, 1};
  image_ci.mipLevels = 1;
  image_ci.arrayLayers = 1;
  image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
  image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_ci.usage = usage;
  Check(c.vkCreateImage(c.device, &image_ci, nullptr, &r.image), "image");
  VkMemoryRequirements req{};
  c.vkGetImageMemoryRequirements(c.device, r.image, &req);
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = req.size;
  allocation.memoryTypeIndex = c.MemoryType(req.memoryTypeBits, 0);
  Check(c.vkAllocateMemory(c.device, &allocation, nullptr, &r.image_memory),
        "image memory");
  Check(c.vkBindImageMemory(c.device, r.image, r.image_memory, 0),
        "bind image");
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = r.image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = f.value;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  Check(c.vkCreateImageView(c.device, &vi, nullptr, &r.view), "image view");
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = VK_FILTER_NEAREST;
  si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW =
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.maxLod = 0;
  Check(c.vkCreateSampler(c.device, &si, nullptr, &r.sampler), "sampler");
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = (mode == "unformatted" ? 2 : 1) * 16 * 16 * 4 * sizeof(float);
  bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  Check(c.vkCreateBuffer(c.device, &bi, nullptr, &r.output), "result buffer");
  c.vkGetBufferMemoryRequirements(c.device, r.output, &req);
  allocation.allocationSize = req.size;
  allocation.memoryTypeIndex =
      c.MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
  Check(c.vkAllocateMemory(c.device, &allocation, nullptr, &r.output_memory),
        "result memory");
  Check(c.vkBindBufferMemory(c.device, r.output, r.output_memory, 0),
        "bind result");
  Check(
      c.vkMapMemory(c.device, r.output_memory, 0, VK_WHOLE_SIZE, 0, &r.mapped),
      "map result");
  std::memset(r.mapped, 0xcd, bi.size);
  VkMappedMemoryRange mapped{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
  mapped.memory = r.output_memory;
  mapped.size = VK_WHOLE_SIZE;
  Check(c.vkFlushMappedMemoryRanges(c.device, 1, &mapped), "flush sentinel");
  VkDescriptorSetLayoutBinding bindings[] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       nullptr},
      {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
       VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo li{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  VkDescriptorSetLayoutBinding alias_bindings[4];
  std::copy(std::begin(bindings), std::end(bindings), alias_bindings);
  if (descriptor_alias) {
    alias_bindings[0].descriptorCount = 2;
    alias_bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2,
                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  }
  li.bindingCount = descriptor_alias ? 4 : 3;
  li.pBindings = alias_bindings;
  Check(c.vkCreateDescriptorSetLayout(c.device, &li, nullptr, &r.set_layout),
        "descriptor layout");
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &r.set_layout;
  Check(c.vkCreatePipelineLayout(c.device, &pli, nullptr, &r.layout),
        "pipeline layout");
  VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
  if (descriptor_alias)
    sizes[0].descriptorCount = 4;
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 1;
  dpi.poolSizeCount = 3;
  dpi.pPoolSizes = sizes;
  Check(c.vkCreateDescriptorPool(c.device, &dpi, nullptr, &r.descriptors),
        "descriptor pool");
  VkDescriptorSetAllocateInfo dai{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = r.descriptors;
  dai.descriptorSetCount = 1;
  dai.pSetLayouts = &r.set_layout;
  VkDescriptorSet set{};
  Check(c.vkAllocateDescriptorSets(c.device, &dai, &set), "descriptor set");
  VkDescriptorImageInfo storage{VK_NULL_HANDLE, r.view,
                                VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo storage_aliases[] = {storage, storage};
  VkDescriptorImageInfo texture{r.sampler, r.view,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkDescriptorBufferInfo output{r.output, 0, bi.size};
  std::vector<VkWriteDescriptorSet> writes;
  auto add = [&](uint32_t binding, VkDescriptorType type,
                 const VkDescriptorImageInfo *image,
                 const VkDescriptorBufferInfo *buffer) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount =
        descriptor_alias && type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? 2 : 1;
    w.descriptorType = type;
    w.pImageInfo = image;
    w.pBufferInfo = buffer;
    writes.push_back(w);
  };
  add(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &output);
  if (mode != "sample")
    add(descriptor_alias ? 3 : 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        descriptor_alias ? storage_aliases : &storage, nullptr);
  if (sampled)
    add(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &texture, nullptr);
  c.vkUpdateDescriptorSets(c.device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
  if (descriptor_alias) {
    VkCopyDescriptorSet copy{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};
    copy.srcSet = copy.dstSet = set;
    copy.srcBinding = 3;
    copy.dstBinding = 0;
    copy.descriptorCount = 2;
    c.vkUpdateDescriptorSets(c.device, 0, nullptr, 1, &copy);
  }
  auto pipeline = Pipeline(c, r, f,
                           descriptor_alias         ? "unformatted_array"
                           : mode == "write_sample" ? "write"
                                                    : mode.c_str());
  VkPipeline mutator =
      mode == "unformatted"
          ? Pipeline(c, r, f,
                     descriptor_alias ? "unformatted_modify_array"
                                      : "unformatted_modify")
          : VK_NULL_HANDLE;
  VkPipeline reader =
      mode == "write_sample" ? Pipeline(c, r, f, "sample") : VK_NULL_HANDLE;
  VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpi.queueFamilyIndex = c.family;
  Check(c.vkCreateCommandPool(c.device, &cpi, nullptr, &r.commands),
        "command pool");
  VkCommandBufferAllocateInfo cai{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = r.commands;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VkCommandBuffer cmd{};
  Check(c.vkAllocateCommandBuffers(c.device, &cai, &cmd), "command buffer");
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  Check(c.vkBeginCommandBuffer(cmd, &begin), "begin");
  auto barrier = [&](VkImageLayout from, VkImageLayout to,
                     VkPipelineStageFlags src, VkPipelineStageFlags dst,
                     VkAccessFlags before, VkAccessFlags after) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = before;
    b.dstAccessMask = after;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = r.image;
    b.subresourceRange = vi.subresourceRange;
    c.vkCmdPipelineBarrier(cmd, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
  };
  barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
          VK_ACCESS_TRANSFER_WRITE_BIT);
  VkClearColorValue clear{};
  float first = 1.f;
  if (mode != "modify") {
    if (f.kind == 'u')
      first = 17.f;
    else if (f.kind == 'i' || std::strstr(f.name, "SNORM"))
      first = -1.f;
  }
  if (descriptor_alias)
    first = f.kind == 'u'   ? 17.f
            : f.kind == 'i' ? -17.f
            : std::strstr(f.name, "SNORM") || std::strstr(f.name, "SFLOAT")
                ? -0.5f
                : 0.25f;
  uint32_t exact_bits = 0;
  if (mode == "unformatted") {
    switch (f.value) {
    case VK_FORMAT_R8G8_UINT:
      exact_bits = 165;
      break;
    case VK_FORMAT_R8G8_SINT:
      exact_bits = uint32_t(-91);
      break;
    case VK_FORMAT_R16G16_UINT:
      exact_bits = 42330;
      break;
    case VK_FORMAT_R16G16_SINT:
      exact_bits = uint32_t(-23206);
      break;
    case VK_FORMAT_A2B10G10R10_UINT_PACK32:
      exact_bits = 677;
      break;
    case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT:
      exact_bits = 0x80000011u;
      break;
    default:
      break;
    }
  }
  if (exact_bits)
    first = f.kind == 'i' ? float(int32_t(exact_bits)) : float(exact_bits);
  if (f.kind == 'f') {
    clear.float32[0] = first;
    clear.float32[2] = 1;
  } else if (f.kind == 'i') {
    clear.int32[0] = static_cast<int32_t>(first);
    clear.int32[2] = 1;
  } else {
    clear.uint32[0] = static_cast<uint32_t>(first);
    clear.uint32[2] = 1;
  }
  if (exact_bits) {
    clear.uint32[0] = exact_bits;
    c.log("INTEGER_EXACT_READ format=" + std::string(f.name) +
          " clear_bits=" + std::to_string(exact_bits) + " shader_compare=1");
  }
  c.vkCmdClearColorImage(cmd, r.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clear, 1, &vi.subresourceRange);
  auto target = mode == "sample" ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                 : VK_IMAGE_LAYOUT_GENERAL;
  barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, target,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  c.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r.layout, 0, 1,
                            &set, 0, nullptr);
  c.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  c.vkCmdDispatch(cmd, 2, 2, 1);
  if (reader) {
    barrier(VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT);
    c.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, reader);
    c.vkCmdDispatch(cmd, 2, 2, 1);
  }
  if (mutator) {
    barrier(VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    c.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mutator);
    c.vkCmdDispatch(cmd, 2, 2, 1);
  }
  VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  c.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0,
                         nullptr);
  Check(c.vkEndCommandBuffer(cmd), "end");
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  Check(c.vkCreateFence(c.device, &fi, nullptr, &r.fence), "fence");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  Check(c.vkQueueSubmit(c.queue, 1, &submit, r.fence), "submit");
  c.in_flight = true;
  Check(c.vkWaitForFences(c.device, 1, &r.fence, VK_TRUE, 10'000'000'000ull),
        "GPU wait");
  c.in_flight = false;
  Check(c.vkInvalidateMappedMemoryRanges(c.device, 1, &mapped),
        "invalidate readback");
  if (c.automatic_raw && mode != "sample" &&
      (f.value == VK_FORMAT_R8G8_UNORM || f.value == VK_FORMAT_R8G8_SNORM ||
       f.value == VK_FORMAT_R16G16_SFLOAT || f.value == VK_FORMAT_R32G32_SFLOAT ||
       f.value == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
       f.value == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||
       f.value == VK_FORMAT_R16_SNORM ||
       f.value == VK_FORMAT_R8G8_UINT ||
       f.value == VK_FORMAT_R8G8_SINT ||
       f.value == VK_FORMAT_R16G16_UNORM ||
       f.value == VK_FORMAT_R16G16_SNORM ||
       f.value == VK_FORMAT_R16G16_UINT ||
       f.value == VK_FORMAT_R16G16_SINT ||
       f.value == VK_FORMAT_A2B10G10R10_UINT_PACK32 ||
       f.value == VK_FORMAT_R32G32_UINT ||
       f.value == VK_FORMAT_R32G32_SINT ||
       f.value == VK_FORMAT_R16G16B16A16_SNORM)) {
    auto query = reinterpret_cast<PFN_mesa_uwp_get_raw_image>(
        GetProcAddress(c.library, "mesa_uwp_get_raw_image"));
    VkBuffer raw{};
    dzn_raw_image_layout layout{};
    dzn_raw_image_stats stats{};
    Check(query(c.device, r.image, 0, &raw, &layout, &stats),
          "Automatic raw statistics");
    c.log("AUTOMATIC_RAW " + std::string(f.name) + " mode=" + mode +
          " alias_array=" + std::to_string(descriptor_alias) +
          " texture_to_raw=" + std::to_string(stats.texture_to_raw) +
          " raw_to_texture=" + std::to_string(stats.raw_to_texture) +
          " clean_reuse=" + std::to_string(stats.clean_reuse) +
          " explicit_prepare=0");
    if (!stats.texture_to_raw || ((mode == "modify" || mode == "write_sample" ||
                                   mode == "unformatted") &&
                                  !stats.raw_to_texture))
      throw std::runtime_error("Automatic image lowering was not exercised");
  }

  if (descriptor_alias && f.kind == 'f' && !std::strstr(f.name, "SFLOAT")) {
    float maximum = std::strstr(f.name, "SNORM")
                        ? (std::strstr(f.name, "R16") ? 32767.f : 127.f)
                    : std::strstr(f.name, "R8")                     ? 255.f
                    : f.value == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ? 1023.f
                                                                    : 65535.f;
    first = std::round(first * maximum) / maximum;
  }
  auto values = static_cast<const float *>(r.mapped);
  for (unsigned pixel = 0; pixel < (mutator ? 512u : 256u); ++pixel)
    for (unsigned channel = 0; channel < 4; ++channel) {
      float expected = channel == 0 ? first : channel == 2 ? 1.f : 0.f;
      if (mode == "modify")
        expected = 1.f - expected;
      if (mode == "write_sample") {
        unsigned x = pixel % 16, y = pixel / 16;
        expected = static_cast<float>(channel == 0   ? ((x ^ y) & 1)
                                      : channel == 1 ? (x & 1)
                                      : channel == 2 ? (y & 1)
                                                     : 1);
      }
      if (pixel >= 256) {
        unsigned x = pixel % 16, y = (pixel % 256) / 16;
        float pattern = static_cast<float>(channel == 0   ? ((x ^ y) & 1)
                                           : channel == 1 ? (x & 1)
                                           : channel == 2 ? (y & 1)
                                                          : 1);
        expected = expected != 0 ? pattern : 1.f - pattern;
      }
      if (channel >= f.channels)
        expected = channel == 3 ? 1.f : 0.f;
      float actual = values[pixel * 4 + channel];
      if (!std::isfinite(actual) ||
          std::abs(actual - expected) >
              (descriptor_alias ? 1.f / 65535.f + 0.0001f : 0.0001f)) {
        c.log("MISMATCH pixel=" + std::to_string(pixel) +
              " component=" + std::to_string(channel) + " expected=" +
              std::to_string(expected) + " actual=" + std::to_string(actual));
        return false;
      }
    }
  return true;
}
#include "native_alias.inc"
#include "native_raw.inc"
#include "graphics.inc"
#include "persistent_raw.inc"
#include "dimensions.inc"

int Run(const std::filesystem::path &driver,
        const std::filesystem::path &shaders, Logger logger, bool automatic_raw = true) {
  Context c;
  c.automatic_raw = automatic_raw;
  c.log = std::move(logger);
  c.shaders = shaders;
  c.log("BEGIN DozenDiagnostics schema=19 extent=16x16 samples=1");
  try {
    c.Init(driver);
    for (const auto &f : Formats) {
      VkFormatProperties3 flags{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
      VkFormatProperties2 props{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &flags};
      c.vkGetPhysicalDeviceFormatProperties2(c.physical, f.value, &props);
      c.log("FORMAT " + std::string(f.name) +
            " optimal=" + std::to_string(flags.optimalTilingFeatures) +
            " buffer=" + std::to_string(flags.bufferFeatures));
      for (const std::string mode :
           {"sample", "read", "unformatted", "modify", "write_sample"}) {
        const auto features = flags.optimalTilingFeatures;
        std::string reason;
        if (!(features & VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT))
          reason = "transfer destination unavailable";
        else if ((mode == "sample" || mode == "write_sample") &&
                 !(features & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT))
          reason = "sampled image unavailable";
        else if (mode != "sample" &&
                 !(features & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT))
          reason = "storage image unavailable";
        else if (mode != "sample" && mode != "unformatted" && f.extended &&
                 !c.features.shaderStorageImageExtendedFormats)
          reason = "extended storage formats unavailable";
        else if (mode == "unformatted" &&
                 (!c.features.shaderStorageImageReadWithoutFormat ||
                  !(features &
                    VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT)))
          reason = "read without format unavailable";
        if (mode == "unformatted" && reason.empty() &&
            (!c.features.shaderStorageImageWriteWithoutFormat ||
             !(features &
               VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT)))
          reason =
              "unformatted read-modify-write requires write without format";
        std::string label = std::string(f.name) + " " + mode;
        if (!reason.empty()) {
          ++c.unsupported;
          c.log("UNSUPPORTED " + label + " reason=" + reason);
          continue;
        }
        c.log("CASE " + label);
        try {
          if (Execute(c, f, mode)) {
            ++c.passed;
            c.log(
                "PASS " + label +
                (mode == "unformatted"
                     ? " checked_components=2048 phases=read,read_modify_write"
                     : " checked_components=1024"));
          } else {
            ++c.failed;
            c.log("FAIL " + label);
          }
        } catch (const ApiError &error) {
          ++c.errors;
          c.log("ERROR " + label + " " + error.what());
          if (error.result == VK_ERROR_DEVICE_LOST ||
              error.result == VK_TIMEOUT)
            throw;
        } catch (const std::exception &error) {
          ++c.errors;
          c.log("ERROR " + label + " " + error.what());
        }
      }
    }
    for (const auto &f : Formats) {
      if (f.value != VK_FORMAT_R8_SNORM &&
          f.value != VK_FORMAT_R8G8B8A8_SNORM &&
          f.value != VK_FORMAT_R16_UNORM &&
          f.value != VK_FORMAT_R16G16B16A16_UNORM &&
          f.value != VK_FORMAT_R8_UNORM &&
          f.value != VK_FORMAT_R8G8B8A8_UNORM &&
          f.value != VK_FORMAT_R16_SFLOAT &&
          f.value != VK_FORMAT_R16G16B16A16_SFLOAT &&
          f.value != VK_FORMAT_R32_SFLOAT &&
          f.value != VK_FORMAT_R32G32B32A32_SFLOAT &&
          f.value != VK_FORMAT_R8G8_UNORM &&
          f.value != VK_FORMAT_R8G8_SNORM &&
          f.value != VK_FORMAT_R16G16_SFLOAT &&
          f.value != VK_FORMAT_R32G32_SFLOAT &&
          f.value != VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
          f.value != VK_FORMAT_B10G11R11_UFLOAT_PACK32 &&
          f.value != VK_FORMAT_R16_SNORM &&
          f.value != VK_FORMAT_R8G8_UINT &&
          f.value != VK_FORMAT_R8G8_SINT &&
          f.value != VK_FORMAT_R16G16_UNORM &&
          f.value != VK_FORMAT_R16G16_SNORM &&
          f.value != VK_FORMAT_R16G16_UINT &&
          f.value != VK_FORMAT_R16G16_SINT &&
          f.value != VK_FORMAT_A2B10G10R10_UINT_PACK32 &&
          f.value != VK_FORMAT_R32G32_UINT &&
          f.value != VK_FORMAT_R32G32_SINT &&
          f.value != VK_FORMAT_R16G16B16A16_SNORM)
        continue;
      VkFormatProperties3 flags{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
      VkFormatProperties2 props{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &flags};
      c.vkGetPhysicalDeviceFormatProperties2(c.physical, f.value, &props);
      std::string label =
          std::string(f.name) + " unformatted_copied_alias_array";
      if (!(flags.optimalTilingFeatures &
            VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT)) {
        ++c.unsupported;
        c.log("UNSUPPORTED " + label);
        continue;
      }
      c.log("CASE " + label);
      if (Execute(c, f, "unformatted", true)) {
        ++c.passed;
        c.log("PASS " + label +
              " checked_components=2048 fractional=1 read_alias=1 "
              "write_alias=0 copied_descriptors=2");
      } else {
        ++c.failed;
        c.log("FAIL " + label);
      }
    }
    RunGraphics(c);
    RunSimultaneous(c);
    Dimensions::Run(c);
    VkPhysicalDeviceIDProperties id{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    c.vkGetPhysicalDeviceProperties2(c.physical, &properties);
    bool aliases = NativeAlias::Run(shaders, c.log, id);
    bool raw_formats = NativeRaw::Run(shaders, c.log, id);
    bool persistent_raw = PersistentRaw::Run(c);
    bool automatic_images = PersistentRaw::RunAutomatic(c);
    c.log("END passed=" + std::to_string(c.passed) +
          " failed=" + std::to_string(c.failed) +
          " unsupported=" + std::to_string(c.unsupported) +
          " api_errors=" + std::to_string(c.errors) +
          " native_alias_ok=" + std::to_string(aliases) +
          " native_raw_ok=" + std::to_string(raw_formats) +
          " persistent_raw_ok=" + std::to_string(persistent_raw) +
          " automatic_image_ok=" + std::to_string(automatic_images));
    return c.failed || c.errors || !aliases || !raw_formats || !persistent_raw || !automatic_images
               ? 1
               : 0;
  } catch (const std::exception &error) {
    c.log("FATAL " + std::string(error.what()));
    return 2;
  }
}
} // namespace DozenDiagnostics
