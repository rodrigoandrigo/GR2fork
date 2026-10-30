// SPDX-License-Identifier: GPL-2.0-or-later
bool Graphics(Context &c, const Format &f, const std::string &mode) {
  Resources r{c};
  bool simultaneous = mode.starts_with("simultaneous_");
  bool sampler_readonly = mode == "simultaneous_sampler_readonly_attachment";
  bool effects = mode.ends_with("side_effects");
  bool partial = mode == "point_cull_both" || mode == "line_cull_both";
  auto make_image = [&](VkFormat format, VkImageUsageFlags usage,
                        VkImageAspectFlags aspect, VkImage &image,
                        VkDeviceMemory &memory, VkImageView &view) {
    VkImageFormatProperties supported{};
    Check(c.vkGetPhysicalDeviceImageFormatProperties(
              c.physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
              usage, 0, &supported),
          "graphics image format");
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {16, 16, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    Check(c.vkCreateImage(c.device, &ci, nullptr, &image), "graphics image");
    VkMemoryRequirements req{};
    c.vkGetImageMemoryRequirements(c.device, image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = c.MemoryType(req.memoryTypeBits, 0);
    Check(c.vkAllocateMemory(c.device, &ai, nullptr, &memory),
          "graphics image memory");
    Check(c.vkBindImageMemory(c.device, image, memory, 0),
          "bind graphics image");
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    Check(c.vkCreateImageView(c.device, &vi, nullptr, &view),
          "graphics image view");
  };
  make_image(f.value,
             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 (simultaneous
                      ? VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                      : 0),
             VK_IMAGE_ASPECT_COLOR_BIT, r.image, r.image_memory, r.view);
  const auto depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
  make_image(depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
             VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, r.depth,
             r.depth_memory, r.depth_view);
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = 8192;
  bi.usage =
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  Check(c.vkCreateBuffer(c.device, &bi, nullptr, &r.output),
        "graphics readback buffer");
  VkMemoryRequirements req{};
  c.vkGetBufferMemoryRequirements(c.device, r.output, &req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex =
      c.MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
  Check(c.vkAllocateMemory(c.device, &ai, nullptr, &r.output_memory),
        "graphics readback memory");
  Check(c.vkBindBufferMemory(c.device, r.output, r.output_memory, 0),
        "bind graphics readback");
  Check(
      c.vkMapMemory(c.device, r.output_memory, 0, VK_WHOLE_SIZE, 0, &r.mapped),
      "map graphics readback");
  std::memset(r.mapped, 0xcd, 8192);
  static_cast<uint32_t *>(r.mapped)[1024] = 0;
  static_cast<uint32_t *>(r.mapped)[1025] = 0;
  VkMappedMemoryRange mapped{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
  mapped.memory = r.output_memory;
  mapped.size = VK_WHOLE_SIZE;
  Check(c.vkFlushMappedMemoryRanges(c.device, 1, &mapped),
        "flush graphics sentinel");
  VkAttachmentDescription attachments[2]{};
  attachments[0].format = f.value;
  attachments[1].format = depth_format;
  for (auto &a : attachments) {
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  }
  attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  if (simultaneous)
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkAttachmentReference color{
      0, simultaneous ? VK_IMAGE_LAYOUT_GENERAL
                      : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depth{1,
                              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color;
  subpass.pDepthStencilAttachment = &depth;
  VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rp.attachmentCount = 2;
  rp.pAttachments = attachments;
  rp.subpassCount = 1;
  rp.pSubpasses = &subpass;
  Check(c.vkCreateRenderPass(c.device, &rp, nullptr, &r.render_pass),
        "render pass");
  VkImageView views[] = {r.view, r.depth_view};
  VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fb.renderPass = r.render_pass;
  fb.attachmentCount = 2;
  fb.pAttachments = views;
  fb.width = fb.height = 16;
  fb.layers = 1;
  Check(c.vkCreateFramebuffer(c.device, &fb, nullptr, &r.framebuffer),
        "framebuffer");
  VkPushConstantRange push{
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
  VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  li.pushConstantRangeCount = 1;
  li.pPushConstantRanges = &push;
  VkDescriptorSet effects_set{};
  if (effects) {
    VkDescriptorSetLayoutBinding binding{
        0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo ci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 1;
    ci.pBindings = &binding;
    Check(c.vkCreateDescriptorSetLayout(c.device, &ci, nullptr, &r.set_layout),
          "effects descriptor layout");
    li.setLayoutCount = 1;
    li.pSetLayouts = &r.set_layout;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pool{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    Check(c.vkCreateDescriptorPool(c.device, &pool, nullptr, &r.descriptors),
          "effects descriptor pool");
    VkDescriptorSetAllocateInfo ai{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = r.descriptors;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &r.set_layout;
    Check(c.vkAllocateDescriptorSets(c.device, &ai, &effects_set),
          "effects descriptor set");
    VkDescriptorBufferInfo buffer{r.output, 4096, 8};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = effects_set;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &buffer;
    c.vkUpdateDescriptorSets(c.device, 1, &write, 0, nullptr);
  }
  if (simultaneous) {
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    Check(c.vkCreateSampler(c.device, &sampler, nullptr, &r.sampler),
          "simultaneous sampler");
    bool separate = mode == "simultaneous_separate_storage_sampler_attachment";
    bool immutable =
        mode == "simultaneous_immutable_storage_sampler_attachment";
    unsigned descriptor_count = separate ? 4 : 3;
    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr}};
    if (separate)
      bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    if (immutable)
      bindings[2].pImmutableSamplers = &r.sampler;
    VkDescriptorSetLayoutCreateInfo ci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = descriptor_count;
    ci.pBindings = bindings;
    Check(c.vkCreateDescriptorSetLayout(c.device, &ci, nullptr, &r.set_layout),
          "simultaneous layout");
    li.setLayoutCount = 1;
    li.pSetLayouts = &r.set_layout;
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
    if (separate)
      sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    VkDescriptorPoolCreateInfo pool{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = descriptor_count;
    pool.pPoolSizes = sizes;
    Check(c.vkCreateDescriptorPool(c.device, &pool, nullptr, &r.descriptors),
          "simultaneous pool");
    VkDescriptorSetAllocateInfo ai{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = r.descriptors;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &r.set_layout;
    Check(c.vkAllocateDescriptorSets(c.device, &ai, &effects_set),
          "simultaneous set");
    VkDescriptorImageInfo image{r.sampler, r.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo buffer{r.output, 4096, 8};
    VkWriteDescriptorSet writes[4]{};
    for (unsigned i = 0; i < descriptor_count; i++) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = effects_set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = bindings[i].descriptorType;
      if (i == 1)
        writes[i].pBufferInfo = &buffer;
      else
        writes[i].pImageInfo = &image;
    }
    c.vkUpdateDescriptorSets(c.device, descriptor_count, writes, 0, nullptr);
  }
  Check(c.vkCreatePipelineLayout(c.device, &li, nullptr, &r.layout),
        "graphics layout");
  auto module = [&](const char *name) {
    std::ifstream file(c.shaders / name, std::ios::binary | std::ios::ate);
    if (!file)
      throw std::runtime_error("Missing graphics shader");
    auto bytes = static_cast<size_t>(file.tellg());
    std::vector<uint32_t> code(bytes / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char *>(code.data()), bytes);
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code.data();
    VkShaderModule result{};
    Check(c.vkCreateShaderModule(c.device, &ci, nullptr, &result),
          "graphics shader");
    r.shaders.push_back(result);
    return result;
  };
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = stages[1].sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[0].module = module(effects ? "effects.vert.spv" : "graphics.vert.spv");
  std::string fragment = mode + ".frag.spv";
  stages[1].module = module(simultaneous ? fragment.c_str()
                            : effects    ? "effects.frag.spv"
                                         : "graphics.frag.spv");
  stages[0].pName = stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vertex{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology =
      mode.starts_with("point_")  ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
      : mode.starts_with("line_") ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                  : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkViewport vp{0, 0, 16, 16, 0, 1};
  VkRect2D scissor{{0, 0}, {16, 16}};
  VkPipelineViewportStateCreateInfo viewport{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = viewport.scissorCount = 1;
  viewport.pViewports = &vp;
  viewport.pScissors = &scissor;
  if (simultaneous)
    scissor.extent.width = 7;
  VkPipelineRasterizationStateCreateInfo raster{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1;
  if (mode.starts_with("cull_") || mode.starts_with("point_") ||
      mode.starts_with("line_"))
    raster.cullMode = VK_CULL_MODE_FRONT_AND_BACK;
  raster.rasterizerDiscardEnable = mode == "rasterizer_discard_side_effects";
  VkPipelineMultisampleStateCreateInfo ms{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blend{};
  blend.colorWriteMask = 15;
  if (mode == "write_r")
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
  if (mode == "blend_alpha") {
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  }
  if (mode == "blend_add") {
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  }
  VkPipelineColorBlendStateCreateInfo cb{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &blend;
  VkPipelineDepthStencilStateCreateInfo ds{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable =
      mode.starts_with("depth_") || mode == "point_cull_depth_greater";
  ds.depthWriteEnable = mode != "depth_no_write";
  ds.depthCompareOp =
      (mode == "depth_greater" || mode == "point_cull_depth_greater")
          ? VK_COMPARE_OP_GREATER
          : VK_COMPARE_OP_LESS;
  ds.stencilTestEnable = mode.starts_with("stencil_");
  ds.front.compareOp = VK_COMPARE_OP_ALWAYS;
  ds.front.failOp = ds.front.depthFailOp = VK_STENCIL_OP_KEEP;
  ds.front.passOp = VK_STENCIL_OP_REPLACE;
  ds.front.compareMask = ds.front.writeMask = 0xff;
  ds.front.reference = 3;
  ds.back = ds.front;
  auto pipeline = [&]() {
    VkGraphicsPipelineCreateInfo pi{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vertex;
    pi.pInputAssemblyState = &assembly;
    pi.pViewportState = &viewport;
    pi.pRasterizationState = &raster;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.layout = r.layout;
    pi.renderPass = r.render_pass;
    VkPipeline result{};
    auto status = c.vkCreateGraphicsPipelines(c.device, VK_NULL_HANDLE, 1, &pi,
                                              nullptr, &result);
    if (result)
      r.pipelines.push_back(result);
    Check(status, "graphics pipeline");
    return result;
  };
  auto first = pipeline();
  VkPipeline second{};
  if (mode == "depth_write" || mode == "depth_no_write" ||
      (mode.starts_with("stencil_") || mode == "cull_restore")) {
    ds.depthWriteEnable = VK_TRUE;
    if (ds.stencilTestEnable) {
      ds.front.compareOp = VK_COMPARE_OP_EQUAL;
      ds.front.reference = mode == "stencil_equal" ? 3 : 4;
      ds.front.passOp = VK_STENCIL_OP_KEEP;
      ds.back = ds.front;
    }
    if (mode == "cull_restore")
      raster.cullMode = VK_CULL_MODE_NONE;
    second = pipeline();
  }
  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.queueFamilyIndex = c.family;
  Check(c.vkCreateCommandPool(c.device, &pool, nullptr, &r.commands),
        "graphics command pool");
  VkCommandBufferAllocateInfo cai{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = r.commands;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VkCommandBuffer cmd{};
  Check(c.vkAllocateCommandBuffers(c.device, &cai, &cmd),
        "graphics command buffer");
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  Check(c.vkBeginCommandBuffer(cmd, &begin), "graphics begin");
  VkClearValue clears[2]{};
  clears[0].color.float32[2] = clears[0].color.float32[3] = 1;
  if (sampler_readonly) {
    clears[0].color.float32[0] = .25f;
    clears[0].color.float32[1] = .5f;
  }
  clears[1].depthStencil = {1, 0};
  VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  pass.renderPass = r.render_pass;
  pass.framebuffer = r.framebuffer;
  pass.renderArea = {{0, 0}, {16, 16}};
  pass.clearValueCount = 2;
  pass.pClearValues = clears;
  c.vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
  struct Push {
    float color[4];
    float depth;
    uint32_t vertex, instance, padding;
  } data{{1, 0, 0, 1}, .25f, 0, 0, 0};
  if (mode == "blend_alpha")
    data.color[3] = .5f;
  if (mode == "base_vertex_instance") {
    data.vertex = 3;
    data.instance = 7;
  }
  if (mode.starts_with("point_"))
    data.padding = 1;
  if (mode.starts_with("line_"))
    data.padding = 2;
  if (effects || simultaneous)
    c.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.layout, 0,
                              1, &effects_set, 0, nullptr);
  auto draw = [&](VkPipeline p) {
    c.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    c.vkCmdPushConstants(cmd, r.layout, push.stageFlags, 0, sizeof(data),
                         &data);
    c.vkCmdDraw(cmd,
                mode.starts_with("point_")  ? 1
                : mode.starts_with("line_") ? 2
                                            : 3,
                1, data.vertex, data.instance);
  };
  draw(first);
  if (simultaneous)
    c.vkCmdDraw(cmd, 3, 1, 0, 0);
  if (second) {
    data.color[0] = 0;
    data.color[1] = 1;
    data.depth = .75f;
    draw(second);
  }
  c.vkCmdEndRenderPass(cmd);
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                          (simultaneous ? VK_ACCESS_SHADER_WRITE_BIT : 0);
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barrier.oldLayout = simultaneous ? VK_IMAGE_LAYOUT_GENERAL
                                   : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex =
      VK_QUEUE_FAMILY_IGNORED;
  barrier.image = r.image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  c.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {16, 16, 1};
  c.vkCmdCopyImageToBuffer(cmd, r.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           r.output, 1, &copy);
  VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  host.srcAccessMask =
      VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  c.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0,
                         nullptr);
  Check(c.vkEndCommandBuffer(cmd), "graphics end");
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  Check(c.vkCreateFence(c.device, &fi, nullptr, &r.fence), "graphics fence");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  Check(c.vkQueueSubmit(c.queue, 1, &submit, r.fence), "graphics submit");
  c.in_flight = true;
  Check(c.vkWaitForFences(c.device, 1, &r.fence, VK_TRUE, 10'000'000'000ull),
        "graphics GPU wait");
  c.in_flight = false;
  Check(c.vkInvalidateMappedMemoryRanges(c.device, 1, &mapped),
        "graphics invalidate");
  if (simultaneous) {
    unsigned format = 0;
    while (format < 6 && std::strcmp(f.name, NativeRaw::Formats[format].name))
      ++format;
    if (format == 6)
      throw std::runtime_error("Unknown simultaneous format");
    auto count = static_cast<uint32_t *>(r.mapped)[1024];
    c.log("SIMULTANEOUS_SIDE_EFFECTS fragments=" + std::to_string(count));
    if (count != (sampler_readonly ? 0 : 224))
      return false;
    bool storage =
        mode != "simultaneous_sampler_attachment" && !sampler_readonly;
    bool sampled = mode != "simultaneous_storage_attachment";
    for (unsigned pixel = 0; pixel < 256; pixel++) {
      unsigned x = pixel % 16;
      NativeRaw::Pixel expected = x < 7 ? NativeRaw::Pixel{.125f, .25f, .5f, 1}
                                        : NativeRaw::Pixel{0, 0, 1, 1};
      if (storage && x >= 7 && x < 14)
        expected = {.75f, .5f, .25f, 1};
      if (sampled && x < 7) {
        expected = storage ? NativeRaw::Pixel{.75f, .5f, .25f, 1}
                           : NativeRaw::Pixel{0, 0, 1, 1};
        expected = NativeRaw::Unpack(format, NativeRaw::Pack(format, expected));
      }
      if (sampler_readonly) {
        expected = {.25f, .5f, 1, 1};
        if (x < 7) {
          auto source =
              NativeRaw::Unpack(format, NativeRaw::Pack(format, expected));
          expected = {1 - source[0], 0, 0, 1};
        }
      }
      auto bits = NativeRaw::Pack(format, expected);
      auto actual = static_cast<const char *>(r.mapped) +
                    pixel * NativeRaw::Formats[format].bytes;
      if (!NativeRaw::EqualBits(format, actual, bits,
                                NativeRaw::Formats[format].bytes)) {
        c.log("SIMULTANEOUS_MISMATCH pixel=" + std::to_string(pixel));
        return false;
      }
    }
    return true;
  }
  std::array<float, 4> expected{1, 0, 0, 1};
  if (mode == "cull_both" || mode == "cull_both_side_effects" ||
      mode == "rasterizer_discard_side_effects" ||
      mode == "point_cull_depth_greater" || mode == "depth_greater")
    expected = {0, 0, 1, 1};
  if (mode == "write_r" || mode == "blend_add")
    expected = {1, 0, 1, 1};
  if (mode == "blend_alpha")
    expected = {.5f, 0, .5f, .5f};
  if (mode == "depth_no_write" || mode == "stencil_equal" ||
      mode == "cull_restore")
    expected = {0, 1, 0, 1};
  if (effects) {
    auto vertex_writes = static_cast<uint32_t *>(r.mapped)[1024];
    auto fragment_writes = static_cast<uint32_t *>(r.mapped)[1025];
    c.log("SIDE_EFFECTS vertex=" + std::to_string(vertex_writes) +
          " fragment=" + std::to_string(fragment_writes));
    if (vertex_writes != 3 || fragment_writes != 0)
      return false;
  }
  unsigned colored{};
  for (unsigned pixel = 0; pixel < 256; ++pixel)
    for (unsigned channel = 0; channel < 4; ++channel) {
      float actual{};
      if (f.value == VK_FORMAT_R32G32B32A32_SFLOAT)
        actual = static_cast<float *>(r.mapped)[pixel * 4 + channel];
      else if (f.value == VK_FORMAT_R16G16B16A16_SFLOAT) {
        auto bits = static_cast<uint16_t *>(r.mapped)[pixel * 4 + channel];
        unsigned exponent = (bits >> 10) & 31, fraction = bits & 1023;
        actual = exponent == 31 ? (fraction ? NAN : INFINITY)
                 : exponent     ? std::ldexp(1.f + float(fraction) / 1024,
                                             int(exponent) - 15)
                                : std::ldexp(float(fraction), -24);
        if (bits & 0x8000)
          actual = -actual;
      } else {
        unsigned index = channel;
        if (f.value == VK_FORMAT_B8G8R8A8_UNORM && channel < 3)
          index = 2 - channel;
        actual = static_cast<uint8_t *>(r.mapped)[pixel * 4 + index] / 255.f;
      }
      if (partial) {
        auto read_red = [&]() {
          if (f.value == VK_FORMAT_R32G32B32A32_SFLOAT)
            return static_cast<float *>(r.mapped)[pixel * 4] > 0.5f;
          if (f.value == VK_FORMAT_R16G16B16A16_SFLOAT)
            return static_cast<uint16_t *>(r.mapped)[pixel * 4] == 0x3c00;
          return static_cast<uint8_t *>(
                     r.mapped)[pixel * 4 +
                               (f.value == VK_FORMAT_B8G8R8A8_UNORM ? 2 : 0)] >
                 128;
        };
        bool red = read_red();
        if (channel == 0 && red)
          ++colored;
        expected = red ? std::array<float, 4>{1, 0, 0, 1}
                       : std::array<float, 4>{0, 0, 1, 1};
      }
      float tolerance = f.value == VK_FORMAT_R8G8B8A8_UNORM ||
                                f.value == VK_FORMAT_B8G8R8A8_UNORM
                            ? 1.1f / 255.f
                            : .0001f;
      if (!std::isfinite(actual) ||
          std::abs(actual - expected[channel]) > tolerance) {
        c.log("MISMATCH pixel=" + std::to_string(pixel) +
              " component=" + std::to_string(channel) +
              " expected=" + std::to_string(expected[channel]) +
              " actual=" + std::to_string(actual));
        return false;
      }
    }
  if (partial) {
    c.log("COVERAGE colored_pixels=" + std::to_string(colored));
    if (!colored)
      return false;
  }
  return true;
}
void RunGraphics(Context &c) {
  const Format targets[] = {
      {VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM"},
      {VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM"},
      {VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT"},
      {VK_FORMAT_R32G32B32A32_SFLOAT, "R32G32B32A32_SFLOAT"}};
  VkFormatProperties2 depth{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
  c.vkGetPhysicalDeviceFormatProperties2(c.physical,
                                         VK_FORMAT_D32_SFLOAT_S8_UINT, &depth);
  for (const auto &f : targets)
    for (const std::string mode :
         {"opaque", "write_r", "blend_alpha", "blend_add", "cull_both",
          "base_vertex_instance", "depth_less", "depth_greater", "depth_write",
          "depth_no_write", "stencil_equal", "stencil_not_equal",
          "point_cull_both", "line_cull_both", "point_cull_depth_greater",
          "cull_both_side_effects", "rasterizer_discard_side_effects",
          "cull_restore"}) {
      std::string label = "graphics " + std::string(f.name) + " " + mode;
      VkFormatProperties2 props{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
      c.vkGetPhysicalDeviceFormatProperties2(c.physical, f.value, &props);
      auto required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
      if (mode.starts_with("blend_"))
        required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
      if (mode.ends_with("side_effects") &&
          (!c.features.vertexPipelineStoresAndAtomics ||
           !c.features.fragmentStoresAndAtomics)) {
        ++c.unsupported;
        c.log("UNSUPPORTED " + label +
              " reason=graphics storage writes unavailable");
        continue;
      }
      if ((props.formatProperties.optimalTilingFeatures & required) !=
              required ||
          !(depth.formatProperties.optimalTilingFeatures &
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        ++c.unsupported;
        c.log("UNSUPPORTED " + label +
              " reason=attachment capabilities unavailable");
        continue;
      }
      c.log("CASE " + label);
      try {
        if (Graphics(c, f, mode)) {
          ++c.passed;
          c.log("PASS " + label + " checked_components=1024");
        } else {
          ++c.failed;
          c.log("FAIL " + label);
        }
      } catch (const ApiError &error) {
        ++c.errors;
        c.log("ERROR " + label + " " + error.what());
        if (error.result == VK_ERROR_DEVICE_LOST || error.result == VK_TIMEOUT)
          throw;
      } catch (const std::exception &error) {
        ++c.errors;
        c.log("ERROR " + label + " " + error.what());
      }
    }
}

void RunSimultaneous(Context &c) {
  if (!c.automatic_raw)
    return;
  unsigned passed = 0, failed = 0;
  for (const auto &native : NativeRaw::Formats) {
    const Format *format = nullptr;
    for (const auto &candidate : Formats)
      if (!std::strcmp(candidate.name, native.name))
        format = &candidate;
    for (const std::string mode :
         {"simultaneous_storage_attachment", "simultaneous_sampler_attachment",
          "simultaneous_storage_sampler_attachment",
          "simultaneous_separate_storage_sampler_attachment",
          "simultaneous_immutable_storage_sampler_attachment",
          "simultaneous_sampler_readonly_attachment"}) {
      c.log("SIMULTANEOUS_CASE " + std::string(native.name) + " " + mode);
      bool ok = Graphics(c, *format, mode);
      if (ok) {
        ++passed;
        ++c.passed;
      } else {
        ++failed;
        ++c.failed;
      }
      c.log(std::string(ok ? "SIMULTANEOUS_PASS " : "SIMULTANEOUS_FAIL ") +
            native.name + " " + mode);
    }
  }
  c.log("SIMULTANEOUS_END passed=" + std::to_string(passed) +
        " failed=" + std::to_string(failed));
}
