/* Copyright (c) 2016-2017, ARM Limited and Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "framework/application.hpp"
#include "framework/assets.hpp"
#include "framework/common.hpp"
#include "framework/context.hpp"
#include "framework/math.hpp"
#include "framework/thread_pool.hpp"
#include "platform/os.hpp"
#include "platform/platform.hpp"
#include <string.h>

using namespace MaliSDK;
using namespace std;
using namespace glm;

#define NUM_INSTANCES_X 32
#define NUM_INSTANCES_Y 32
#define NUM_INSTANCES (NUM_INSTANCES_X * NUM_INSTANCES_Y)
#define MAX_INSTANCES_PER_DRAW_CALL 16

// This sample expands rotating_texture with drawing many rotating quads.

struct Backbuffer
{
	// We get this image from the platform. Its memory is bound to the display or window.
	VkImage image;

	// We need an image view to be able to access the image as a framebuffer.
	VkImageView view;

	// The actual framebuffer.
	VkFramebuffer framebuffer;
};

struct Buffer
{
	// The buffer object.
	VkBuffer buffer;

	// Buffer objects are backed by device memory.
	VkDeviceMemory memory;
};

struct Texture
{
	// The image object itself.
	VkImage image;

	// To use images in shaders, we need an image view.
	VkImageView view;

	// Memory for the texture.
	VkDeviceMemory memory;

	// Images have layouts, stores the current layout used.
	VkImageLayout layout;

	// For simplicity, tie a sampler object to the texture.
	VkSampler sampler;

	unsigned width, height;
};

struct PerFrame
{
	Buffer uniformBuffer;
	VkDescriptorSet descriptorSet;
	VkDescriptorPool descriptorPool;
};

struct Vertex
{
	vec2 position;
	vec2 color;
};

class MultiThreading : public VulkanApplication
{
public:
	virtual bool initialize(Context *pContext);
	virtual void render(unsigned swapchainIndex, float deltaTime);
	virtual void terminate();
	virtual void updateSwapchain(const vector<VkImage> &backbuffers, const Platform::SwapchainDimensions &dim);

private:
	Context *pContext = nullptr;

	vector<Backbuffer> backbuffers;
	vector<PerFrame> perFrame;
	unsigned width, height;

	// The renderpass description.
	VkRenderPass renderPass;

	// The graphics pipeline.
	VkPipeline pipeline;

	// Pipeline objects can be cached in a pipeline cache.
	// Mostly useful when you have many pipeline objects.
	VkPipelineCache pipelineCache;

	// Specified the pipeline layout for resources.
	// We don't use any in this sample, but we still need to provide a dummy one.
	VkPipelineLayout pipelineLayout;

	VkDescriptorSetLayout setLayout;

	Buffer vertexBuffer;
	Buffer instanceBuffer;
	Texture texture;

	Buffer createBuffer(const void *pInitial, size_t size, VkFlags usage);
	Texture createTextureFromAsset(const char *pPath);

	uint32_t findMemoryTypeFromRequirements(uint32_t deviceRequirements, uint32_t hostRequirements);
	uint32_t findMemoryTypeFromRequirementsWithFallback(uint32_t deviceRequirements, uint32_t hostRequirements);

	void initRenderPass(VkFormat format);
	void termBackbuffers();

	void initPerFrame(unsigned numBackbuffers);
	void termPerFrame();

	void initVertexBuffers();
	void initPipeline();
	void initPipelineLayout();

	void imageMemoryBarrier(VkCommandBuffer cmd, VkImage image, VkAccessFlags srcAccessMask,
	                        VkAccessFlags dstAccessMask, VkPipelineStageFlags srcStageMask,
	                        VkPipelineStageFlags dstStageMask, VkImageLayout oldLayout, VkImageLayout newLayout);

	float accumulatedTime = 0.0f;

	ThreadPool threadPool;

	void renderScene(VkCommandBuffer cmd, unsigned beginInstance, unsigned endInstance, VkDescriptorSet set);
};

// To create a buffer, both the device and application have requirements from the buffer object.
// Vulkan exposes the different types of buffers the device can allocate, and we have to find a suitable one.
// deviceRequirements is a bitmask expressing which memory types can be used for a buffer object.
// The different memory types' properties must match with what the application wants.
uint32_t MultiThreading::findMemoryTypeFromRequirements(uint32_t deviceRequirements, uint32_t hostRequirements)
{
	const VkPhysicalDeviceMemoryProperties &props = pContext->getPlatform().getMemoryProperties();
	for (uint32_t i = 0; i < VK_MAX_MEMORY_TYPES; i++)
	{
		if (deviceRequirements & (1u << i))
		{
			if ((props.memoryTypes[i].propertyFlags & hostRequirements) == hostRequirements)
			{
				return i;
			}
		}
	}

	LOGE("Failed to obtain suitable memory type.\n");
	abort();
}

uint32_t MultiThreading::findMemoryTypeFromRequirementsWithFallback(uint32_t deviceRequirements,
                                                                    uint32_t hostRequirements)
{
	const VkPhysicalDeviceMemoryProperties &props = pContext->getPlatform().getMemoryProperties();
	for (uint32_t i = 0; i < VK_MAX_MEMORY_TYPES; i++)
	{
		if (deviceRequirements & (1u << i))
		{
			if ((props.memoryTypes[i].propertyFlags & hostRequirements) == hostRequirements)
			{
				return i;
			}
		}
	}

	// If we cannot find the particular memory type we're looking for, just pick the first one available.
	if (hostRequirements != 0)
		return findMemoryTypeFromRequirements(deviceRequirements, 0);
	else
	{
		LOGE("Failed to obtain suitable memory type.\n");
		abort();
	}
}

Texture MultiThreading::createTextureFromAsset(const char *pPath)
{
	// We want to first create a staging buffer.
	// We will then copy this buffer into an optimally tiled texture with vkCmdCopyBufferToImage.
	// The layout of such a texture is not specified as it is highly GPU-dependent and optimized for
	// utilizing texture caches better.
	unsigned width, height;
	vector<uint8_t> buffer;

	if (FAILED(loadRgba8888TextureFromAsset(pPath, &buffer, &width, &height)))
	{
		LOGE("Failed to load texture from asset.\n");
		abort();
	}

	VkDevice device = pContext->getDevice();
	VkImage image;
	VkDeviceMemory memory;

	Buffer stagingBuffer = createBuffer(buffer.data(), width * height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);

	// We will transition the actual texture into a proper layout before transfering any data, so leave it as undefined.
	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = VK_FORMAT_R8G8B8A8_UNORM;
	info.extent.width = width;
	info.extent.height = height;
	info.extent.depth = 1;
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	// Create texture.
	VK_CHECK(vkCreateImage(device, &info, nullptr, &image));

	// Allocate memory for the texture.
	VkMemoryRequirements memReqs;
	vkGetImageMemoryRequirements(device, image, &memReqs);

	VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	alloc.allocationSize = memReqs.size;
	alloc.memoryTypeIndex =
	    findMemoryTypeFromRequirementsWithFallback(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VK_CHECK(vkAllocateMemory(device, &alloc, nullptr, &memory));
	vkBindImageMemory(device, image, memory, 0);

	// Create an image view for the new texture.
	// Note that CreateImageView must happen after BindImageMemory.
	VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	viewInfo.image = image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
	viewInfo.components.r = VK_COMPONENT_SWIZZLE_R;
	viewInfo.components.g = VK_COMPONENT_SWIZZLE_G;
	viewInfo.components.b = VK_COMPONENT_SWIZZLE_B;
	viewInfo.components.a = VK_COMPONENT_SWIZZLE_A;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;

	VkImageView view;
	VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &view));

	// Now we need to transfer the staging texture into the real texture.
	// For this we will need a command buffer.
	VkCommandBuffer cmd = pContext->requestPrimaryCommandBuffer();

	// We will only submit this once before it's recycled.
	VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &beginInfo);

	imageMemoryBarrier(cmd, image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	VkBufferImageCopy region;
	memset(&region, 0, sizeof(region));
	region.bufferOffset = 0;
	region.bufferRowLength = width;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent.width = width;
	region.imageExtent.height = height;
	region.imageExtent.depth = 1;

	// Copy the buffer to our optimally tiled image.
	vkCmdCopyBufferToImage(cmd, stagingBuffer.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	imageMemoryBarrier(cmd, image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	VK_CHECK(vkEndCommandBuffer(cmd));
	pContext->submit(cmd);

	// We want to free the staging buffer and memory right away, so wait for GPU complete the transfer.
	vkQueueWaitIdle(pContext->getGraphicsQueue());

	// Now it's safe to free the temporary resources.
	vkDestroyBuffer(device, stagingBuffer.buffer, nullptr);
	vkFreeMemory(device, stagingBuffer.memory, nullptr);

	// Finally, create a sampler.
	VkSamplerCreateInfo samplerInfo = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.mipLodBias = 0.0f;
	samplerInfo.maxAnisotropy = 1.0f;
	samplerInfo.compareEnable = false;
	samplerInfo.minLod = 0.0f;
	samplerInfo.maxLod = 0.0f;

	VkSampler sampler;
	VK_CHECK(vkCreateSampler(device, &samplerInfo, nullptr, &sampler));

	Texture ret = {
		image, view, memory, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, sampler, width, height,
	};
	return ret;
}

void MultiThreading::imageMemoryBarrier(VkCommandBuffer cmd, VkImage image, VkAccessFlags srcAccessMask,
                                        VkAccessFlags dstAccessMask, VkPipelineStageFlags srcStageMask,
                                        VkPipelineStageFlags dstStageMask, VkImageLayout oldLayout,
                                        VkImageLayout newLayout)
{
	VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };

	barrier.srcAccessMask = srcAccessMask;
	barrier.dstAccessMask = dstAccessMask;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;

	vkCmdPipelineBarrier(cmd, srcStageMask, dstStageMask, false, 0, nullptr, 0, nullptr, 1, &barrier);
}

Buffer MultiThreading::createBuffer(const void *pInitialData, size_t size, VkFlags usage)
{
	Buffer buffer;
	VkDevice device = pContext->getDevice();

	VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	info.usage = usage;
	info.size = size;

	VK_CHECK(vkCreateBuffer(device, &info, nullptr, &buffer.buffer));

	// Ask device about its memory requirements.
	VkMemoryRequirements memReqs;
	vkGetBufferMemoryRequirements(device, buffer.buffer, &memReqs);

	VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	alloc.allocationSize = memReqs.size;

	// We want host visible and coherent memory to simplify things.
	alloc.memoryTypeIndex = findMemoryTypeFromRequirements(
	    memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

	VK_CHECK(vkAllocateMemory(device, &alloc, nullptr, &buffer.memory));

	// Buffers are not backed by memory, so bind our memory explicitly to the buffer.
	vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0);

	// Map the memory and dump data in there.
	if (pInitialData)
	{
		void *pData;
		VK_CHECK(vkMapMemory(device, buffer.memory, 0, size, 0, &pData));
		memcpy(pData, pInitialData, size);
		vkUnmapMemory(device, buffer.memory);
	}

	return buffer;
}

void MultiThreading::initRenderPass(VkFormat format)
{
	VkAttachmentDescription attachment = { 0 };
	// Backbuffer format.
	attachment.format = format;
	// Not multisampled.
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	// When starting the frame, we want tiles to be cleared.
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	// When ending the frame, we want tiles to be written out.
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	// Don't care about stencil since we're not using it.
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;

	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	// We have one subpass.
	// This subpass has 1 color attachment.
	VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription subpass = { 0 };
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorRef;

	// Create a dependency to external events.
	// We need to wait for the WSI semaphore to signal.
	// Only pipeline stages which depend on COLOR_ATTACHMENT_OUTPUT_BIT will
	// actually wait for the semaphore, so we must also wait for that pipeline stage.
	VkSubpassDependency dependency = { 0 };
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	// Since we changed the image layout, we need to make the memory visible to
	// color attachment to modify.
	dependency.srcAccessMask = 0;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	// Finally, create the renderpass.
	VkRenderPassCreateInfo rpInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	rpInfo.attachmentCount = 1;
	rpInfo.pAttachments = &attachment;
	rpInfo.subpassCount = 1;
	rpInfo.pSubpasses = &subpass;
	rpInfo.dependencyCount = 1;
	rpInfo.pDependencies = &dependency;
	VK_CHECK(vkCreateRenderPass(pContext->getDevice(), &rpInfo, nullptr, &renderPass));
}

void MultiThreading::initVertexBuffers()
{
	static const Vertex data[] = {
		{
		    vec2(-0.5f, +0.5f), vec2(0.0f, 0.0f),
		},
		{
		    vec2(-0.5f, -0.5f), vec2(0.0f, 1.0f),
		},
		{
		    vec2(+0.5f, +0.5f), vec2(1.0f, 0.0f),
		},
		{
		    vec2(+0.5f, -0.5f), vec2(1.0f, 1.0f),
		},
	};

	vertexBuffer = createBuffer(data, sizeof(data), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

	// Set up an instance buffer for our triangles.
	vector<vec4> instanceData(NUM_INSTANCES_X * NUM_INSTANCES_Y);
	for (int y = 0; y < NUM_INSTANCES_Y; y++)
	{
		for (int x = 0; x < NUM_INSTANCES_X; x++)
		{
			float offsetX = float(x - NUM_INSTANCES_X / 2) / (NUM_INSTANCES_X / 2);
			float offsetY = float(y - NUM_INSTANCES_Y / 2) / (NUM_INSTANCES_Y / 2);

			float scaleX = 1.0f / NUM_INSTANCES_X;
			float scaleY = 1.0f / NUM_INSTANCES_Y;

			instanceData[y * NUM_INSTANCES_X + x] = vec4(offsetX, offsetY, scaleX, scaleY);
		}
	}

	instanceBuffer =
	    createBuffer(instanceData.data(), instanceData.size() * sizeof(vec4), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
}

void MultiThreading::initPipelineLayout()
{
	VkDevice device = pContext->getDevice();

	VkDescriptorSetLayoutBinding bindings[2] = { { 0 } };
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

	VkDescriptorSetLayoutCreateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	info.bindingCount = 2;
	info.pBindings = bindings;
	VK_CHECK(vkCreateDescriptorSetLayout(device, &info, nullptr, &setLayout));

	VkPipelineLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	layoutInfo.setLayoutCount = 1;
	layoutInfo.pSetLayouts = &setLayout;
	VK_CHECK(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout));
}

void MultiThreading::initPipeline()
{
	VkDevice device = pContext->getDevice();

	// Specify we will use triangle strip to draw geometry.
	VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
		VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
	};
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

	// Specify our two attributes, Position, Color and OffsetScale.
	VkVertexInputAttributeDescription attributes[3] = { { 0 } };
	attributes[0].location = 0; // Position in shader specifies layout(location = 0) to link with this attribute.
	attributes[0].binding = 0;
	attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
	attributes[0].offset = 0;
	attributes[1].location = 1; // TexCoord in shader specifies layout(location = 1) to link with this attribute.
	attributes[1].binding = 0;
	attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
	attributes[1].offset = 2 * sizeof(float);
	attributes[2].location = 2; // OffsetScale in shader specifies layout(location = 2) to link with this attribute.
	attributes[2].binding = 1; // This attribute is sources from the per-instance buffer.
	attributes[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
	attributes[2].offset = 0;

	// We have two vertex buffers, per-vertex data with stride 8 floats (vec4 + vec4) and
	// one instance buffer with one vec4 per instance.
	VkVertexInputBindingDescription bindings[2] = { { 0 } };
	bindings[0].binding = 0;
	bindings[0].stride = sizeof(Vertex);
	bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	bindings[1].binding = 1;
	bindings[1].stride = sizeof(vec4);
	bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

	VkPipelineVertexInputStateCreateInfo vertexInput = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vertexInput.vertexBindingDescriptionCount = 2;
	vertexInput.pVertexBindingDescriptions = bindings;
	vertexInput.vertexAttributeDescriptionCount = 3;
	vertexInput.pVertexAttributeDescriptions = attributes;

	// Specify rasterization state.
	VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_BACK_BIT;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.depthClampEnable = false;
	raster.rasterizerDiscardEnable = false;
	raster.depthBiasEnable = false;
	raster.lineWidth = 1.0f;

	// Our attachment will write to all color channels, but no blending is enabled.
	VkPipelineColorBlendAttachmentState blendAttachment = { 0 };
	blendAttachment.blendEnable = false;
	blendAttachment.colorWriteMask = 0xf;

	VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	blend.attachmentCount = 1;
	blend.pAttachments = &blendAttachment;

	// We will have one viewport and scissor box.
	VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;

	// Disable all depth testing.
	VkPipelineDepthStencilStateCreateInfo depthStencil = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	depthStencil.depthTestEnable = false;
	depthStencil.depthWriteEnable = false;
	depthStencil.depthBoundsTestEnable = false;
	depthStencil.stencilTestEnable = false;

	// No multisampling.
	VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	// Specify that these states will be dynamic, i.e. not part of pipeline state object.
	static const VkDynamicState dynamics[] = {
		VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
	};
	VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dynamic.pDynamicStates = dynamics;
	dynamic.dynamicStateCount = sizeof(dynamics) / sizeof(dynamics[0]);

	// Load our SPIR-V shaders.
	VkPipelineShaderStageCreateInfo shaderStages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
	};

	shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	shaderStages[0].module = loadShaderModule(device, "shaders/textured.vert.spv");
	shaderStages[0].pName = "main";
	shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	shaderStages[1].module = loadShaderModule(device, "shaders/textured.frag.spv");
	shaderStages[1].pName = "main";

	VkGraphicsPipelineCreateInfo pipe = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pipe.stageCount = 2;
	pipe.pStages = shaderStages;
	pipe.pVertexInputState = &vertexInput;
	pipe.pInputAssemblyState = &inputAssembly;
	pipe.pRasterizationState = &raster;
	pipe.pColorBlendState = &blend;
	pipe.pMultisampleState = &multisample;
	pipe.pViewportState = &viewport;
	pipe.pDepthStencilState = &depthStencil;
	pipe.pDynamicState = &dynamic;
	pipe.renderPass = renderPass;
	pipe.layout = pipelineLayout;

	VK_CHECK(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipe, nullptr, &pipeline));

	// Pipeline is baked, we can delete the shader modules now.
	vkDestroyShaderModule(device, shaderStages[0].module, nullptr);
	vkDestroyShaderModule(device, shaderStages[1].module, nullptr);
}

bool MultiThreading::initialize(Context *pContext)
{
	this->pContext = pContext;

	// Initialize thread pool for rendering later.
	threadPool.setWorkerThreadCount(OS::getNumberOfCpuThreads());
	pContext->setRenderingThreadCount(OS::getNumberOfCpuThreads());

	// Create the vertex buffer and instance buffer.
	initVertexBuffers();

	// Initialize the pipeline layout.
	initPipelineLayout();

	// Load texture.
	texture = createTextureFromAsset("textures/icon.png");

	// Create a pipeline cache (although we'll only create one pipeline).
	VkPipelineCacheCreateInfo pipelineCacheInfo = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
	VK_CHECK(vkCreatePipelineCache(pContext->getDevice(), &pipelineCacheInfo, nullptr, &pipelineCache));

	return true;
}

void MultiThreading::renderScene(VkCommandBuffer cmd, unsigned beginInstance, unsigned endInstance, VkDescriptorSet set)
{
	// Bind the graphics pipeline.
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

	// Set up dynamic state.
	// Viewport
	VkViewport vp = { 0 };
	vp.x = 0.0f;
	vp.y = 0.0f;
	vp.width = float(width);
	vp.height = float(height);
	vp.minDepth = 0.0f;
	vp.maxDepth = 1.0f;
	vkCmdSetViewport(cmd, 0, 1, &vp);

	// Scissor box
	VkRect2D scissor;
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent.width = width;
	scissor.extent.height = height;
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	// Bind vertex buffers.
	VkDeviceSize offsets[2] = { 0, 0 };
	VkBuffer buffers[2] = { vertexBuffer.buffer, instanceBuffer.buffer };
	vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);

	// Bind the descriptor set.
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &set, 0, nullptr);

	// Simulate a lot of draw calls.
	// NOTE: Normally, you could just instance the quads as is in a single draw call.
	// This is more to demonstrate how to use multithreading.
	for (unsigned baseInstance = beginInstance; baseInstance < endInstance;)
	{
		unsigned instancesToDraw = glm::min(endInstance - baseInstance, unsigned(MAX_INSTANCES_PER_DRAW_CALL));
		vkCmdDraw(cmd, 4, instancesToDraw, 0, baseInstance);
		baseInstance += instancesToDraw;
	}

	VK_CHECK(vkEndCommandBuffer(cmd));
}

void MultiThreading::render(unsigned swapchainIndex, float deltaTime)
{
	// Render to this backbuffer.
	Backbuffer &backbuffer = backbuffers[swapchainIndex];
	PerFrame &frame = perFrame[swapchainIndex];

	// Request a fresh command buffer.
	VkCommandBuffer cmd = pContext->requestPrimaryCommandBuffer();

	// We will only submit this once before it's recycled.
	VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &beginInfo);

	// Set clear color values.
	VkClearValue clearValue;
	clearValue.color.float32[0] = 0.1f;
	clearValue.color.float32[1] = 0.1f;
	clearValue.color.float32[2] = 0.2f;
	clearValue.color.float32[3] = 1.0f;

	// Update the uniform buffers memory.
	mat4 *pMatrix = nullptr;
	VK_CHECK(vkMapMemory(pContext->getDevice(), frame.uniformBuffer.memory, 0, sizeof(mat4), 0,
	                     reinterpret_cast<void **>(&pMatrix)));

	float aspect = float(width) / height;
	float textureAspect = float(texture.width) / texture.height;

	// Simple orthographic projection.
	mat4 proj = ortho(aspect * -1.0f, aspect * 1.0f, -1.0f, 1.0f, 0.0f, 1.0f);

	// Create a simple rotation matrix which rotates around the Z axis
	// and write it to the mapped memory.
	accumulatedTime += deltaTime;
	mat4 rotation = rotate(accumulatedTime, vec3(0.0f, 0.0f, 1.0f));

	// Scale the quad such that it matches the aspect ratio of our texture.
	mat4 model = scale(rotation, vec3(textureAspect, 1.0f, 1.0f));

	// Fix up the projection matrix so it matches what Vulkan expects.
	*pMatrix = vulkanStyleProjection(proj) * model;
	vkUnmapMemory(pContext->getDevice(), frame.uniformBuffer.memory);

	// Begin the render pass.
	VkRenderPassBeginInfo rpBegin = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	rpBegin.renderPass = renderPass;
	rpBegin.framebuffer = backbuffer.framebuffer;
	rpBegin.renderArea.extent.width = width;
	rpBegin.renderArea.extent.height = height;
	rpBegin.clearValueCount = 1;
	rpBegin.pClearValues = &clearValue;

	// We will use secondary command buffers only to submit commands in this subpass.
	vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);

	unsigned numThreads = threadPool.getWorkerThreadCount();
	vector<VkCommandBuffer> commandBuffers(numThreads);

	// Push rendering work to threads.
	for (unsigned i = 0; i < numThreads; i++)
	{
		VkCommandBuffer secondaryCmd = pContext->requestSecondaryCommandBuffer(i);
		commandBuffers[i] = secondaryCmd;

		// Use RENDER_PASS_CONTINUE_BIT since this secondary command buffer will be part of a render pass.
		// It is possible to use secondary command buffers for other things than just rendering.
		VkCommandBufferBeginInfo secondaryBeginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		VkCommandBufferInheritanceInfo inheritance = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO };
		secondaryBeginInfo.flags =
		    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
		secondaryBeginInfo.pInheritanceInfo = &inheritance;

		// The secondary buffer doesn't really know about the primary command buffer yet, so give it some essential knowledge.
		inheritance.renderPass = renderPass;
		inheritance.framebuffer = backbuffer.framebuffer;
		inheritance.subpass = 0;

		vkBeginCommandBuffer(secondaryCmd, &secondaryBeginInfo);

		unsigned beginInstance = (i * NUM_INSTANCES) / numThreads;
		unsigned endInstance = ((i + 1) * NUM_INSTANCES) / numThreads;
		VkDescriptorSet descriptorSet = frame.descriptorSet;

		// Push our work to our thread pool.
		threadPool.pushWorkToThread(i, [=] { renderScene(secondaryCmd, beginInstance, endInstance, descriptorSet); });
	}

	// Wait for thread pool to go idle.
	threadPool.waitIdle();

	// Submit the secondary command buffers to the primary command buffer.
	vkCmdExecuteCommands(cmd, commandBuffers.size(), commandBuffers.data());

	// Complete render pass.
	vkCmdEndRenderPass(cmd);

	// Complete the command buffer.
	VK_CHECK(vkEndCommandBuffer(cmd));

	// Submit it to the queue.
	pContext->submitSwapchain(cmd);
}

void MultiThreading::termBackbuffers()
{
	// Tear down backbuffers.
	// If our swapchain changes, we will call this, and create a new swapchain.
	VkDevice device = pContext->getDevice();

	if (!backbuffers.empty())
	{
		// Wait until device is idle before teardown.
		vkQueueWaitIdle(pContext->getGraphicsQueue());
		for (auto &backbuffer : backbuffers)
		{
			vkDestroyFramebuffer(device, backbuffer.framebuffer, nullptr);
			vkDestroyImageView(device, backbuffer.view, nullptr);
		}
		backbuffers.clear();
		vkDestroyRenderPass(device, renderPass, nullptr);
		vkDestroyPipeline(device, pipeline, nullptr);
	}
}

void MultiThreading::termPerFrame()
{
	VkDevice device = pContext->getDevice();

	for (auto &frame : perFrame)
	{
		vkDestroyBuffer(device, frame.uniformBuffer.buffer, nullptr);
		vkFreeMemory(device, frame.uniformBuffer.memory, nullptr);
		vkDestroyDescriptorPool(device, frame.descriptorPool, nullptr);
	}
	perFrame.clear();
}

void MultiThreading::terminate()
{
	vkDeviceWaitIdle(pContext->getDevice());

	// Teardown.
	VkDevice device = pContext->getDevice();

	// Vertex buffers
	vkDestroyBuffer(device, vertexBuffer.buffer, nullptr);
	vkFreeMemory(device, vertexBuffer.memory, nullptr);
	vkDestroyBuffer(device, instanceBuffer.buffer, nullptr);
	vkFreeMemory(device, instanceBuffer.memory, nullptr);

	// Texture
	vkDestroyImageView(device, texture.view, nullptr);
	vkDestroyImage(device, texture.image, nullptr);
	vkDestroySampler(device, texture.sampler, nullptr);
	vkFreeMemory(device, texture.memory, nullptr);

	// Per-frame resources
	termPerFrame();
	termBackbuffers();

	vkDestroyPipelineCache(device, pipelineCache, nullptr);
	vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
	vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
}

void MultiThreading::initPerFrame(unsigned numBackbuffers)
{
	VkDevice device = pContext->getDevice();

	for (unsigned i = 0; i < numBackbuffers; i++)
	{
		PerFrame frame;
		frame.uniformBuffer = createBuffer(nullptr, sizeof(mat4), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

		static const VkDescriptorPoolSize poolSizes[2] = {
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
		};

		VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		poolInfo.poolSizeCount = 2;
		poolInfo.pPoolSizes = poolSizes;
		poolInfo.maxSets = 1;
		VK_CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &frame.descriptorPool));

		VkDescriptorSetAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		allocInfo.descriptorPool = frame.descriptorPool;
		allocInfo.descriptorSetCount = 1;
		allocInfo.pSetLayouts = &setLayout;
		VK_CHECK(vkAllocateDescriptorSets(device, &allocInfo, &frame.descriptorSet));

		VkWriteDescriptorSet writes[2] = {
			{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
		};

		VkDescriptorBufferInfo bufferInfo = { frame.uniformBuffer.buffer, 0, sizeof(mat4) };
		VkDescriptorImageInfo imageInfo = { texture.sampler, texture.view, texture.layout };

		writes[0].dstSet = frame.descriptorSet;
		writes[0].dstBinding = 0;
		writes[0].descriptorCount = 1;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[0].pImageInfo = &imageInfo;

		writes[1].dstSet = frame.descriptorSet;
		writes[1].dstBinding = 1;
		writes[1].descriptorCount = 1;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		writes[1].pBufferInfo = &bufferInfo;

		vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

		perFrame.push_back(frame);
	}
}

void MultiThreading::updateSwapchain(const vector<VkImage> &newBackbuffers, const Platform::SwapchainDimensions &dim)
{
	VkDevice device = pContext->getDevice();
	width = dim.width;
	height = dim.height;

	// In case we're reinitializing the swapchain, terminate the old one first.
	termBackbuffers();
	termPerFrame();

	// We can't initialize the renderpass until we know the swapchain format.
	initRenderPass(dim.format);
	// We can't initialize the pipeline until we know the render pass.
	initPipeline();

	// Initialize per-frame resources.
	initPerFrame(newBackbuffers.size());

	// For all backbuffers in the swapchain ...
	for (auto image : newBackbuffers)
	{
		Backbuffer backbuffer;
		backbuffer.image = image;

		// Create an image view which we can render into.
		VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = dim.format;
		view.image = image;
		view.subresourceRange.baseMipLevel = 0;
		view.subresourceRange.baseArrayLayer = 0;
		view.subresourceRange.levelCount = 1;
		view.subresourceRange.layerCount = 1;
		view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		view.components.r = VK_COMPONENT_SWIZZLE_R;
		view.components.g = VK_COMPONENT_SWIZZLE_G;
		view.components.b = VK_COMPONENT_SWIZZLE_B;
		view.components.a = VK_COMPONENT_SWIZZLE_A;

		VK_CHECK(vkCreateImageView(device, &view, nullptr, &backbuffer.view));

		// Build the framebuffer.
		VkFramebufferCreateInfo fbInfo = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		fbInfo.renderPass = renderPass;
		fbInfo.attachmentCount = 1;
		fbInfo.pAttachments = &backbuffer.view;
		fbInfo.width = width;
		fbInfo.height = height;
		fbInfo.layers = 1;

		VK_CHECK(vkCreateFramebuffer(device, &fbInfo, nullptr, &backbuffer.framebuffer));

		backbuffers.push_back(backbuffer);
	}
}

VulkanApplication *MaliSDK::createApplication()
{
	return new MultiThreading();
}
