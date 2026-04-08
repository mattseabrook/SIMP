// vulkan.cpp

#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <span>
#include <chrono>
#include <algorithm>

#ifdef _WIN32
#include <Windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include "vulkan.h"
#include "game.h"
#include "config.h"
#include "window.h"
#include "raycast.h"
#include "render.h"
#include "megatexture.h"
#include "system.h"
#include "pvx.h"
#include "../resource.h"

// GPU raycast compute shader (kept local to Engine/include/shaders)
#include "shaders/vk_raycast_spv.h"

//
// Vulkan context
//
VulkanContext vkCtx;

static unsigned char* menuTitleImage = nullptr;
static int menuTitleW = 0, menuTitleH = 0, menuTitleC = 0;

static void ensureMenuTitleImageLoaded()
{
	if (menuTitleImage)
		return;

	menuTitleImage = RenderTextImageFromTTFResource(
		IDR_MENU_TITLE_TTF,
		"SUMMERCAMP",
		420,
		255,
		0,
		0,
		&menuTitleW,
		&menuTitleH,
		&menuTitleC);
	if (menuTitleImage)
	{
		char buf[256];
		snprintf(buf, sizeof(buf), "[Vulkan] Built menu title image lazily: %dx%d %d channels\n", menuTitleW, menuTitleH, menuTitleC);
		OutputDebugStringA(buf);
	}
	else
	{
		OutputDebugStringA("[Vulkan] Failed to build menu title image from TTF\n");
	}
}

/*
===============================================================================
Function Name: findMemoryType

Description:
	- Finds a suitable memory type for the Vulkan texture.
	- This is called during texture creation.

Parameters:
	- typeFilter: Bitmask of memory types to consider.
	- properties: Desired memory properties.

Returns:
	- The index of the suitable memory type.
===============================================================================
*/
static uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
	VkPhysicalDeviceMemoryProperties memProps;
	vkGetPhysicalDeviceMemoryProperties(vkCtx.physicalDevice, &memProps);
	for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
	{
		if ((typeFilter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
			return i;
	}
	throw std::runtime_error("Failed memory type");
}

static bool createBuffer(VkDeviceSize size,
	VkBufferUsageFlags usage,
	VkMemoryPropertyFlags properties,
	VkBuffer &buffer,
	VkDeviceMemory &bufferMemory)
{
	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	if (vkCreateBuffer(vkCtx.device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS)
		return false;

	VkMemoryRequirements memRequirements{};
	vkGetBufferMemoryRequirements(vkCtx.device, buffer, &memRequirements);

	VkMemoryAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

	if (vkAllocateMemory(vkCtx.device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS)
		return false;

	vkBindBufferMemory(vkCtx.device, buffer, bufferMemory, 0);
	return true;
}

static uint64_t fnv1a64Update(uint64_t hash, const uint8_t* data, size_t len)
{
	constexpr uint64_t prime = 1099511628211ull;
	for (size_t i = 0; i < len; ++i)
	{
		hash ^= static_cast<uint64_t>(data[i]);
		hash *= prime;
	}
	return hash;
}

static uint64_t hashTileMap(const TileMap &tileMap)
{
	uint64_t hash = 1469598103934665603ull;
	for (const auto &row : tileMap)
	{
		if (!row.empty())
			hash = fnv1a64Update(hash, row.data(), row.size());
		uint32_t rowSize = static_cast<uint32_t>(row.size());
		hash = fnv1a64Update(hash, reinterpret_cast<const uint8_t*>(&rowSize), sizeof(rowSize));
	}
	return hash;
}

static uint64_t hashExploredMap(const std::vector<uint8_t>& explored, size_t expectedSize)
{
	uint64_t hash = 1469598103934665603ull;
	const uint32_t exploredSize = static_cast<uint32_t>(explored.size());
	hash = fnv1a64Update(hash, reinterpret_cast<const uint8_t*>(&exploredSize), sizeof(exploredSize));
	const uint32_t expected = static_cast<uint32_t>(expectedSize);
	hash = fnv1a64Update(hash, reinterpret_cast<const uint8_t*>(&expected), sizeof(expected));
	if (explored.size() == expectedSize && !explored.empty())
		hash = fnv1a64Update(hash, explored.data(), explored.size());
	return hash;
}

static uint64_t hashMegatexEdges()
{
	uint64_t hash = 1469598103934665603ull;
	for (const auto &e : megatex.edges)
	{
		const int32_t values[6] = {
			e.cellX,
			e.cellY,
			e.side,
			e.xOffsetPixels,
			e.pixelWidth,
			e.direction
		};
		hash = fnv1a64Update(hash, reinterpret_cast<const uint8_t*>(values), sizeof(values));
	}
	uint32_t count = static_cast<uint32_t>(megatex.edges.size());
	hash = fnv1a64Update(hash, reinterpret_cast<const uint8_t*>(&count), sizeof(count));
	return hash;
}

static uint32_t getRaycastSupersampleSetting()
{
	if (config.contains("raycastSupersample"))
		return static_cast<uint32_t>(std::clamp(config["raycastSupersample"].get<int>(), 1, 16));
	if (config.contains("raycastMultisample"))
		return static_cast<uint32_t>(std::clamp(config["raycastMultisample"].get<int>(), 1, 16));
	return 1u;
}

static void destroyRaycastResources()
{
	if (vkCtx.tileMapBuffer)
	{
		vkDestroyBuffer(vkCtx.device, vkCtx.tileMapBuffer, nullptr);
		vkCtx.tileMapBuffer = VK_NULL_HANDLE;
	}
	if (vkCtx.tileMapBufferMemory)
	{
		if (vkCtx.mappedTileMapData)
		{
			vkUnmapMemory(vkCtx.device, vkCtx.tileMapBufferMemory);
			vkCtx.mappedTileMapData = nullptr;
		}
		vkFreeMemory(vkCtx.device, vkCtx.tileMapBufferMemory, nullptr);
		vkCtx.tileMapBufferMemory = VK_NULL_HANDLE;
	}
	if (vkCtx.edgeOffsetsBuffer)
	{
		vkDestroyBuffer(vkCtx.device, vkCtx.edgeOffsetsBuffer, nullptr);
		vkCtx.edgeOffsetsBuffer = VK_NULL_HANDLE;
	}
	if (vkCtx.edgeOffsetsBufferMemory)
	{
		if (vkCtx.mappedEdgeOffsetsData)
		{
			vkUnmapMemory(vkCtx.device, vkCtx.edgeOffsetsBufferMemory);
			vkCtx.mappedEdgeOffsetsData = nullptr;
		}
		vkFreeMemory(vkCtx.device, vkCtx.edgeOffsetsBufferMemory, nullptr);
		vkCtx.edgeOffsetsBufferMemory = VK_NULL_HANDLE;
	}
	if (vkCtx.megatexBuffer)
	{
		vkDestroyBuffer(vkCtx.device, vkCtx.megatexBuffer, nullptr);
		vkCtx.megatexBuffer = VK_NULL_HANDLE;
	}
	if (vkCtx.megatexBufferMemory)
	{
		if (vkCtx.mappedMegatexData)
		{
			vkUnmapMemory(vkCtx.device, vkCtx.megatexBufferMemory);
			vkCtx.mappedMegatexData = nullptr;
		}
		vkFreeMemory(vkCtx.device, vkCtx.megatexBufferMemory, nullptr);
		vkCtx.megatexBufferMemory = VK_NULL_HANDLE;
	}
	if (vkCtx.raycastDescPool)
	{
		vkDestroyDescriptorPool(vkCtx.device, vkCtx.raycastDescPool, nullptr);
		vkCtx.raycastDescPool = VK_NULL_HANDLE;
	}
	if (vkCtx.raycastPipeline)
	{
		vkDestroyPipeline(vkCtx.device, vkCtx.raycastPipeline, nullptr);
		vkCtx.raycastPipeline = VK_NULL_HANDLE;
	}
	if (vkCtx.raycastPipelineLayout)
	{
		vkDestroyPipelineLayout(vkCtx.device, vkCtx.raycastPipelineLayout, nullptr);
		vkCtx.raycastPipelineLayout = VK_NULL_HANDLE;
	}
	if (vkCtx.raycastDescSetLayout)
	{
		vkDestroyDescriptorSetLayout(vkCtx.device, vkCtx.raycastDescSetLayout, nullptr);
		vkCtx.raycastDescSetLayout = VK_NULL_HANDLE;
	}
	if (vkCtx.raycastImageView)
	{
		vkDestroyImageView(vkCtx.device, vkCtx.raycastImageView, nullptr);
		vkCtx.raycastImageView = VK_NULL_HANDLE;
	}
	if (vkCtx.raycastImage)
	{
		vkDestroyImage(vkCtx.device, vkCtx.raycastImage, nullptr);
		vkCtx.raycastImage = VK_NULL_HANDLE;
	}
	if (vkCtx.raycastImageMemory)
	{
		vkFreeMemory(vkCtx.device, vkCtx.raycastImageMemory, nullptr);
		vkCtx.raycastImageMemory = VK_NULL_HANDLE;
	}
	vkCtx.raycastImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	vkCtx.tileMapBufferSize = 0;
	vkCtx.edgeOffsetsBufferSize = 0;
	vkCtx.megatexBufferSize = 0;
	vkCtx.lastMapWidth = 0;
	vkCtx.lastMapHeight = 0;
	vkCtx.lastTileMapHash = 0;
	vkCtx.hasTileMapHash = false;
	vkCtx.lastExploredHash = 0;
	vkCtx.hasExploredHash = false;
	vkCtx.lastEdgeHash = 0;
	vkCtx.hasEdgeHash = false;
	vkCtx.edgeOffsetsDirty = true;
	vkCtx.lastEdgeCount = 0;
	vkCtx.tileMapUploadCache.clear();
	vkCtx.edgeOffsetsUploadCache.clear();
	vkCtx.lastMegatexVersion = 0;
	vkCtx.hasMegatexVersion = false;
	vkCtx.megatexUploadCache.clear();
}

static bool ensureRaycastImage(uint32_t width, uint32_t height)
{
	if (vkCtx.raycastImage && vkCtx.textureWidth == width && vkCtx.textureHeight == height)
		return true;

	// Recreate raycast resources that depend on image size
	if (vkCtx.raycastImageView)
	{
		vkDestroyImageView(vkCtx.device, vkCtx.raycastImageView, nullptr);
		vkCtx.raycastImageView = VK_NULL_HANDLE;
	}
	if (vkCtx.raycastImage)
	{
		vkDestroyImage(vkCtx.device, vkCtx.raycastImage, nullptr);
		vkCtx.raycastImage = VK_NULL_HANDLE;
	}
	if (vkCtx.raycastImageMemory)
	{
		vkFreeMemory(vkCtx.device, vkCtx.raycastImageMemory, nullptr);
		vkCtx.raycastImageMemory = VK_NULL_HANDLE;
	}
	
	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
	imageInfo.extent = {width, height, 1};
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	if (vkCreateImage(vkCtx.device, &imageInfo, nullptr, &vkCtx.raycastImage) != VK_SUCCESS)
		return false;

	VkMemoryRequirements memReq{};
	vkGetImageMemoryRequirements(vkCtx.device, vkCtx.raycastImage, &memReq);

	VkMemoryAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memReq.size;
	allocInfo.memoryTypeIndex = findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (vkAllocateMemory(vkCtx.device, &allocInfo, nullptr, &vkCtx.raycastImageMemory) != VK_SUCCESS)
		return false;

	vkBindImageMemory(vkCtx.device, vkCtx.raycastImage, vkCtx.raycastImageMemory, 0);

	VkImageViewCreateInfo viewInfo{};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = vkCtx.raycastImage;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;

	if (vkCreateImageView(vkCtx.device, &viewInfo, nullptr, &vkCtx.raycastImageView) != VK_SUCCESS)
		return false;

	vkCtx.raycastImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	return true;
}

static bool ensureTileMapBuffer(const TileMap &tileMap)
{
	if (tileMap.empty() || tileMap[0].empty())
		return false;

	uint32_t mapHeight = static_cast<uint32_t>(tileMap.size());
	uint32_t mapWidth = static_cast<uint32_t>(tileMap[0].size());
	const size_t tileCount = static_cast<size_t>(mapWidth) * mapHeight;
	const uint64_t mapHash = hashTileMap(tileMap);
	const uint64_t exploredHash = hashExploredMap(state.raycast.exploredMap, tileCount);

	if (vkCtx.tileMapBuffer &&
		vkCtx.lastMapWidth == mapWidth &&
		vkCtx.lastMapHeight == mapHeight &&
		vkCtx.hasTileMapHash &&
		vkCtx.lastTileMapHash == mapHash &&
		vkCtx.hasExploredHash &&
		vkCtx.lastExploredHash == exploredHash)
		return true;

	// Buffer holds tiles + explored flags (double the tile data)
	bool needRecreate = !vkCtx.tileMapBuffer || vkCtx.lastMapWidth != mapWidth || vkCtx.lastMapHeight != mapHeight;
	if (needRecreate)
	{
		// Destroy old buffer
		if (vkCtx.tileMapBuffer)
		{
			vkDestroyBuffer(vkCtx.device, vkCtx.tileMapBuffer, nullptr);
			if (vkCtx.mappedTileMapData)
			{
				vkUnmapMemory(vkCtx.device, vkCtx.tileMapBufferMemory);
				vkCtx.mappedTileMapData = nullptr;
			}
			vkFreeMemory(vkCtx.device, vkCtx.tileMapBufferMemory, nullptr);
			vkCtx.tileMapBuffer = VK_NULL_HANDLE;
			vkCtx.tileMapBufferMemory = VK_NULL_HANDLE;
		}
	}

	vkCtx.edgeOffsetsDirty = true;
	vkCtx.tileMapUploadCache.resize(tileCount * 2ull);
	for (uint32_t y = 0; y < mapHeight; ++y)
		std::memcpy(vkCtx.tileMapUploadCache.data() + static_cast<size_t>(y) * mapWidth,
			tileMap[y].data(),
			mapWidth);

	if (state.raycast.exploredMap.size() == tileCount)
		std::memcpy(vkCtx.tileMapUploadCache.data() + tileCount,
			state.raycast.exploredMap.data(),
			tileCount);
	else
		std::memset(vkCtx.tileMapUploadCache.data() + tileCount, 0, tileCount);

	VkDeviceSize bufferSize = static_cast<VkDeviceSize>((vkCtx.tileMapUploadCache.size() + 3ull) & ~3ull);
	if (needRecreate)
	{
		if (!createBuffer(bufferSize,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			vkCtx.tileMapBuffer,
			vkCtx.tileMapBufferMemory))
			return false;

		if (vkMapMemory(vkCtx.device, vkCtx.tileMapBufferMemory, 0, bufferSize, 0, &vkCtx.mappedTileMapData) != VK_SUCCESS)
			return false;
		vkCtx.tileMapBufferSize = bufferSize;
	}

	if (!vkCtx.mappedTileMapData)
		return false;

	std::memset(vkCtx.mappedTileMapData, 0, static_cast<size_t>(bufferSize));
	std::memcpy(vkCtx.mappedTileMapData,
		vkCtx.tileMapUploadCache.data(),
		vkCtx.tileMapUploadCache.size());

	vkCtx.lastMapWidth = mapWidth;
	vkCtx.lastMapHeight = mapHeight;
	vkCtx.lastTileMapHash = mapHash;
	vkCtx.hasTileMapHash = true;
	vkCtx.lastExploredHash = exploredHash;
	vkCtx.hasExploredHash = true;
	return true;
}

static bool ensureEdgeOffsetsBuffer(const TileMap &tileMap)
{
	if (tileMap.empty() || tileMap[0].empty())
		return false;

	uint32_t mapHeight = static_cast<uint32_t>(tileMap.size());
	uint32_t mapWidth = static_cast<uint32_t>(tileMap[0].size());
	const size_t count = static_cast<size_t>(mapWidth) * mapHeight * 4ull;
	VkDeviceSize bufferSize = static_cast<VkDeviceSize>(count * 3ull * sizeof(uint32_t));
	bool needRecreate = (!vkCtx.edgeOffsetsBuffer) || (vkCtx.edgeOffsetsBufferSize != bufferSize);
	const uint64_t edgeHash = hashMegatexEdges();
	if (vkCtx.lastEdgeCount != megatex.edges.size())
		vkCtx.edgeOffsetsDirty = true;
	if (vkCtx.lastMapWidth != mapWidth || vkCtx.lastMapHeight != mapHeight)
		vkCtx.edgeOffsetsDirty = true;

	const bool edgeUnchanged = (vkCtx.lastMapWidth == mapWidth &&
								vkCtx.lastMapHeight == mapHeight &&
								vkCtx.hasEdgeHash &&
								vkCtx.lastEdgeHash == edgeHash &&
								vkCtx.lastEdgeCount == megatex.edges.size());
	if (!needRecreate && edgeUnchanged)
	{
		vkCtx.edgeOffsetsDirty = false;
		vkCtx.lastEdgeCount = megatex.edges.size();
		return true;
	}

	vkCtx.edgeOffsetsUploadCache.assign(count * 3ull, 0u);
	for (const auto &e : megatex.edges)
	{
		if (e.cellX < 0 || e.cellY < 0)
			continue;
		if (e.cellX >= static_cast<int>(mapWidth) || e.cellY >= static_cast<int>(mapHeight))
			continue;
		size_t idx = (static_cast<size_t>(e.cellY) * mapWidth + static_cast<size_t>(e.cellX)) * 4ull + static_cast<size_t>(e.side & 3);
		size_t idx3 = idx * 3ull;
		if (idx3 + 2 < vkCtx.edgeOffsetsUploadCache.size())
		{
			vkCtx.edgeOffsetsUploadCache[idx3 + 0] = static_cast<uint32_t>(e.xOffsetPixels);
			vkCtx.edgeOffsetsUploadCache[idx3 + 1] = static_cast<uint32_t>(std::max(1, e.pixelWidth));
			vkCtx.edgeOffsetsUploadCache[idx3 + 2] = static_cast<uint32_t>(e.direction < 0 ? 1u : 0u);
		}
	}

	if (needRecreate)
	{
		if (vkCtx.edgeOffsetsBuffer)
		{
			vkDestroyBuffer(vkCtx.device, vkCtx.edgeOffsetsBuffer, nullptr);
			if (vkCtx.mappedEdgeOffsetsData)
			{
				vkUnmapMemory(vkCtx.device, vkCtx.edgeOffsetsBufferMemory);
				vkCtx.mappedEdgeOffsetsData = nullptr;
			}
			vkFreeMemory(vkCtx.device, vkCtx.edgeOffsetsBufferMemory, nullptr);
			vkCtx.edgeOffsetsBuffer = VK_NULL_HANDLE;
			vkCtx.edgeOffsetsBufferMemory = VK_NULL_HANDLE;
		}
		if (!createBuffer(bufferSize,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			vkCtx.edgeOffsetsBuffer,
			vkCtx.edgeOffsetsBufferMemory))
			return false;
		if (vkMapMemory(vkCtx.device, vkCtx.edgeOffsetsBufferMemory, 0, bufferSize, 0, &vkCtx.mappedEdgeOffsetsData) != VK_SUCCESS)
			return false;
		vkCtx.edgeOffsetsBufferSize = bufferSize;
	}

	if (!vkCtx.mappedEdgeOffsetsData)
		return false;

	std::memcpy(vkCtx.mappedEdgeOffsetsData,
		vkCtx.edgeOffsetsUploadCache.data(),
		static_cast<size_t>(bufferSize));
	vkCtx.lastEdgeHash = edgeHash;
	vkCtx.hasEdgeHash = true;
	vkCtx.edgeOffsetsDirty = false;
	vkCtx.lastEdgeCount = megatex.edges.size();

	return true;
}

static bool ensureMegatexBuffer()
{
	const uint32_t headerWords = 4u;
	uint32_t tileWidth = 0u;
	uint32_t tileHeight = 0u;
	uint32_t tileCount = 0u;
	size_t payloadBytes = 0;

	if (state.raycast.useMegatexture && megatex.loaded && megatex.tileWidth > 0 && megatex.tileHeight > 0)
	{
		tileWidth = static_cast<uint32_t>(megatex.tileWidth);
		tileHeight = static_cast<uint32_t>(megatex.tileHeight);
		tileCount = static_cast<uint32_t>(megatex.tileCache.size());
		payloadBytes = static_cast<size_t>(tileWidth) * static_cast<size_t>(tileHeight) * static_cast<size_t>(tileCount) * 4ull;
	}

	VkDeviceSize bufferSize = static_cast<VkDeviceSize>(headerWords * sizeof(uint32_t) + payloadBytes);
	bool needRecreate = (!vkCtx.megatexBuffer) || (vkCtx.megatexBufferSize != bufferSize);
	const bool unchanged = (!needRecreate &&
	                        vkCtx.hasMegatexVersion &&
	                        vkCtx.lastMegatexVersion == megatex.version);
	if (unchanged)
		return true;

	vkCtx.megatexUploadCache.assign(static_cast<size_t>((bufferSize + sizeof(uint32_t) - 1) / sizeof(uint32_t)), 0u);
	vkCtx.megatexUploadCache[0] = tileWidth;
	vkCtx.megatexUploadCache[1] = tileHeight;
	vkCtx.megatexUploadCache[2] = tileCount;
	vkCtx.megatexUploadCache[3] = 0u;

	if (payloadBytes > 0)
	{
		uint8_t* dst = reinterpret_cast<uint8_t*>(vkCtx.megatexUploadCache.data() + headerWords);
		size_t copied = 0;
		for (const auto& tile : megatex.tileCache)
		{
			if (copied + tile.size() > payloadBytes)
				break;
			std::memcpy(dst + copied, tile.data(), tile.size());
			copied += tile.size();
		}
	}

	if (needRecreate)
	{
		if (vkCtx.megatexBuffer)
		{
			vkDestroyBuffer(vkCtx.device, vkCtx.megatexBuffer, nullptr);
			if (vkCtx.mappedMegatexData)
			{
				vkUnmapMemory(vkCtx.device, vkCtx.megatexBufferMemory);
				vkCtx.mappedMegatexData = nullptr;
			}
			vkFreeMemory(vkCtx.device, vkCtx.megatexBufferMemory, nullptr);
			vkCtx.megatexBuffer = VK_NULL_HANDLE;
			vkCtx.megatexBufferMemory = VK_NULL_HANDLE;
		}
		if (!createBuffer(bufferSize,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			vkCtx.megatexBuffer,
			vkCtx.megatexBufferMemory))
			return false;
		if (vkMapMemory(vkCtx.device, vkCtx.megatexBufferMemory, 0, bufferSize, 0, &vkCtx.mappedMegatexData) != VK_SUCCESS)
			return false;
		vkCtx.megatexBufferSize = bufferSize;
	}

	if (!vkCtx.mappedMegatexData)
		return false;

	std::memcpy(vkCtx.mappedMegatexData,
		vkCtx.megatexUploadCache.data(),
		static_cast<size_t>(bufferSize));
	vkCtx.lastMegatexVersion = megatex.version;
	vkCtx.hasMegatexVersion = true;
	return true;
}

static void updateRaycastDescriptors()
{
	if (!vkCtx.raycastDescSet || !vkCtx.tileMapBuffer || !vkCtx.raycastImageView || !vkCtx.megatexBuffer)
		return;

	VkDescriptorBufferInfo tileBuf{};
	tileBuf.buffer = vkCtx.tileMapBuffer;
	tileBuf.offset = 0;
	tileBuf.range = vkCtx.tileMapBufferSize;

	VkDescriptorImageInfo outImg{};
	outImg.imageView = vkCtx.raycastImageView;
	outImg.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

	VkDescriptorBufferInfo offsetsBuf{};
	offsetsBuf.buffer = vkCtx.edgeOffsetsBuffer;
	offsetsBuf.offset = 0;
	offsetsBuf.range = vkCtx.edgeOffsetsBufferSize;

	VkDescriptorBufferInfo megatexBuf{};
	megatexBuf.buffer = vkCtx.megatexBuffer;
	megatexBuf.offset = 0;
	megatexBuf.range = vkCtx.megatexBufferSize;

	VkWriteDescriptorSet writes[4]{};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = vkCtx.raycastDescSet;
	writes[0].dstBinding = 0;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[0].descriptorCount = 1;
	writes[0].pBufferInfo = &tileBuf;

	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].dstSet = vkCtx.raycastDescSet;
	writes[1].dstBinding = 1;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	writes[1].descriptorCount = 1;
	writes[1].pImageInfo = &outImg;

	writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[2].dstSet = vkCtx.raycastDescSet;
	writes[2].dstBinding = 2;
	writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[2].descriptorCount = 1;
	writes[2].pBufferInfo = &offsetsBuf;

	writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[3].dstSet = vkCtx.raycastDescSet;
	writes[3].dstBinding = 3;
	writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[3].descriptorCount = 1;
	writes[3].pBufferInfo = &megatexBuf;

	vkUpdateDescriptorSets(vkCtx.device, 4, writes, 0, nullptr);
}

static bool ensureRaycastPipeline()
{
	if (vkCtx.raycastPipeline)
		return true;

	VkDescriptorSetLayoutBinding bindings[4]{};
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[2].binding = 2;
	bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[2].descriptorCount = 1;
	bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[3].binding = 3;
	bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[3].descriptorCount = 1;
	bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	VkDescriptorSetLayoutCreateInfo dslInfo{};
	dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dslInfo.bindingCount = 4;
	dslInfo.pBindings = bindings;
	if (vkCreateDescriptorSetLayout(vkCtx.device, &dslInfo, nullptr, &vkCtx.raycastDescSetLayout) != VK_SUCCESS)
		return false;

	struct RaycastConstants
	{
		float playerX;
		float playerY;
		float playerAngle;
		float playerFOV;
		uint32_t screenWidth;
		uint32_t screenHeight;
		uint32_t mapWidth;
		uint32_t mapHeight;
		float visualScale;
		float torchRange;
		float falloffMul;
		float fovMul;
		uint32_t supersample;
		float wallHeightUnits;
		uint32_t useMegatexture;
		float mapOverlayZoom;
	};
	static_assert(sizeof(RaycastConstants) == 64, "RaycastConstants must match shader push constant size");

	VkPushConstantRange pcRange{};
	pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pcRange.offset = 0;
	pcRange.size = sizeof(RaycastConstants);

	VkPipelineLayoutCreateInfo plInfo{};
	plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	plInfo.setLayoutCount = 1;
	plInfo.pSetLayouts = &vkCtx.raycastDescSetLayout;
	plInfo.pushConstantRangeCount = 1;
	plInfo.pPushConstantRanges = &pcRange;
	if (vkCreatePipelineLayout(vkCtx.device, &plInfo, nullptr, &vkCtx.raycastPipelineLayout) != VK_SUCCESS)
		return false;

	VkShaderModuleCreateInfo smInfo{};
	smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	smInfo.codeSize = shaders_vk_raycast_spv_len;
	smInfo.pCode = reinterpret_cast<const uint32_t *>(shaders_vk_raycast_spv);
	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(vkCtx.device, &smInfo, nullptr, &module) != VK_SUCCESS)
		return false;

	VkPipelineShaderStageCreateInfo stageInfo{};
	stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stageInfo.module = module;
	stageInfo.pName = "main";

	VkComputePipelineCreateInfo cpInfo{};
	cpInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	cpInfo.stage = stageInfo;
	cpInfo.layout = vkCtx.raycastPipelineLayout;
	VkResult cpRes = vkCreateComputePipelines(vkCtx.device, VK_NULL_HANDLE, 1, &cpInfo, nullptr, &vkCtx.raycastPipeline);
	vkDestroyShaderModule(vkCtx.device, module, nullptr);
	if (cpRes != VK_SUCCESS)
		return false;

	VkDescriptorPoolSize poolSizes[2]{};
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	poolSizes[0].descriptorCount = 3;
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	poolSizes[1].descriptorCount = 1;

	VkDescriptorPoolCreateInfo dpInfo{};
	dpInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	dpInfo.maxSets = 1;
	dpInfo.poolSizeCount = 2;
	dpInfo.pPoolSizes = poolSizes;
	if (vkCreateDescriptorPool(vkCtx.device, &dpInfo, nullptr, &vkCtx.raycastDescPool) != VK_SUCCESS)
		return false;

	VkDescriptorSetAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = vkCtx.raycastDescPool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &vkCtx.raycastDescSetLayout;
	if (vkAllocateDescriptorSets(vkCtx.device, &allocInfo, &vkCtx.raycastDescSet) != VK_SUCCESS)
		return false;

	updateRaycastDescriptors();
	return true;
}

static void renderFrameRaycastVkCPU()
{
	const auto &tileMap = *state.raycast.map;
	const RaycastPlayer &ply = state.raycast.player;

	// Wait for previous frame's GPU blit to finish before writing to the mapped texture
	vkWaitForFences(vkCtx.device, 1, &vkCtx.inFlightFence, VK_TRUE, UINT64_MAX);

	uint8_t *dst = static_cast<uint8_t *>(vkCtx.mappedTextureData);
	size_t pitch = vkCtx.textureRowPitch;

	// Clear framebuffer to black before rendering
	std::memset(dst, 0, pitch * static_cast<size_t>(state.ui.height));

	renderRaycastView(tileMap, ply, dst, pitch, state.ui.width, state.ui.height);
	presentFrame();
}

static void renderFrameRaycastVkGPU()
{
	const auto &tileMap = *state.raycast.map;
	const RaycastPlayer &ply = state.raycast.player;

	if (!ensureRaycastPipeline())
	{
		renderFrameRaycastVkCPU();
		return;
	}
	if (!ensureRaycastImage(static_cast<uint32_t>(state.ui.width), static_cast<uint32_t>(state.ui.height)))
	{
		renderFrameRaycastVkCPU();
		return;
	}
	if (!ensureTileMapBuffer(tileMap) || !ensureEdgeOffsetsBuffer(tileMap) || !ensureMegatexBuffer())
	{
		renderFrameRaycastVkCPU();
		return;
	}
	updateRaycastDescriptors();

	struct RaycastConstants
	{
		float playerX;
		float playerY;
		float playerAngle;
		float playerFOV;
		uint32_t screenWidth;
		uint32_t screenHeight;
		uint32_t mapWidth;
		uint32_t mapHeight;
		float visualScale;
		float torchRange;
		float falloffMul;
		float fovMul;
		uint32_t supersample;
		float wallHeightUnits;
		uint32_t useMegatexture;
		float mapOverlayZoom;
	};
	RaycastConstants constants{};
	constants.playerX = ply.x;
	constants.playerY = ply.y;
	constants.playerAngle = ply.angle;
	constants.playerFOV = ply.fov;
	constants.screenWidth = static_cast<uint32_t>(state.ui.width);
	constants.screenHeight = static_cast<uint32_t>(state.ui.height);
	constants.mapWidth = static_cast<uint32_t>(tileMap[0].size());
	constants.mapHeight = static_cast<uint32_t>(tileMap.size());
	constants.visualScale = config.contains("raycastScale") ? config["raycastScale"].get<float>() : 3.0f;
	float baseTorchRange = 20.0f;
	constants.torchRange = baseTorchRange * constants.visualScale;
	constants.falloffMul = config.contains("raycastFalloffMul") ? config["raycastFalloffMul"].get<float>() : 0.85f;
	constants.fovMul = config.contains("raycastFovMul") ? config["raycastFovMul"].get<float>() : 1.0f;
	constants.supersample = getRaycastSupersampleSetting();
	constants.wallHeightUnits = 1.0f;
	constants.useMegatexture = state.raycast.useMegatexture ? 1u : 0u;
	constants.mapOverlayZoom = state.raycast.showMapOverlay ? state.raycast.mapOverlayZoom : 0.0f;

	// Present frame with compute shader execution (single-frame sync like Engine presentFrame())
	vkWaitForFences(vkCtx.device, 1, &vkCtx.inFlightFence, VK_TRUE, UINT64_MAX);

	uint32_t imgIdx = 0;
	VkResult acquireRes = vkAcquireNextImageKHR(vkCtx.device, vkCtx.swapchain, UINT64_MAX, vkCtx.imageAvailableSemaphore, VK_NULL_HANDLE, &imgIdx);
	if (acquireRes == VK_ERROR_OUT_OF_DATE_KHR)
	{
		recreateSwapchain(static_cast<uint32_t>(state.ui.width), static_cast<uint32_t>(state.ui.height));
		return;
	}
	if (acquireRes != VK_SUCCESS && acquireRes != VK_SUBOPTIMAL_KHR)
	{
		return;
	}
	vkResetFences(vkCtx.device, 1, &vkCtx.inFlightFence);

	VkCommandBuffer cmdBuf = vkCtx.frameCommandBuffer;
	if (cmdBuf == VK_NULL_HANDLE)
		return;
	vkResetCommandBuffer(cmdBuf, 0);

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmdBuf, &beginInfo);

	// Transition raycast image to GENERAL for compute writes
	if (vkCtx.raycastImageLayout != VK_IMAGE_LAYOUT_GENERAL)
	{
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = vkCtx.raycastImageLayout;
		barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = vkCtx.raycastImage;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		vkCmdPipelineBarrier(cmdBuf,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			0,
			0,
			nullptr,
			0,
			nullptr,
			1,
			&barrier);
		vkCtx.raycastImageLayout = VK_IMAGE_LAYOUT_GENERAL;
	}

	// Bind and dispatch
	vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, vkCtx.raycastPipeline);
	vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, vkCtx.raycastPipelineLayout, 0, 1, &vkCtx.raycastDescSet, 0, nullptr);
	vkCmdPushConstants(cmdBuf, vkCtx.raycastPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
	uint32_t dispatchX = (constants.screenWidth + 7u) / 8u;
	uint32_t dispatchY = (constants.screenHeight + 7u) / 8u;
	vkCmdDispatch(cmdBuf, dispatchX, dispatchY, 1);

	// Barrier to make raycast image available for transfer read
	{
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = vkCtx.raycastImage;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;
		barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		vkCmdPipelineBarrier(cmdBuf,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			0,
			0,
			nullptr,
			0,
			nullptr,
			1,
			&barrier);
		vkCtx.raycastImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	}

	// Transition swapchain image to TRANSFER_DST
	VkImageMemoryBarrier swapBarrier{};
	swapBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	swapBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	swapBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	swapBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	swapBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	swapBarrier.image = vkCtx.swapchainImages[imgIdx];
	swapBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	swapBarrier.subresourceRange.levelCount = 1;
	swapBarrier.subresourceRange.layerCount = 1;
	swapBarrier.srcAccessMask = 0;
	swapBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmdBuf,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0,
		0,
		nullptr,
		0,
		nullptr,
		1,
		&swapBarrier);

	VkImageBlit blit{};
	blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.srcSubresource.layerCount = 1;
	blit.srcOffsets[1] = {static_cast<int32_t>(constants.screenWidth), static_cast<int32_t>(constants.screenHeight), 1};
	blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.dstSubresource.layerCount = 1;
	blit.dstOffsets[1] = {static_cast<int32_t>(vkCtx.swapchainExtent.width), static_cast<int32_t>(vkCtx.swapchainExtent.height), 1};

	vkCmdBlitImage(cmdBuf,
		vkCtx.raycastImage,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		vkCtx.swapchainImages[imgIdx],
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1,
		&blit,
		VK_FILTER_LINEAR);

	// Transition swapchain to PRESENT
	swapBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	swapBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	swapBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	swapBarrier.dstAccessMask = 0;
	vkCmdPipelineBarrier(cmdBuf,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		0,
		0,
		nullptr,
		0,
		nullptr,
		1,
		&swapBarrier);

	vkEndCommandBuffer(cmdBuf);

	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores = &vkCtx.imageAvailableSemaphore;
	submit.pWaitDstStageMask = &waitStage;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmdBuf;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = &vkCtx.renderFinishedSemaphore;
	vkQueueSubmit(vkCtx.graphicsQueue, 1, &submit, vkCtx.inFlightFence);

	VkPresentInfoKHR present{};
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = &vkCtx.renderFinishedSemaphore;
	present.swapchainCount = 1;
	present.pSwapchains = &vkCtx.swapchain;
	present.pImageIndices = &imgIdx;
	VkResult presentRes = vkQueuePresentKHR(vkCtx.graphicsQueue, &present);
	if (presentRes == VK_ERROR_OUT_OF_DATE_KHR)
	{
		recreateSwapchain(static_cast<uint32_t>(state.ui.width), static_cast<uint32_t>(state.ui.height));
	}

}

/*
===============================================================================
Function Name: createVulkanTexture

Description:
	- Creates the Vulkan texture and related resources.
	- This is called when initializing the Vulkan context.

Parameters:
	- width: New width of the texture.
	- height: New height of the texture.
===============================================================================
*/
static void createVulkanTexture(uint32_t width, uint32_t height)
{
	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
	imageInfo.extent = {width, height, 1};
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_LINEAR;
	imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_GENERAL;

	vkCreateImage(vkCtx.device, &imageInfo, nullptr, &vkCtx.textureImage);

	VkMemoryRequirements memReq;
	vkGetImageMemoryRequirements(vkCtx.device, vkCtx.textureImage, &memReq);

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memReq.size;
	allocInfo.memoryTypeIndex = findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

	vkAllocateMemory(vkCtx.device, &allocInfo, nullptr, &vkCtx.textureImageMemory);
	vkBindImageMemory(vkCtx.device, vkCtx.textureImage, vkCtx.textureImageMemory, 0);

	VkImageSubresource subres = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
	VkSubresourceLayout layout = {};
	vkGetImageSubresourceLayout(vkCtx.device, vkCtx.textureImage, &subres, &layout);
	vkCtx.textureRowPitch = layout.rowPitch;
	vkMapMemory(vkCtx.device, vkCtx.textureImageMemory, 0, VK_WHOLE_SIZE, 0, &vkCtx.mappedTextureData);

	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = vkCtx.textureImage;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;

	vkCreateImageView(vkCtx.device, &viewInfo, nullptr, &vkCtx.textureImageView);

	VkSamplerCreateInfo samplerInfo = {};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_NEAREST;
	samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.maxAnisotropy = 1.0f;
	samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;

	vkCreateSampler(vkCtx.device, &samplerInfo, nullptr, &vkCtx.textureSampler);

	vkCtx.rowBuffer.resize(width * 4);
	vkCtx.previousFrameData.resize(width * height * 3);
	vkCtx.forceFullUpdate = true;
	vkCtx.textureWidth = width;
	vkCtx.textureHeight = height;
}

/*
===============================================================================
Function Name: destroyTexture

Description:
	- Destroys the Vulkan texture and related resources.
	- This is called when cleaning up the Vulkan context.
===============================================================================
*/
static void destroyTexture()
{
	if (vkCtx.mappedTextureData)
		vkUnmapMemory(vkCtx.device, vkCtx.textureImageMemory);
	if (vkCtx.textureSampler)
		vkDestroySampler(vkCtx.device, vkCtx.textureSampler, nullptr);
	if (vkCtx.textureImageView)
		vkDestroyImageView(vkCtx.device, vkCtx.textureImageView, nullptr);
	if (vkCtx.textureImage)
		vkDestroyImage(vkCtx.device, vkCtx.textureImage, nullptr);
	if (vkCtx.textureImageMemory)
		vkFreeMemory(vkCtx.device, vkCtx.textureImageMemory, nullptr);

	vkCtx.textureSampler = VK_NULL_HANDLE;
	vkCtx.textureImageView = VK_NULL_HANDLE;
	vkCtx.textureImage = VK_NULL_HANDLE;
	vkCtx.textureImageMemory = VK_NULL_HANDLE;
	vkCtx.mappedTextureData = nullptr;
	vkCtx.textureRowPitch = 0;
}

/*
===============================================================================
Function Name: resizeVulkanTexture

Description:
	- Resizes the Vulkan texture to the specified dimensions.
	- This is called when the window size changes or when the texture needs to be updated.

Parameters:
	- width: New width of the texture.
	- height: New height of the texture.
===============================================================================
*/
void resizeVulkanTexture(uint32_t width, uint32_t height)
{
	destroyTexture();
	createVulkanTexture(width, height);
	// Raycast GPU output image is separate; recreate lazily on next frame.
	if (vkCtx.raycastImage)
	{
		vkDestroyImageView(vkCtx.device, vkCtx.raycastImageView, nullptr);
		vkDestroyImage(vkCtx.device, vkCtx.raycastImage, nullptr);
		vkFreeMemory(vkCtx.device, vkCtx.raycastImageMemory, nullptr);
		vkCtx.raycastImageView = VK_NULL_HANDLE;
		vkCtx.raycastImage = VK_NULL_HANDLE;
		vkCtx.raycastImageMemory = VK_NULL_HANDLE;
		vkCtx.raycastImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	}
}

/*
===============================================================================
Function Name: recreateSwapchain

Description:
	- Recreates the Vulkan swapchain with the new dimensions.

Parameters:
	- width: New width of the swapchain.
	- height: New height of the swapchain.
===============================================================================
*/
void recreateSwapchain(uint32_t width, uint32_t height)
{
	if (!vkCtx.device || !vkCtx.surface || width == 0 || height == 0)
		return;

	// Wait for all GPU work to finish before touching swapchain resources.
	// Without this, in-flight command buffers referencing old swapchain images
	// cause implicit stalls and choppy resize on some drivers.
	vkDeviceWaitIdle(vkCtx.device);

	VkSurfaceCapabilitiesKHR caps{};
	if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vkCtx.physicalDevice, vkCtx.surface, &caps) != VK_SUCCESS)
		return;

	VkExtent2D targetExtent{};
	if (caps.currentExtent.width != UINT32_MAX)
	{
		targetExtent = caps.currentExtent;
	}
	else
	{
		targetExtent.width = (std::max)(caps.minImageExtent.width, (std::min)(caps.maxImageExtent.width, width));
		targetExtent.height = (std::max)(caps.minImageExtent.height, (std::min)(caps.maxImageExtent.height, height));
	}

	if (vkCtx.swapchain && vkCtx.swapchainExtent.width == targetExtent.width &&
		vkCtx.swapchainExtent.height == targetExtent.height)
	{
		return;
	}

	uint32_t desiredImageCount = (std::max)(2u, caps.minImageCount + 1u);
	if (caps.maxImageCount > 0)
		desiredImageCount = (std::min)(desiredImageCount, caps.maxImageCount);

	VkSwapchainKHR oldSwapchain = vkCtx.swapchain;
	VkSwapchainCreateInfoKHR swapInfo = {};
	swapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	swapInfo.surface = vkCtx.surface;
	swapInfo.minImageCount = desiredImageCount;
	swapInfo.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
	swapInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	swapInfo.imageExtent = targetExtent;
	swapInfo.imageArrayLayers = 1;
	swapInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	swapInfo.preTransform = caps.currentTransform;
	swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

	// Use FIFO (V-sync) to guarantee tear-free presentation.
	// MAILBOX can still tear on some compositors and driver configurations.
	swapInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;

	swapInfo.clipped = VK_TRUE;
	swapInfo.oldSwapchain = oldSwapchain;

	VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
	if (vkCreateSwapchainKHR(vkCtx.device, &swapInfo, nullptr, &newSwapchain) != VK_SUCCESS)
		return;

	vkCtx.swapchain = newSwapchain;
	vkCtx.swapchainExtent = targetExtent;
	vkCtx.swapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;

	uint32_t count = 0;
	vkGetSwapchainImagesKHR(vkCtx.device, vkCtx.swapchain, &count, nullptr);
	vkCtx.swapchainImages.resize(count);
	vkGetSwapchainImagesKHR(vkCtx.device, vkCtx.swapchain, &count, vkCtx.swapchainImages.data());

	if (oldSwapchain)
		vkDestroySwapchainKHR(vkCtx.device, oldSwapchain, nullptr);
}

/*
===============================================================================
Function Name: initializeVulkan

Description:
	- Initializes the Vulkan context and creates necessary resources.
===============================================================================
*/
void initializeVulkan()
{
    OutputDebugStringA("[Vulkan] initializeVulkan() called\n");

	VkApplicationInfo appInfo = {};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "Vulkan Renderer";
	appInfo.applicationVersion = (1 << 22);
	appInfo.pEngineName = "Phantom Engine";
	appInfo.engineVersion = (1 << 22);
	appInfo.apiVersion = (1 << 22);

	VkInstanceCreateInfo instInfo = {};
	instInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instInfo.pApplicationInfo = &appInfo;

	const char *exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
	instInfo.enabledExtensionCount = 2;
	instInfo.ppEnabledExtensionNames = exts;

	if (vkCreateInstance(&instInfo, nullptr, &vkCtx.instance) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan instance");

	uint32_t devCount = 0;
	if (vkEnumeratePhysicalDevices(vkCtx.instance, &devCount, nullptr) != VK_SUCCESS)
		throw std::runtime_error("Failed to enumerate Vulkan physical devices");
	if (devCount == 0)
		throw std::runtime_error("No Vulkan-capable GPU found");
	std::vector<VkPhysicalDevice> devs(devCount);
	if (vkEnumeratePhysicalDevices(vkCtx.instance, &devCount, devs.data()) != VK_SUCCESS)
		throw std::runtime_error("Failed to enumerate Vulkan physical devices");
	vkCtx.physicalDevice = devs[0];

	uint32_t qFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(vkCtx.physicalDevice, &qFamilyCount, nullptr);
	if (qFamilyCount == 0)
		throw std::runtime_error("No Vulkan queue families found");
	std::vector<VkQueueFamilyProperties> qFamilies(qFamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(vkCtx.physicalDevice, &qFamilyCount, qFamilies.data());
	bool foundGraphicsQueue = false;
	for (uint32_t i = 0; i < qFamilyCount; ++i)
	{
		if (qFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
		{
			vkCtx.graphicsQueueFamily = i;
			foundGraphicsQueue = true;
			break;
		}
	}
	if (!foundGraphicsQueue)
		throw std::runtime_error("Failed to find Vulkan graphics queue family");

	float qPriority = 1.0f;
	VkDeviceQueueCreateInfo qInfo = {};
	qInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qInfo.queueFamilyIndex = vkCtx.graphicsQueueFamily;
	qInfo.queueCount = 1;
	qInfo.pQueuePriorities = &qPriority;

	VkDeviceCreateInfo devInfo = {};
	devInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	devInfo.queueCreateInfoCount = 1;
	devInfo.pQueueCreateInfos = &qInfo;
	const char *devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
	devInfo.enabledExtensionCount = 1;
	devInfo.ppEnabledExtensionNames = devExts;

	if (vkCreateDevice(vkCtx.physicalDevice, &devInfo, nullptr, &vkCtx.device) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan logical device");
	vkGetDeviceQueue(vkCtx.device, vkCtx.graphicsQueueFamily, 0, &vkCtx.graphicsQueue);

	VkWin32SurfaceCreateInfoKHR surfInfo = {};
	surfInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	surfInfo.hwnd = g_hwnd;
	surfInfo.hinstance = GetModuleHandle(nullptr);

	// Load vkCreateWin32SurfaceKHR function pointer
	auto pfnCreateWin32SurfaceKHR = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
		vkGetInstanceProcAddr(vkCtx.instance, "vkCreateWin32SurfaceKHR"));
	if (!pfnCreateWin32SurfaceKHR)
		throw std::runtime_error("Failed to load vkCreateWin32SurfaceKHR");
	if (pfnCreateWin32SurfaceKHR(vkCtx.instance, &surfInfo, nullptr, &vkCtx.surface) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Win32 Vulkan surface");

	VkBool32 supported = VK_FALSE;
	if (vkGetPhysicalDeviceSurfaceSupportKHR(vkCtx.physicalDevice, vkCtx.graphicsQueueFamily, vkCtx.surface, &supported) != VK_SUCCESS)
		throw std::runtime_error("Failed querying Vulkan present support");
	if (!supported)
		throw std::runtime_error("Graphics queue family does not support Vulkan presentation");

	recreateSwapchain(state.ui.width, state.ui.height);
	if (!vkCtx.swapchain)
		throw std::runtime_error("Failed to create Vulkan swapchain");

	VkSemaphoreCreateInfo semInfo = {};
	semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	if (vkCreateSemaphore(vkCtx.device, &semInfo, nullptr, &vkCtx.imageAvailableSemaphore) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan image-available semaphore");
	if (vkCreateSemaphore(vkCtx.device, &semInfo, nullptr, &vkCtx.renderFinishedSemaphore) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan render-finished semaphore");

	VkFenceCreateInfo fenceInfo = {};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	if (vkCreateFence(vkCtx.device, &fenceInfo, nullptr, &vkCtx.inFlightFence) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan in-flight fence");

	VkCommandPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.queueFamilyIndex = vkCtx.graphicsQueueFamily;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

	if (vkCreateCommandPool(vkCtx.device, &poolInfo, nullptr, &vkCtx.commandPool) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan command pool");

	VkCommandBufferAllocateInfo cmdAllocInfo = {};
	cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cmdAllocInfo.commandPool = vkCtx.commandPool;
	cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cmdAllocInfo.commandBufferCount = 1;
	if (vkAllocateCommandBuffers(vkCtx.device, &cmdAllocInfo, &vkCtx.frameCommandBuffer) != VK_SUCCESS)
		throw std::runtime_error("Failed to allocate Vulkan frame command buffer");

	uint32_t texW = state.raycast.enabled ? state.ui.width : CONTENT_WIDTH;
	uint32_t texH = state.raycast.enabled ? state.ui.height : CONTENT_HEIGHT;
	resizeVulkanTexture(texW, texH);
	scaleFactor = state.raycast.enabled ? 1.0f : static_cast<float>(state.ui.width) / CONTENT_WIDTH;
}

/*
===============================================================================
Function Name: renderFrameVk

Description:
	- Renders the current frame using Vulkan.
===============================================================================
*/
void renderFrameVk()
{
	const PVXFile *vdx = state.transientPVX ? state.transientPVX : state.currentPVX;
	size_t frameIdx = state.transientPVX ? state.transient_frame_index : state.currentFrameIndex;
	if (!vdx)
		return;

	// Wait for the previous frame's blit to finish reading the texture
	// BEFORE writing new pixel data to prevent host/GPU race conditions.
	vkWaitForFences(vkCtx.device, 1, &vkCtx.inFlightFence, VK_TRUE, UINT64_MAX);

	std::span<const uint8_t> pixels = vdx->frameData[frameIdx];

	uint8_t *dst = static_cast<uint8_t *>(vkCtx.mappedTextureData);
	size_t pitch = vkCtx.textureRowPitch;

	const int texW = static_cast<int>(vkCtx.textureWidth);
	const int texH = static_cast<int>(vkCtx.textureHeight);
	auto changed = getChangedRowsAndUpdatePrevious(pixels, vkCtx.previousFrameData, texW, texH, vkCtx.forceFullUpdate || !state.transient_animation_name.empty());
	vkCtx.forceFullUpdate = false;

	for (size_t y : changed)
	{
		convertRGBRowToBGRA(pixels.data() + y * texW * 3, vkCtx.rowBuffer.data(), texW);
		std::memcpy(dst + y * pitch, vkCtx.rowBuffer.data(), static_cast<size_t>(texW) * 4);
	}

	presentFrame();
}

/*
===============================================================================
Function Name: renderFrameRaycastVk

Description:
	- Renders the current frame using raycasting with Vulkan.
	- This function assumes the raycast view is enabled and uses the current map and player state
===============================================================================
*/
void renderFrameRaycastVk()
{
	// Prefer GPU raycast in Auto/GPU mode for parity with v64tng performance.
	bool useGPU = (state.renderMode == GameState::RenderMode::GPU || state.renderMode == GameState::RenderMode::Auto);
	if (useGPU)
	{
		renderFrameRaycastVkGPU();
		return;
	}

	renderFrameRaycastVkCPU();
}

/*
===============================================================================
Function Name: renderSimpleText

Description:
	- Simple software text rendering function for menu text.
	- Uses a basic 8x16 bitmap font approach for simplicity.
===============================================================================
*/
void renderSimpleText(uint8_t* framebuffer, size_t pitch, int width, int height, 
				  const char* text, int x, int y, uint32_t color, int scale)
{
	// Use shared 8x8 bitmap font table (defined in bitmap_font.cpp)
	extern const uint8_t g_font8x8[37][8];
	const uint8_t (*font8x8)[8] = g_font8x8;

	const int glyphW = 8 * scale;
	const int glyphH = 8 * scale;
	const int charSpacing = glyphW + scale * 2; // Balanced spacing
	int len = static_cast<int>(strlen(text));
	int totalWidth = len * charSpacing - scale; // Total width minus trailing space
	int startX = x - totalWidth / 2; // Center the text
	
	// Shadow offset - larger and more distinct
	const int shadowOffset = (scale / 2) > 0 ? (scale / 2) : 1;
	const uint32_t shadowColor = 0xFF000000; // Black shadow
	
	// Helper lambda to draw text with a single 32-bit store per pixel and
	// pre-clamped scanline fills instead of per-pixel bounds checks.
	auto drawChar = [&](int charX, int charY, uint32_t drawColor) {
		for (int i = 0; i < len; i++)
		{
			char c = text[i];
			int charIndex = 0;
			
			if (c == ' ') charIndex = 0;
			else if (c >= 'A' && c <= 'Z') charIndex = c - 'A' + 1;
			else if (c >= '0' && c <= '9') charIndex = c - '0' + 27;
			else continue; // Skip unsupported characters
			
			const uint8_t* glyph = font8x8[charIndex];
			
			for (int row = 0; row < 8; row++)
			{
				uint8_t rowData = glyph[row];
				if (!rowData) continue;  // Skip fully empty glyph rows

				for (int col = 0; col < 8; col++)
				{
					if (!(rowData & (1 << col)))
						continue;

					// Pre-clamp the scale×scale block against frame bounds.
					int bx = charX + i * charSpacing + col * scale;
					int by = charY + row * scale - glyphH / 2;
					int x0 = (std::max)(bx, 0);
					int y0 = (std::max)(by, 0);
					int x1 = (std::min)(bx + scale, width);
					int y1 = (std::min)(by + scale, height);

					for (int py = y0; py < y1; py++)
					{
						uint32_t* row32 = reinterpret_cast<uint32_t*>(framebuffer + py * pitch);
						for (int px = x0; px < x1; px++)
							row32[px] = drawColor;
					}
				}
			}
		}
	};
	
	// Draw shadow first (multiple offsets for a chunkier look)
	drawChar(startX + shadowOffset,     y + shadowOffset,     shadowColor);
	drawChar(startX + shadowOffset + 2, y + shadowOffset,     shadowColor);
	drawChar(startX + shadowOffset,     y + shadowOffset + 2, shadowColor);
	drawChar(startX + shadowOffset + 2, y + shadowOffset + 2, shadowColor);

	// Draw main text on top
	drawChar(startX, y, color);
}

/*
===============================================================================
Function Name: renderFrameMenuVk

Description:
	- Renders the menu frame using Vulkan.
	- Renders menu.pvx background and overlays text using software rendering.
===============================================================================
*/
void renderFrameMenuVk()
{
	// Render menu.pvx background from streaming frame buffer
	if (!state.menu.currentFrame || state.menu.currentFrame->empty())
		return;

	// Wait for the previous frame's blit to finish reading the texture
	// BEFORE writing new pixel data. Without this, the host writes race
	// with the GPU's transfer read, causing visible flickering/phasing.
	vkWaitForFences(vkCtx.device, 1, &vkCtx.inFlightFence, VK_TRUE, UINT64_MAX);

	std::span<const uint8_t> pixels(*state.menu.currentFrame);
	ensureMenuTitleImageLoaded();

	// ---- CPU-side staging buffer for all compositing ----
	// Vulkan HOST_VISIBLE mapped memory is typically write-combined / uncached.
	// The alpha-blend in title compositing was reading back from that memory
	// (~100× slower than normal RAM), which is why menu.pvx played back so
	// slowly.  Composite into normal cached RAM, then flush once at the end.
	const uint32_t texW = vkCtx.textureWidth;
	const uint32_t texH = vkCtx.textureHeight;
	const size_t stagePitch = static_cast<size_t>(texW) * 4;

	static std::vector<uint8_t> stagingBuffer;
	if (stagingBuffer.size() < stagePitch * texH)
		stagingBuffer.resize(stagePitch * texH);
	uint8_t *stage = stagingBuffer.data();

	// RGB→BGRA: single whole-frame conversion (contiguous source & destination).
	// At 1920×1080 this processes all 2,073,600 pixels in one AVX2 blast
	// instead of 1080 separate function calls.
	convertRGBRowToBGRA(pixels.data(), stage, static_cast<size_t>(texW) * texH);

	int menuY = static_cast<int>(texH * 0.57f); // Lift menu up to give bottom breathing room

	// Render menu title — now reads/writes hit normal cached RAM.
	if (menuTitleImage && menuTitleH > 0 && menuTitleW > 0)
	{
		int paddingTop = static_cast<int>(texH * 0.02f);
		int gapPad = 30; // leave a small gap above menu text
		int availableH = menuY - paddingTop - gapPad;
		if (availableH < 0) availableH = 0;

		int targetH = availableH;
		if (targetH < 220) targetH = 220;
		if (targetH > 480) targetH = 480;
		int targetW = (menuTitleW * targetH) / menuTitleH;
		int maxW = static_cast<int>(texW * 0.9f);
		if (targetW > maxW)
		{
			targetW = maxW;
			targetH = (menuTitleH * targetW) / menuTitleW;
		}

		int startX = (static_cast<int>(texW) - targetW) / 2;
		int startY = paddingTop; // Tight to top with small padding

		// Pre-clamp loop bounds to eliminate per-pixel boundary checks.
		const int yStart = (std::max)(0, -startY);
		const int yEnd   = (std::min)(targetH, static_cast<int>(texH) - startY);
		const int xStart = (std::max)(0, -startX);
		const int xEnd   = (std::min)(targetW, static_cast<int>(texW) - startX);

		for (int y = yStart; y < yEnd; ++y)
		{
			const int dstY = startY + y;
			const int srcY = (y * menuTitleH) / targetH;
			unsigned char* dstRow = stage + static_cast<size_t>(dstY) * stagePitch;
			const unsigned char* srcRow = menuTitleImage + static_cast<size_t>(srcY) * menuTitleW * 4;

			for (int x = xStart; x < xEnd; ++x)
			{
				const int srcX = (x * menuTitleW) / targetW;
				const unsigned char* srcPixel = srcRow + srcX * 4;
				const unsigned int a = srcPixel[3];

				if (a == 0)
					continue;

				unsigned char* dstPixel = dstRow + static_cast<size_t>(startX + x) * 4;

				if (a == 255)
				{
					// Fully opaque — skip blend math entirely.
					dstPixel[0] = srcPixel[2]; // B
					dstPixel[1] = srcPixel[1]; // G
					dstPixel[2] = srcPixel[0]; // R
					dstPixel[3] = 255;
				}
				else
				{
					const unsigned int invA = 255 - a;
					// Fast /255 approximation: (v + 128) >> 8  (max error ±1 LSB).
					dstPixel[0] = static_cast<unsigned char>((srcPixel[2] * a + dstPixel[0] * invA + 128) >> 8);
					dstPixel[1] = static_cast<unsigned char>((srcPixel[1] * a + dstPixel[1] * invA + 128) >> 8);
					dstPixel[2] = static_cast<unsigned char>((srcPixel[0] * a + dstPixel[2] * invA + 128) >> 8);
					dstPixel[3] = 255;
				}
			}
		}
	}

	// Software render menu text overlay
	const char* menuItems[] = {"NEW GAME", "MULTIPLAYER", "OPTIONS", "EXIT"};
	const int menuCount = static_cast<int>(sizeof(menuItems) / sizeof(menuItems[0]));
	int itemHeight = 100; // Slightly tighter vertical pack to avoid bottom crowding

	// Animated highlight color: flicker between creamy red and light cyan.
	using namespace std::chrono;
	bool flickerPhase = (duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() / 125) % 2 == 0;
	const uint32_t highlightColorA = 0xFFFFB4A0; // creamy red (AABBGGRR layout -> B=0xA0, G=0xB4, R=0xFF)
	const uint32_t highlightColorB = 0xFF8CDCFF; // light cyan
	uint32_t highlightColor = flickerPhase ? highlightColorA : highlightColorB;

	for (int i = 0; i < menuCount; ++i)
	{
		int textY = menuY + i * itemHeight;
		bool selected = (state.menu.selectedItem == i);
		int textScale = selected ? 12 : 9; // Larger for the highlighted item
		uint32_t color = selected ? highlightColor : 0xFFFFFFFF; // White for non-selected
		renderSimpleText(stage, stagePitch, texW, texH,
					menuItems[i], texW / 2, textY, color, textScale);
	}

	// Bulk-copy the fully composited staging buffer into the mapped GPU
	// texture.  Single sequential write pass — ideal for WC/uncached memory.
	{
		uint8_t *dst = static_cast<uint8_t *>(vkCtx.mappedTextureData);
		const size_t pitch = vkCtx.textureRowPitch;
		if (pitch == stagePitch)
		{
			std::memcpy(dst, stage, stagePitch * texH);
		}
		else
		{
			for (uint32_t y = 0; y < texH; ++y)
				std::memcpy(dst + y * pitch, stage + y * stagePitch, stagePitch);
		}
	}

	presentFrame();
}

/*
===============================================================================
Function Name: presentFrame

Description:
	- Presents the rendered frame to the swapchain.
	- This function waits for the previous frame to finish before presenting the new frame.
===============================================================================
*/
void presentFrame()
{
	// NOTE: The fence wait happens BEFORE host writes in renderFrameVk/renderFrameMenuVk
	// to prevent GPU read / CPU write races. By the time we get here, the fence is
	// already signaled, so this wait is a fast no-op that just confirms readiness.
	vkWaitForFences(vkCtx.device, 1, &vkCtx.inFlightFence, VK_TRUE, UINT64_MAX);

	uint32_t imgIdx;
	VkResult acquireRes = vkAcquireNextImageKHR(vkCtx.device, vkCtx.swapchain, UINT64_MAX, vkCtx.imageAvailableSemaphore, VK_NULL_HANDLE, &imgIdx);
	if (acquireRes == VK_ERROR_OUT_OF_DATE_KHR)
	{
		recreateSwapchain(static_cast<uint32_t>(state.ui.width), static_cast<uint32_t>(state.ui.height));
		return;
	}
	if (acquireRes != VK_SUCCESS && acquireRes != VK_SUBOPTIMAL_KHR)
	{
		return;
	}
	vkResetFences(vkCtx.device, 1, &vkCtx.inFlightFence);

	VkCommandBuffer cmdBuf = vkCtx.frameCommandBuffer;
	if (cmdBuf == VK_NULL_HANDLE)
		return;
	vkResetCommandBuffer(cmdBuf, 0);

	VkCommandBufferBeginInfo beginInfo = {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmdBuf, &beginInfo);

	VkImageMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.image = vkCtx.swapchainImages[imgIdx];
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

	VkImageMemoryBarrier texBarrier = {};
	texBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	texBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
	texBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	texBarrier.oldLayout = texBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	texBarrier.image = vkCtx.textureImage;
	texBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	texBarrier.subresourceRange.levelCount = 1;
	texBarrier.subresourceRange.layerCount = 1;

	// Also add a global memory barrier to flush all host writes before the transfer read.
	// This prevents partial framebuffer reads that cause flickering/tearing on the menu.
	VkMemoryBarrier memBarrier = {};
	memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
	memBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

	vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &memBarrier, 0, nullptr, 1, &texBarrier);

	VkImageBlit blit = {};
	blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.srcSubresource.layerCount = 1;
	blit.srcOffsets[1] = {static_cast<int32_t>(vkCtx.textureWidth), static_cast<int32_t>(vkCtx.textureHeight), 1};
	blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.dstSubresource.layerCount = 1;
	if (state.raycast.enabled)
	{
		blit.dstOffsets[1] = {static_cast<int32_t>(vkCtx.swapchainExtent.width), static_cast<int32_t>(vkCtx.swapchainExtent.height), 1};
	}
	else
	{
		// Scale content to fill window width, maintain aspect ratio
		// No letterboxing needed - just stretch to fill
		blit.dstOffsets[1].x = static_cast<int32_t>(vkCtx.swapchainExtent.width);
		blit.dstOffsets[1].y = static_cast<int32_t>(vkCtx.swapchainExtent.height);
		blit.dstOffsets[1].z = 1;
	}

	vkCmdBlitImage(cmdBuf, vkCtx.textureImage, VK_IMAGE_LAYOUT_GENERAL, vkCtx.swapchainImages[imgIdx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

	vkEndCommandBuffer(cmdBuf);

	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkSubmitInfo submit = {};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores = &vkCtx.imageAvailableSemaphore;
	submit.pWaitDstStageMask = &waitStage;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmdBuf;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = &vkCtx.renderFinishedSemaphore;

	vkQueueSubmit(vkCtx.graphicsQueue, 1, &submit, vkCtx.inFlightFence);

	VkPresentInfoKHR present = {};
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = &vkCtx.renderFinishedSemaphore;
	present.swapchainCount = 1;
	present.pSwapchains = &vkCtx.swapchain;
	present.pImageIndices = &imgIdx;
	VkResult presentRes = vkQueuePresentKHR(vkCtx.graphicsQueue, &present);
	if (presentRes == VK_ERROR_OUT_OF_DATE_KHR)
	{
		recreateSwapchain(static_cast<uint32_t>(state.ui.width), static_cast<uint32_t>(state.ui.height));
	}

}

/*
===============================================================================
Function Name: cleanupVulkan

Description:
	- Cleans up the Vulkan resources and resets the Vulkan context.
	- This is called when the application is shutting down or when the window is destroyed.
===============================================================================
*/
void cleanupVulkan()
{
	destroyRaycastResources();
	destroyTexture();
	if (vkCtx.swapchain)
		vkDestroySwapchainKHR(vkCtx.device, vkCtx.swapchain, nullptr);
	if (vkCtx.surface)
		vkDestroySurfaceKHR(vkCtx.instance, vkCtx.surface, nullptr);
	if (vkCtx.imageAvailableSemaphore)
		vkDestroySemaphore(vkCtx.device, vkCtx.imageAvailableSemaphore, nullptr);
	if (vkCtx.renderFinishedSemaphore)
		vkDestroySemaphore(vkCtx.device, vkCtx.renderFinishedSemaphore, nullptr);
	if (vkCtx.inFlightFence)
		vkDestroyFence(vkCtx.device, vkCtx.inFlightFence, nullptr);
	if (vkCtx.frameCommandBuffer && vkCtx.commandPool)
		vkFreeCommandBuffers(vkCtx.device, vkCtx.commandPool, 1, &vkCtx.frameCommandBuffer);
	if (vkCtx.commandPool)
		vkDestroyCommandPool(vkCtx.device, vkCtx.commandPool, nullptr);
	if (vkCtx.device)
		vkDestroyDevice(vkCtx.device, nullptr);
	if (vkCtx.instance)
		vkDestroyInstance(vkCtx.instance, nullptr);
	if (menuTitleImage)
	{
		FreeImage(menuTitleImage);
		menuTitleImage = nullptr;
		menuTitleW = menuTitleH = menuTitleC = 0;
	}

	vkCtx = {};
}
