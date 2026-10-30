// SPDX-License-Identifier: GPL-2.0-or-later
namespace Dimensions {
struct Resources : DozenDiagnostics::Resources {
  std::vector<VkImageView> storage_views;
  VkImage alias{};
  Resources(Context &context) : DozenDiagnostics::Resources{context} {}
  ~Resources() {
    if (!c.in_flight) {
      for (auto view : storage_views)
        c.vkDestroyImageView(c.device, view, nullptr);
      if (alias)
        c.vkDestroyImage(c.device, alias, nullptr);
    }
  }
};
NativeRaw::Pixel Color(unsigned format, unsigned x, unsigned y, unsigned z,
                       unsigned level, bool cube, unsigned store_format = ~0u) {
  NativeRaw::Pixel color{float((x + z + level) % 4) * .25f,
                         float((y + z + level + 1) % 4) * .25f, .5f, 1};
  if (cube)
    color = {float((z + level) % 8) * .125f,
             float((z * 3 + level + 1) % 8) * .125f, .5f, 1};
  return NativeRaw::Unpack(
      format,
      NativeRaw::Pack(store_format == ~0u ? format : store_format, color));
}
bool Execute(Context &c, unsigned format, const std::string &mode,
             bool incompatible = false, bool partial = false,
             bool reinterpret = false, bool dedicated = false) {
  bool cube = mode == "cube" || mode == "cube_array";
  bool one = mode == "1d" || mode == "1d_array";
  bool array = mode.ends_with("array");
  bool volume = mode == "3d";
  unsigned layers = cube ? (array ? 12 : 6) : array ? 2 : 1;
  const Format *f = nullptr;
  for (const auto &candidate : Formats)
    if (!std::strcmp(candidate.name, NativeRaw::Formats[format].name))
      f = &candidate;
  unsigned store_format = format;
  if (reinterpret) {
    const unsigned alternatives[] = {1, 0, 4, 3, 2, 4};
    store_format = alternatives[format];
  }
  Resources r{c};
  VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
  if (incompatible)
    image.flags |= VK_IMAGE_CREATE_ALIAS_BIT;
  if (reinterpret)
    image.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  image.imageType = one      ? VK_IMAGE_TYPE_1D
                    : volume ? VK_IMAGE_TYPE_3D
                             : VK_IMAGE_TYPE_2D;
  image.format = f->value;
  image.extent = {8, one ? 1u : 8u, volume ? 4u : 1u};
  image.mipLevels = 2;
  image.arrayLayers = layers;
  image.samples = VK_SAMPLE_COUNT_1_BIT;
  image.tiling = VK_IMAGE_TILING_OPTIMAL;
  image.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  Check(c.vkCreateImage(c.device, &image, nullptr, &r.image),
        "dimension image");
  VkMemoryRequirements requirements{};
  c.vkGetImageMemoryRequirements(c.device, r.image, &requirements);
  VkMemoryRequirements alias_requirements{};
  VkDeviceSize image_offset = 0;
  if (incompatible) {
    VkImageCreateInfo alias = image;
    alias.imageType = VK_IMAGE_TYPE_2D;
    alias.format = VK_FORMAT_R8G8B8A8_UNORM;
    alias.extent = partial ? VkExtent3D{256, 256, 1} : VkExtent3D{16, 4, 1};
    alias.mipLevels = alias.arrayLayers = 1;
    alias.usage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    Check(c.vkCreateImage(c.device, &alias, nullptr, &r.alias),
          "incompatible alias image");
    c.vkGetImageMemoryRequirements(c.device, r.alias, &alias_requirements);
    if (partial)
      image_offset = requirements.alignment;
  }
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  VkMemoryDedicatedAllocateInfo dedicated_info{
      VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dedicated_info.image = r.image;
  if (dedicated)
    allocation.pNext = &dedicated_info;
  allocation.allocationSize =
      std::max(requirements.size + image_offset, alias_requirements.size);
  allocation.memoryTypeIndex =
      c.MemoryType(requirements.memoryTypeBits &
                       (incompatible ? alias_requirements.memoryTypeBits : ~0u),
                   0);
  Check(c.vkAllocateMemory(c.device, &allocation, nullptr, &r.image_memory),
        "dimension memory");
  Check(c.vkBindImageMemory(c.device, r.image, r.image_memory, image_offset),
        "dimension bind");
  if (incompatible)
    Check(c.vkBindImageMemory(c.device, r.alias, r.image_memory, 0),
          "incompatible alias bind");
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = r.image;
  view.format = f->value;
  view.viewType =
      cube  ? (array ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE)
      : one ? (array ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D)
            : VK_IMAGE_VIEW_TYPE_3D;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, 0, layers};
  Check(c.vkCreateImageView(c.device, &view, nullptr, &r.view),
        "dimension sampled view");
  if (cube)
    view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  view.subresourceRange.levelCount = 1;
  if (reinterpret)
    for (const auto &candidate : Formats)
      if (!std::strcmp(candidate.name, NativeRaw::Formats[store_format].name))
        view.format = candidate.value;
  for (unsigned mip = 0; mip < 2; mip++) {
    view.subresourceRange.baseMipLevel = mip;
    VkImageView storage{};
    Check(c.vkCreateImageView(c.device, &view, nullptr, &storage),
          "dimension storage view");
    r.storage_views.push_back(storage);
  }
  PersistentRaw::Buffer(c, 65536,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        r.output, r.output_memory, r.mapped);
  std::memset(r.mapped, 0xcd, 65536);
  PersistentRaw::Mapped(c, r.output_memory, true);
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
  sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.maxLod = 1;
  Check(c.vkCreateSampler(c.device, &sampler, nullptr, &r.sampler),
        "dimension sampler");
  VkDescriptorSetLayoutBinding bindings[] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2, VK_SHADER_STAGE_COMPUTE_BIT,
       nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       nullptr},
      {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
       VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo set_layout{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_layout.bindingCount = 3;
  set_layout.pBindings = bindings;
  Check(c.vkCreateDescriptorSetLayout(c.device, &set_layout, nullptr,
                                      &r.set_layout),
        "dimension layout");
  VkPipelineLayoutCreateInfo pipeline_layout{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipeline_layout.setLayoutCount = 1;
  pipeline_layout.pSetLayouts = &r.set_layout;
  Check(
      c.vkCreatePipelineLayout(c.device, &pipeline_layout, nullptr, &r.layout),
      "dimension pipeline layout");
  VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
  VkDescriptorPoolCreateInfo pool{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.maxSets = 1;
  pool.poolSizeCount = 3;
  pool.pPoolSizes = sizes;
  Check(c.vkCreateDescriptorPool(c.device, &pool, nullptr, &r.descriptors),
        "dimension pool");
  VkDescriptorSetAllocateInfo set_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  set_info.descriptorPool = r.descriptors;
  set_info.descriptorSetCount = 1;
  set_info.pSetLayouts = &r.set_layout;
  VkDescriptorSet set{};
  Check(c.vkAllocateDescriptorSets(c.device, &set_info, &set), "dimension set");
  VkDescriptorImageInfo storage[] = {
      {VK_NULL_HANDLE, r.storage_views[0], VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, r.storage_views[1], VK_IMAGE_LAYOUT_GENERAL}};
  VkDescriptorImageInfo sampled{r.sampler, r.view, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorBufferInfo buffer{r.output, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet writes[3]{};
  for (unsigned i = 0; i < 3; i++) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorType = bindings[i].descriptorType;
    writes[i].descriptorCount = i == 0 ? 2 : 1;
  }
  writes[0].pImageInfo = storage;
  writes[1].pBufferInfo = &buffer;
  writes[2].pImageInfo = &sampled;
  c.vkUpdateDescriptorSets(c.device, 3, writes, 0, nullptr);
  auto pipeline = Pipeline(c, r, *f, ("dimension_" + mode).c_str());
  VkCommandPoolCreateInfo commands{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  commands.queueFamilyIndex = c.family;
  Check(c.vkCreateCommandPool(c.device, &commands, nullptr, &r.commands),
        "dimension commands");
  VkCommandBufferAllocateInfo command{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  command.commandPool = r.commands;
  command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command.commandBufferCount = 1;
  VkCommandBuffer cmd{};
  Check(c.vkAllocateCommandBuffers(c.device, &command, &cmd),
        "dimension command");
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  Check(c.vkBeginCommandBuffer(cmd, &begin), "dimension begin");
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.dstAccessMask =
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex =
      VK_QUEUE_FAMILY_IGNORED;
  barrier.image = r.image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, 0, layers};
  c.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
  c.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  c.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r.layout, 0, 1,
                            &set, 0, nullptr);
  c.vkCmdDispatch(cmd, 1, 1, 1);
  if (incompatible) {
    for (unsigned cycle = 0; cycle < 2; cycle++) {
      VkMemoryBarrier dependency{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      dependency.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
      dependency.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      c.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &dependency,
                             0, nullptr, 0, nullptr);
      VkImageMemoryBarrier activate{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      activate.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      activate.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      activate.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      activate.srcQueueFamilyIndex = activate.dstQueueFamilyIndex =
          VK_QUEUE_FAMILY_IGNORED;
      activate.image = r.alias;
      activate.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      c.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &activate);
      VkClearColorValue color{{.25f, .5f, .75f, 1}};
      c.vkCmdClearColorImage(cmd, r.alias, VK_IMAGE_LAYOUT_GENERAL, &color, 1,
                             &activate.subresourceRange);
      activate.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      activate.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      activate.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      activate.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      c.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &activate);
      VkBufferImageCopy copy{};
      copy.bufferOffset = 60000;
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent = {16, 4, 1};
      c.vkCmdCopyImageToBuffer(cmd, r.alias,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.output,
                               1, &copy);
      if (cycle == 0) {
        dependency.srcAccessMask =
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        dependency.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        c.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                               &dependency, 0, nullptr, 0, nullptr);
        c.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                               nullptr, 0, nullptr, 1, &barrier);
        c.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        c.vkCmdDispatch(cmd, 1, 1, 1);
      }
    }
  }
  VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  host.srcAccessMask =
      VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  c.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0,
                         nullptr);
  Check(c.vkEndCommandBuffer(cmd), "dimension end");
  VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  Check(c.vkCreateFence(c.device, &fence, nullptr, &r.fence),
        "dimension fence");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  Check(c.vkQueueSubmit(c.queue, 1, &submit, r.fence), "dimension submit");
  c.in_flight = true;
  Check(c.vkWaitForFences(c.device, 1, &r.fence, VK_TRUE, 30'000'000'000ull),
        "dimension wait");
  c.in_flight = false;
  PersistentRaw::Mapped(c, r.output_memory, false);
  auto values = static_cast<NativeRaw::Pixel *>(r.mapped);
  auto equal = [&](unsigned index, const NativeRaw::Pixel &expected) {
    for (unsigned channel = 0; channel < 4; channel++)
      if (!NativeRaw::Equal(values[index][channel], expected[channel])) {
        c.log("DIMENSION_MISMATCH index=" + std::to_string(index) +
              " channel=" + std::to_string(channel) +
              " actual=" + std::to_string(values[index][channel]) +
              " expected=" + std::to_string(expected[channel]));
        return false;
      }
    return true;
  };
  unsigned index = 0;
  for (unsigned mip = 0; mip < 2; mip++) {
    unsigned w = 8 >> mip, h = one ? 1 : 8 >> mip,
             d = volume ? 4 >> mip : layers;
    for (unsigned z = 0; z < d; z++)
      for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++, index++) {
          auto expected = Color(format, x, y, z, mip, cube, store_format);
          if (!equal(index, expected) || !equal(1024 + index, expected))
            return false;
          if (!cube && !equal(2048 + index, expected))
            return false;
        }
  }
  NativeRaw::Pixel query{8, float(one ? (array ? layers : 1) : 8),
                         float(volume          ? 4
                               : cube && array ? 2
                                               : 1),
                         2};
  if (!equal(4095, query))
    return false;
  if (incompatible) {
    auto bytes = static_cast<const uint8_t *>(r.mapped) + 60000;
    for (unsigned pixel = 0; pixel < 64; pixel++)
      if (bytes[pixel * 4] != 64 || bytes[pixel * 4 + 1] != 128 ||
          bytes[pixel * 4 + 2] != 191 || bytes[pixel * 4 + 3] != 255) {
        c.log("INCOMPATIBLE_ALIAS_MISMATCH pixel=" + std::to_string(pixel));
        return false;
      }
  }
  if (cube) {
    auto a = Color(format, 0, 0, 0, 0, true),
         b = Color(format, 0, 0, 4, 0, true),
         d = Color(format, 0, 0, 2, 0, true),
         next = Color(format, 0, 0, 0, 1, true);
    NativeRaw::Pixel edge{}, corner{}, mip{};
    for (unsigned i = 0; i < 4; i++) {
      edge[i] = (a[i] + b[i]) * .5f;
      corner[i] = (a[i] + b[i] + d[i]) / 3;
      mip[i] = (a[i] + next[i]) * .5f;
    }
    NativeRaw::Pixel gather{b[0], a[0], a[0], b[0]};
    if (!equal(4090, gather) || !equal(4091, next) || !equal(4092, edge) ||
        !equal(4093, corner) || !equal(4094, mip))
      return false;
  }
  if (!cube) {
    unsigned layer = array ? 1 : 0;
    auto a = Color(format, 0, 0, layer, 0, false, store_format);
    auto b = Color(format, 1, 0, layer, 0, false, store_format);
    auto next = Color(format, 0, 0, layer, 1, false, store_format);
    NativeRaw::Pixel edge{}, blend{}, spatial{};
    for (unsigned channel = 0; channel < 4; channel++) {
      edge[channel] = (a[channel] + b[channel]) * .5f;
      blend[channel] = (a[channel] + next[channel]) * .5f;
    }
    unsigned count = volume ? 8 : 2;
    for (unsigned i = 0; i < count; i++) {
      auto value = Color(format, i & 1, volume ? (i >> 1) & 1 : 0,
                         volume ? i >> 2 : layer, 0, false, store_format);
      for (unsigned channel = 0; channel < 4; channel++)
        spatial[channel] += value[channel] / count;
    }
    if (!equal(4090, edge) || !equal(4091, spatial) || !equal(4092, next) ||
        !equal(4093, blend))
      return false;
  }
  return true;
}
void Run(Context &c) {
  if (!c.automatic_raw)
    return;
  unsigned passed = 0, failed = 0;
  for (unsigned format = 0; format < 6; format++)
    for (const std::string mode :
         {"1d", "1d_array", "3d", "cube", "cube_array"}) {
      std::string label =
          std::string(NativeRaw::Formats[format].name) + " " + mode;
      c.log("DIMENSION_CASE " + label);
      bool ok = Execute(c, format, mode);
      if (ok) {
        ++passed;
        ++c.passed;
      } else {
        ++failed;
        ++c.failed;
      }
      c.log(std::string(ok ? "DIMENSION_PASS " : "DIMENSION_FAIL ") + label);
    }
  c.log("DIMENSION_END passed=" + std::to_string(passed) +
        " failed=" + std::to_string(failed));
  passed = failed = 0;
  for (unsigned format = 0; format < 6; format++)
    for (bool partial : {false, true}) {
      std::string label = std::string(NativeRaw::Formats[format].name) +
                          (partial ? " partial_overlap" : " full_overlap");
      c.log("INCOMPATIBLE_ALIAS_CASE " + label);
      bool ok = Execute(c, format, "3d", true, partial);
      if (ok) {
        ++passed;
        ++c.passed;
      } else {
        ++failed;
        ++c.failed;
      }
      c.log(std::string(ok ? "INCOMPATIBLE_ALIAS_PASS "
                           : "INCOMPATIBLE_ALIAS_FAIL ") +
            label);
    }
  c.log("INCOMPATIBLE_ALIAS_END passed=" + std::to_string(passed) +
        " failed=" + std::to_string(failed));
  passed = failed = 0;
  for (unsigned format : {0u, 1u, 2u, 4u, 5u}) {
    std::string label = std::string(NativeRaw::Formats[format].name) +
                        " mutable_storage_sampler_views";
    c.log("INCOMPATIBLE_VIEW_CASE " + label);
    bool ok = Execute(c, format, "3d", false, false, true);
    if (ok) {
      ++passed;
      ++c.passed;
    } else {
      ++failed;
      ++c.failed;
    }
    c.log(std::string(ok ? "INCOMPATIBLE_VIEW_PASS "
                         : "INCOMPATIBLE_VIEW_FAIL ") +
          label);
  }
  c.log("INCOMPATIBLE_VIEW_END passed=" + std::to_string(passed) +
        " failed=" + std::to_string(failed));
  passed = failed = 0;
  for (unsigned format = 0; format < 6; format++) {
    std::string label = NativeRaw::Formats[format].name;
    c.log("DEDICATED_IMAGE_CASE " + label);
    bool ok = Execute(c, format, "3d", false, false, false, true);
    if (ok) {
      ++passed;
      ++c.passed;
    } else {
      ++failed;
      ++c.failed;
    }
    c.log(std::string(ok ? "DEDICATED_IMAGE_PASS " : "DEDICATED_IMAGE_FAIL ") +
          label);
  }
  c.log("DEDICATED_IMAGE_END passed=" + std::to_string(passed) +
        " failed=" + std::to_string(failed));
}
} // namespace Dimensions
