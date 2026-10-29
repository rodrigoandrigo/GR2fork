// SPDX-License-Identifier: MIT
#ifndef DZN_RAW_IMAGE_H
#define DZN_RAW_IMAGE_H
#include <vulkan/vulkan_core.h>
#include <stdint.h>
struct dzn_raw_image_layout {
   uint64_t offset, size;
   uint32_t row_pitch, width, height, depth;
};
struct dzn_raw_image_stats {
   uint64_t texture_to_raw, raw_to_texture, clean_reuse;
};
typedef VkResult (VKAPI_PTR *PFN_mesa_uwp_get_raw_image)(VkDevice, VkImage, uint32_t,
   VkBuffer *, struct dzn_raw_image_layout *, struct dzn_raw_image_stats *);
typedef VkResult (VKAPI_PTR *PFN_mesa_uwp_cmd_prepare_raw_image)(VkCommandBuffer, VkImage,
   VkImageLayout, VkBool32);
#endif
