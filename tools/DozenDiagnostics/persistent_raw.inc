// SPDX-License-Identifier: GPL-2.0-or-later
namespace PersistentRaw {
struct BridgeResources : Resources {
  VkBuffer upload{};
  VkImage alias{};
  VkDeviceMemory upload_memory{};
  void *upload_mapped{};
  BridgeResources(Context &context) : Resources{context} {}
  ~BridgeResources() {
    if (c.in_flight)
      return;
    if (alias)
      c.vkDestroyImage(c.device, alias, nullptr);
    if (upload_mapped)
      c.vkUnmapMemory(c.device, upload_memory);
    if (upload)
      c.vkDestroyBuffer(c.device, upload, nullptr);
    if (upload_memory)
      c.vkFreeMemory(c.device, upload_memory, nullptr);
  }
};
void Buffer(Context &c, VkDeviceSize size, VkBufferUsageFlags usage,
            VkBuffer &buffer, VkDeviceMemory &memory, void *&mapped) {
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage;
  Check(c.vkCreateBuffer(c.device, &info, nullptr, &buffer),
        "Persistent buffer");
  VkMemoryRequirements req{};
  c.vkGetBufferMemoryRequirements(c.device, buffer, &req);
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = req.size;
  allocation.memoryTypeIndex =
      c.MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
  Check(c.vkAllocateMemory(c.device, &allocation, nullptr, &memory),
        "Persistent buffer memory");
  Check(c.vkBindBufferMemory(c.device, buffer, memory, 0),
        "Persistent bind buffer");
  Check(c.vkMapMemory(c.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped),
        "Persistent map buffer");
}
void Mapped(Context &c, VkDeviceMemory memory, bool flush) {
  VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
  range.memory = memory;
  range.size = VK_WHOLE_SIZE;
  Check(flush ? c.vkFlushMappedMemoryRanges(c.device, 1, &range)
              : c.vkInvalidateMappedMemoryRanges(c.device, 1, &range),
        "Persistent mapped range");
}
bool Execute(Context &c, unsigned format, PFN_mesa_uwp_get_raw_image get_raw,
             PFN_mesa_uwp_cmd_prepare_raw_image prepare,
             bool subresources = false, bool physical_alias = false,
             bool native_consumers = false, bool automatic = false) {
  const auto &native = NativeRaw::Formats[format];
  const Format *f = nullptr;
  for (const auto &candidate : Formats)
    if (std::strcmp(candidate.name, native.name) == 0)
      f = &candidate;
  if (!f)
    throw std::runtime_error("Persistent format unavailable");
  BridgeResources r(c);
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.flags = physical_alias ? VK_IMAGE_CREATE_ALIAS_BIT : 0;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = f->value;
  info.extent = {16, 16, 1};
  info.mipLevels = info.arrayLayers = subresources ? 2 : 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (native_consumers)
    info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  Check(c.vkCreateImage(c.device, &info, nullptr, &r.image),
        "Persistent image");
  VkMemoryRequirements req{};
  c.vkGetImageMemoryRequirements(c.device, r.image, &req);
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = req.size;
  allocation.memoryTypeIndex = c.MemoryType(req.memoryTypeBits, 0);
  Check(c.vkAllocateMemory(c.device, &allocation, nullptr, &r.image_memory),
        "Persistent image memory");
  Check(c.vkBindImageMemory(c.device, r.image, r.image_memory, 0),
        "Persistent bind image");
  if (physical_alias) {
    Check(c.vkCreateImage(c.device, &info, nullptr, &r.alias),
          "Physical alias image");
    VkMemoryRequirements alias_req{};
    c.vkGetImageMemoryRequirements(c.device, r.alias, &alias_req);
    if (alias_req.size != req.size ||
        !(alias_req.memoryTypeBits & (1u << allocation.memoryTypeIndex)))
      throw std::runtime_error("Physical alias memory requirements mismatch");
    Check(c.vkBindImageMemory(c.device, r.alias, r.image_memory, 0),
          "Bind physical alias");
  }
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = r.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = f->value;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, subresources ? 1u : 0u, 1,
                           subresources ? 1u : 0u, 1};
  Check(c.vkCreateImageView(c.device, &view, nullptr, &r.view),
        "Persistent view");
  if (native_consumers) {
    VkAttachmentDescription attachment{};
    attachment.format = f->value;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference reference{0,
                                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = 1;
    pass.pAttachments = &attachment;
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    Check(c.vkCreateRenderPass(c.device, &pass, nullptr, &r.render_pass),
          "Native consumer render pass");
    VkFramebufferCreateInfo framebuffer{
        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer.renderPass = r.render_pass;
    framebuffer.attachmentCount = 1;
    framebuffer.pAttachments = &r.view;
    framebuffer.width = framebuffer.height = 16;
    framebuffer.layers = 1;
    Check(
        c.vkCreateFramebuffer(c.device, &framebuffer, nullptr, &r.framebuffer),
        "Native consumer framebuffer");
  }
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
  sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  Check(c.vkCreateSampler(c.device, &sampler, nullptr, &r.sampler),
        "Persistent sampler");
  VkBuffer raw{};
  dzn_raw_image_layout raw_layout{};
  dzn_raw_image_stats stats{};
  Check(get_raw(c.device, r.image, subresources ? 3 : 0, &raw, &raw_layout,
                &stats),
        "Persistent raw allocation");
  if (physical_alias) {
    VkBuffer alias_buffer{};
    dzn_raw_image_layout alias_layout{};
    Check(get_raw(c.device, r.alias, subresources ? 3 : 0, &alias_buffer,
                  &alias_layout, &stats),
          "Physical alias raw backing");
    if (alias_buffer != raw || alias_layout.offset != raw_layout.offset ||
        alias_layout.size != raw_layout.size)
      throw std::runtime_error("Physical aliases have independent raw backing");
  }
  Buffer(c, 256 * native.bytes,
         VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
         r.upload, r.upload_memory, r.upload_mapped);
  Buffer(c, 6 * 256 * 4 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
         r.output, r.output_memory, r.mapped);
  std::memset(r.mapped, 0xcd, 6 * 256 * 4 * sizeof(float));
  Mapped(c, r.output_memory, true);
  VkDescriptorSetLayoutBinding bindings[] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       nullptr},
      {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
       VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
  if (automatic)
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  VkDescriptorSetLayoutCreateInfo li{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  li.bindingCount = 3;
  li.pBindings = bindings;
  Check(c.vkCreateDescriptorSetLayout(c.device, &li, nullptr, &r.set_layout),
        "Persistent set layout");
  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &r.set_layout;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &push;
  Check(c.vkCreatePipelineLayout(c.device, &pli, nullptr, &r.layout),
        "Persistent pipeline layout");
  VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, automatic ? 1u : 2u},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
  VkDescriptorPoolCreateInfo pool{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.maxSets = 1;
  pool.poolSizeCount = automatic ? 3 : 2;
  pool.pPoolSizes = sizes;
  Check(c.vkCreateDescriptorPool(c.device, &pool, nullptr, &r.descriptors),
        "Persistent descriptor pool");
  VkDescriptorSetAllocateInfo set_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  set_info.descriptorPool = r.descriptors;
  set_info.descriptorSetCount = 1;
  set_info.pSetLayouts = &r.set_layout;
  VkDescriptorSet set{};
  Check(c.vkAllocateDescriptorSets(c.device, &set_info, &set),
        "Persistent descriptor set");
  VkDescriptorBufferInfo buffers[] = {{raw, 0, raw_layout.size},
                                      {r.output, 0, VK_WHOLE_SIZE}};
  VkDescriptorImageInfo image_info{r.sampler, r.view,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  if (automatic)
    image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkWriteDescriptorSet writes[3]{};
  for (unsigned i = 0; i < 3; i++) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = i == 2
                                   ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                   : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (i < 2)
      writes[i].pBufferInfo = &buffers[i];
    else
      writes[i].pImageInfo = &image_info;
  }
  VkDescriptorImageInfo storage_info{VK_NULL_HANDLE, r.view,
                                     VK_IMAGE_LAYOUT_GENERAL};
  if (automatic) {
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pBufferInfo = nullptr;
    writes[0].pImageInfo = &storage_info;
  }
  c.vkUpdateDescriptorSets(c.device, 3, writes, 0, nullptr);
  auto pipeline = Pipeline(c, r, *f, automatic ? "automatic" : "persistent");
  auto sampled = Pipeline(c, r, *f, "persistent_sample");
  VkCommandPoolCreateInfo command_pool{
      VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  command_pool.queueFamilyIndex = c.family;
  Check(c.vkCreateCommandPool(c.device, &command_pool, nullptr, &r.commands),
        "Persistent command pool");
  std::array<std::array<NativeRaw::Pixel, 256>, 2> initial{}, modified{};
  auto allocate_command = [&] {
    VkCommandBufferAllocateInfo ai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = r.commands;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    Check(c.vkAllocateCommandBuffers(c.device, &ai, &cmd),
          "Persistent command");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    Check(c.vkBeginCommandBuffer(cmd, &begin), "Persistent begin");
    return cmd;
  };
  auto barrier = [&](VkCommandBuffer cmd, VkImageLayout before,
                     VkImageLayout after, VkAccessFlags src, VkAccessFlags dst,
                     bool all = false) {
    if (automatic) {
      if (before == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        before = VK_IMAGE_LAYOUT_GENERAL;
      if (after == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        after = VK_IMAGE_LAYOUT_GENERAL;
    }
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = before;
    b.newLayout = after;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = r.image;
    b.subresourceRange =
        all ? VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, 0, 2}
            : view.subresourceRange;
    c.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
  };
  auto dispatch = [&](VkCommandBuffer cmd, VkPipeline p, unsigned cycle,
                      unsigned phase) {
    uint32_t data[] = {raw_layout.row_pitch,
                       phase,
                       uint32_t(raw_layout.offset),
                       cycle * 768,
                       raw_layout.width,
                       raw_layout.height};
    c.vkCmdPushConstants(cmd, r.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 24,
                         data);
    c.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    c.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r.layout, 0,
                              1, &set, 0, nullptr);
    c.vkCmdDispatch(cmd, 2, 2, 1);
  };
  auto submit = [&](VkCommandBuffer cmd) {
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    host.buffer = r.output;
    host.size = VK_WHOLE_SIZE;
    c.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host,
                           0, nullptr);
    Check(c.vkEndCommandBuffer(cmd), "Persistent end");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    Check(c.vkCreateFence(c.device, &fi, nullptr, &r.fence),
          "Persistent fence");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    c.in_flight = true;
    Check(c.vkQueueSubmit(c.queue, 1, &si, r.fence), "Persistent submit");
    Check(c.vkWaitForFences(c.device, 1, &r.fence, VK_TRUE, 10'000'000'000ull),
          "Persistent GPU wait");
    c.in_flight = false;
    c.vkDestroyFence(c.device, r.fence, nullptr);
    r.fence = VK_NULL_HANDLE;
    VkBuffer reused{};
    dzn_raw_image_layout reused_layout{};
    Check(get_raw(c.device, r.image ? r.image : r.alias, subresources ? 3 : 0,
                  &reused, &reused_layout, &stats),
          "Persistent raw reuse");
    if (reused != raw || reused_layout.size != raw_layout.size)
      throw std::runtime_error("Persistent raw backing replaced");
  };
  for (unsigned cycle = 0; cycle < 2; cycle++) {
    for (unsigned pixel = 0; pixel < 256; pixel++) {
      if (pixel % 16 >= raw_layout.width || pixel / 16 >= raw_layout.height)
        continue;
      auto bits = NativeRaw::Pack(
          format, NativeRaw::Initial(format, pixel + cycle * 7));
      if (format == 1 && pixel == 0)
        bits[0] = 0x7f80;
      std::memcpy(static_cast<char *>(r.upload_mapped) +
                      ((pixel / 16) * raw_layout.width + pixel % 16) *
                          native.bytes,
                  bits.data(), native.bytes);
      initial[cycle][pixel] = NativeRaw::Unpack(format, bits);
      auto v = initial[cycle][pixel];
      auto changed = format < 4 ? NativeRaw::Pixel{v[1], v[0], v[2], v[3]}
                                : NativeRaw::Pixel{v[2], v[0], v[1], v[3]};
      modified[cycle][pixel] =
          NativeRaw::Unpack(format, NativeRaw::Pack(format, changed));
    }
    Mapped(c, r.upload_memory, true);
    auto cmd = allocate_command();
    if (physical_alias && cycle == 0) {
      VkImageMemoryBarrier alias_barrier{
          VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      alias_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      alias_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      alias_barrier.srcQueueFamilyIndex = alias_barrier.dstQueueFamilyIndex =
          VK_QUEUE_FAMILY_IGNORED;
      alias_barrier.image = r.alias;
      alias_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                        info.mipLevels, 0, info.arrayLayers};
      c.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &alias_barrier);
      VkMemoryBarrier alias_memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      alias_memory.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      c.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                             &alias_memory, 0, nullptr, 0, nullptr);
    }

    if (subresources && cycle == 0)
      barrier(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
              VK_ACCESS_TRANSFER_WRITE_BIT, true);
    barrier(cmd,
            cycle          ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : subresources ? VK_IMAGE_LAYOUT_GENERAL
                           : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            cycle ? VK_ACCESS_SHADER_READ_BIT : 0,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, subresources ? 1u : 0u,
                             subresources ? 1u : 0u, 1};
    copy.imageExtent = {raw_layout.width, raw_layout.height, 1};
    c.vkCmdCopyBufferToImage(cmd, r.upload, r.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    if (!automatic)
      Check(prepare(cmd, r.image, VK_IMAGE_LAYOUT_GENERAL, VK_TRUE),
            "Persistent raw write authority");
    dispatch(cmd, pipeline, cycle, 0);
    if ((cycle == 0 || automatic) && !physical_alias)
      barrier(cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
              VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    else {
      VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.buffer = raw;
      b.size = VK_WHOLE_SIZE;
      c.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 1, &b, 0, nullptr);
    }
    if (!automatic)
      Check(prepare(cmd, physical_alias ? r.alias : r.image,
                    VK_IMAGE_LAYOUT_GENERAL, VK_FALSE),
            "Persistent clean read reuse");
    dispatch(cmd, pipeline, cycle, 1);
    if (cycle == 0) {
      barrier(cmd, VK_IMAGE_LAYOUT_GENERAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
      dispatch(cmd, sampled, cycle, 0);
    }
    submit(cmd);
  }
  auto cmd = allocate_command();
  barrier(cmd, VK_IMAGE_LAYOUT_GENERAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
          VK_ACCESS_SHADER_READ_BIT);
  dispatch(cmd, sampled, 1, 0);
  submit(cmd);
  if (physical_alias) {
    c.vkDestroyImageView(c.device, r.view, nullptr);
    r.view = VK_NULL_HANDLE;
    c.vkDestroyImage(c.device, r.image, nullptr);
    r.image = VK_NULL_HANDLE;
    cmd = allocate_command();
    VkMemoryBarrier alias_memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    alias_memory.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    alias_memory.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    c.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                           &alias_memory, 0, nullptr, 0, nullptr);
    Check(prepare(cmd, r.alias, VK_IMAGE_LAYOUT_GENERAL, VK_FALSE),
          "Read surviving physical alias");
    dispatch(cmd, pipeline, 1, 1);
    submit(cmd);
  }
  Mapped(c, r.output_memory, false);
  const float *values = static_cast<const float *>(r.mapped);
  for (unsigned cycle = 0; cycle < 2; cycle++)
    for (unsigned phase = 0; phase < 3; phase++)
      for (unsigned pixel = 0; pixel < 256; pixel++)
        for (unsigned channel = 0; channel < 4; channel++) {
          if (pixel % 16 >= raw_layout.width || pixel / 16 >= raw_layout.height)
            continue;
          float expected = phase ? modified[cycle][pixel][channel]
                                 : initial[cycle][pixel][channel];
          float actual =
              values[(cycle * 768 + phase * 256 + pixel) * 4 + channel];
          if (!NativeRaw::Equal(actual, expected)) {
            c.log("PERSISTENT_RAW_MISMATCH cycle=" + std::to_string(cycle) +
                  " phase=" + std::to_string(phase) +
                  " pixel=" + std::to_string(pixel) +
                  " channel=" + std::to_string(channel) +
                  " expected=" + std::to_string(expected) +
                  " actual=" + std::to_string(actual));
            return false;
          }
        }
  if (native_consumers) {
    auto native_cmd = allocate_command();
    barrier(native_cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    if (!automatic)
      Check(prepare(native_cmd, r.image, VK_IMAGE_LAYOUT_GENERAL, VK_TRUE),
            "Native consumer pending raw write");
    dispatch(native_cmd, pipeline, 1, 0);
    VkBufferMemoryBarrier raw_dependency{
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    raw_dependency.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    raw_dependency.dstAccessMask =
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    raw_dependency.srcQueueFamilyIndex = raw_dependency.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    raw_dependency.buffer = raw;
    raw_dependency.size = VK_WHOLE_SIZE;
    if (automatic) {
      VkMemoryBarrier dependency{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      dependency.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      dependency.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      c.vkCmdPipelineBarrier(native_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &dependency,
                             0, nullptr, 0, nullptr);
    } else {
      c.vkCmdPipelineBarrier(native_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                             1, &raw_dependency, 0, nullptr);
    }
    VkClearColorValue clear{{0.25f, 0.5f, 0.75f, 1.0f}};
    c.vkCmdClearColorImage(native_cmd, r.image, VK_IMAGE_LAYOUT_GENERAL, &clear,
                           1, &view.subresourceRange);
    if (automatic)
      barrier(native_cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    if (!automatic)
      Check(prepare(native_cmd, r.image, VK_IMAGE_LAYOUT_GENERAL, VK_FALSE),
            "Reload native clear");
    dispatch(native_cmd, pipeline, 1, 1);
    barrier(native_cmd, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    dispatch(native_cmd, sampled, 1, 0);
    submit(native_cmd);
    Mapped(c, r.output_memory, false);
    NativeRaw::Pixel clear_values = {0.25f, 0.5f, 0.75f, 1.0f};
    auto clear_expected =
        NativeRaw::Unpack(format, NativeRaw::Pack(format, clear_values));
    for (unsigned pixel = 0; pixel < 256; pixel++)
      for (unsigned phase = 1; phase < 3; phase++)
        for (unsigned channel = 0; channel < 4; channel++)
          if (!NativeRaw::Equal(
                  values[(768 + phase * 256 + pixel) * 4 + channel],
                  clear_expected[channel])) {
            c.log("PERSISTENT_RAW_NATIVE_CLEAR_MISMATCH pixel=" +
                  std::to_string(pixel) +
                  " channel=" + std::to_string(channel));
            return false;
          }
    native_cmd = allocate_command();
    barrier(native_cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_SHADER_READ_BIT);
    if (!automatic)
      Check(prepare(native_cmd, r.image, VK_IMAGE_LAYOUT_GENERAL, VK_FALSE),
            "Cache raw before native upload");
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {raw_layout.width, raw_layout.height, 1};
    c.vkCmdCopyBufferToImage(native_cmd, r.upload, r.image,
                             VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    if (automatic)
      barrier(native_cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    if (!automatic)
      Check(prepare(native_cmd, r.image, VK_IMAGE_LAYOUT_GENERAL, VK_FALSE),
            "Reload native upload");
    dispatch(native_cmd, pipeline, 1, 1);
    barrier(native_cmd, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    dispatch(native_cmd, sampled, 1, 0);
    submit(native_cmd);
    Mapped(c, r.output_memory, false);
    for (unsigned pixel = 0; pixel < 256; pixel++)
      for (unsigned phase = 1; phase < 3; phase++)
        for (unsigned channel = 0; channel < 4; channel++)
          if (!NativeRaw::Equal(
                  values[(768 + phase * 256 + pixel) * 4 + channel],
                  initial[1][pixel][channel])) {
            c.log("PERSISTENT_RAW_NATIVE_UPLOAD_MISMATCH pixel=" +
                  std::to_string(pixel) +
                  " channel=" + std::to_string(channel));
            return false;
          }
    native_cmd = allocate_command();
    barrier(native_cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_SHADER_READ_BIT);
    if (!automatic)
      Check(prepare(native_cmd, r.image, VK_IMAGE_LAYOUT_GENERAL, VK_FALSE),
            "Cache raw before attachment clear");
    VkClearValue attachment_clear{};
    attachment_clear.color = clear;
    VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    render.renderPass = r.render_pass;
    render.framebuffer = r.framebuffer;
    render.renderArea.extent = {16, 16};
    render.clearValueCount = 1;
    render.pClearValues = &attachment_clear;
    c.vkCmdBeginRenderPass(native_cmd, &render, VK_SUBPASS_CONTENTS_INLINE);
    c.vkCmdEndRenderPass(native_cmd);
    if (automatic)
      barrier(native_cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    if (!automatic)
      Check(prepare(native_cmd, r.image, VK_IMAGE_LAYOUT_GENERAL, VK_FALSE),
            "Reload native attachment clear");
    dispatch(native_cmd, pipeline, 1, 1);
    barrier(native_cmd, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    dispatch(native_cmd, sampled, 1, 0);
    submit(native_cmd);
    Mapped(c, r.output_memory, false);
    for (unsigned pixel = 0; pixel < 256; pixel++)
      for (unsigned phase = 1; phase < 3; phase++)
        for (unsigned channel = 0; channel < 4; channel++)
          if (!NativeRaw::Equal(
                  values[(768 + phase * 256 + pixel) * 4 + channel],
                  clear_expected[channel])) {
            c.log("PERSISTENT_RAW_ATTACHMENT_MISMATCH pixel=" +
                  std::to_string(pixel) +
                  " channel=" + std::to_string(channel));
            return false;
          }
    c.log("PERSISTENT_RAW_NATIVE_CONSUMERS clear=1 upload=1 attachment=1 "
          "sampled=1 raw_reload=1");
  }
  if (automatic) {
    auto bits_cmd = allocate_command();
    barrier(bits_cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, subresources ? 1u : 0u,
                             subresources ? 1u : 0u, 1};
    copy.imageExtent = {raw_layout.width, raw_layout.height, 1};
    c.vkCmdCopyImageToBuffer(bits_cmd, r.image,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.upload, 1,
                             &copy);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    host.buffer = r.upload;
    host.size = VK_WHOLE_SIZE;
    c.vkCmdPipelineBarrier(bits_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host,
                           0, nullptr);
    submit(bits_cmd);
    Mapped(c, r.upload_memory, false);
    for (unsigned y = 0; y < raw_layout.height; y++)
      for (unsigned x = 0; x < raw_layout.width; x++) {
        auto expected = NativeRaw::Pack(
            format, native_consumers
                        ? NativeRaw::Pixel{0.25f, 0.5f, 0.75f, 1.0f}
                        : modified[1][y * 16 + x]);
        const auto *actual =
            static_cast<const unsigned char *>(r.upload_mapped) +
            (y * raw_layout.width + x) * native.bytes;
        if (!NativeRaw::EqualBits(format, actual, expected, native.bytes)) {
          c.log("AUTOMATIC_IMAGE_BITS_MISMATCH x=" + std::to_string(x) +
                " y=" + std::to_string(y));
          return false;
        }
      }
    c.log("AUTOMATIC_IMAGE_VERIFIED explicit_prepare=0 image_load_store=1 "
          "sampled=1 native_bits=1 attachment=" +
          std::to_string(native_consumers));
    return stats.texture_to_raw > 0 && stats.raw_to_texture > 0;
  }
  c.log("PERSISTENT_RAW_STATS texture_to_raw=" +
        std::to_string(stats.texture_to_raw) +
        " raw_to_texture=" + std::to_string(stats.raw_to_texture) +
        " clean_reuse=" + std::to_string(stats.clean_reuse) +
        " stable_buffer=1 submissions=" +
        std::to_string(native_consumers ? 6
                       : physical_alias ? 4
                                        : 3));
  unsigned expected_copies = subresources ? 8 : 2;
  if (c.automatic_raw && !automatic) {
    if (native_consumers)
      return stats.texture_to_raw == 9 && stats.raw_to_texture == 3 &&
             stats.clean_reuse == 6;
    return stats.texture_to_raw ==
               expected_copies + 1 + unsigned(physical_alias) &&
           stats.raw_to_texture == expected_copies &&
           stats.clean_reuse == expected_copies + 1;
  }
  if (native_consumers)
    return stats.texture_to_raw == 8 && stats.raw_to_texture == 3 &&
           stats.clean_reuse == 2;
  if (physical_alias)
    return stats.texture_to_raw == expected_copies + 1 &&
           stats.raw_to_texture == expected_copies &&
           stats.clean_reuse == expected_copies;
  return stats.texture_to_raw == expected_copies &&
         stats.raw_to_texture == expected_copies &&
         stats.clean_reuse == expected_copies;
}
bool Run(Context &c) {
  auto get_raw = reinterpret_cast<PFN_mesa_uwp_get_raw_image>(
      GetProcAddress(c.library, "mesa_uwp_get_raw_image"));
  auto prepare = reinterpret_cast<PFN_mesa_uwp_cmd_prepare_raw_image>(
      GetProcAddress(c.library, "mesa_uwp_cmd_prepare_raw_image"));
  if (!get_raw || !prepare)
    throw std::runtime_error("Persistent driver bridge unavailable");
  c.log("PERSISTENT_RAW_BEGIN driver_owned_buffer=1 "
        "production_unformatted_support_unchanged=1");
  unsigned passed = 0, failed = 0;
  for (unsigned format = 0; format < std::size(NativeRaw::Formats); format++) {
    const char *name = NativeRaw::Formats[format].name;
    c.log("PERSISTENT_RAW_CASE " + std::string(name));
    bool ok = Execute(c, format, get_raw, prepare);
    (ok ? passed : failed)++;
    c.log(std::string(ok ? "PERSISTENT_RAW_PASS " : "PERSISTENT_RAW_FAIL ") +
          name + " checked_components=6144");
    std::string subcase = std::string(name) + " mip1_layer1";
    c.log("PERSISTENT_RAW_CASE " + subcase);
    ok = Execute(c, format, get_raw, prepare, true);
    (ok ? passed : failed)++;
    c.log(std::string(ok ? "PERSISTENT_RAW_PASS " : "PERSISTENT_RAW_FAIL ") +
          subcase + " checked_components=1536");
    std::string alias_case = std::string(name) + " physical_alias";
    c.log("PERSISTENT_RAW_CASE " + alias_case);
    ok = Execute(c, format, get_raw, prepare, false, true);
    (ok ? passed : failed)++;
    c.log(std::string(ok ? "PERSISTENT_RAW_PASS " : "PERSISTENT_RAW_FAIL ") +
          alias_case + " checked_components=6144 surviving_alias=1");
    std::string native_case = std::string(name) + " native_consumers";
    c.log("PERSISTENT_RAW_CASE " + native_case);
    ok = Execute(c, format, get_raw, prepare, false, false, true);
    (ok ? passed : failed)++;
    c.log(std::string(ok ? "PERSISTENT_RAW_PASS " : "PERSISTENT_RAW_FAIL ") +
          native_case + " clear_upload_sampled=1");
  }
  c.log("PERSISTENT_RAW_END passed=" + std::to_string(passed) +
        " failed=" + std::to_string(failed));
  return !failed;
}
bool RunAutomatic(Context &c) {
  if (!c.automatic_raw) {
    c.log("AUTOMATIC_IMAGE_SKIP production_profile=1");
    return true;
  }
  auto get_raw = reinterpret_cast<PFN_mesa_uwp_get_raw_image>(
      GetProcAddress(c.library, "mesa_uwp_get_raw_image"));
  auto prepare = reinterpret_cast<PFN_mesa_uwp_cmd_prepare_raw_image>(
      GetProcAddress(c.library, "mesa_uwp_cmd_prepare_raw_image"));
  unsigned passed = 0, failed = 0;
  c.log("AUTOMATIC_IMAGE_BEGIN explicit_prepare=0 vulkan_image_shaders=1");
  for (unsigned format = 0; format < std::size(NativeRaw::Formats); format++)
    for (unsigned variant = 0; variant < 3; variant++) {
      bool mip_layer = variant == 1;
      bool native_consumers = variant == 2;
      std::string label = NativeRaw::Formats[format].name;
      if (mip_layer)
        label += " mip1_layer1";
      if (native_consumers)
        label += " native_consumers";
      c.log("AUTOMATIC_IMAGE_CASE " + label);
      bool ok = Execute(c, format, get_raw, prepare, mip_layer, false,
                        native_consumers, true);
      (ok ? passed : failed)++;
      c.log(
          std::string(ok ? "AUTOMATIC_IMAGE_PASS " : "AUTOMATIC_IMAGE_FAIL ") +
          label);
    }
  c.log("AUTOMATIC_IMAGE_END passed=" + std::to_string(passed) +
        " failed=" + std::to_string(failed));
  return !failed;
}
} // namespace PersistentRaw
