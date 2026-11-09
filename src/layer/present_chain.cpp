// SPDX-License-Identifier: MIT
#include "present_chain.hpp"

#include <vulkan/vk_layer.h>

#include <cstring>

namespace pml::layer {

// The cases are generated from vk.xml (structextends) by tools/gen_chain_sizes.py.
size_t knownPresentNodeSize(VkStructureType sType)
{
	switch (sType)
	{
#define PML_PRESENT_NODE(stype, type) case stype: return sizeof(type);
#define PML_PRESENT_NODE_SIZE(stype, size) case stype: return size;
#define PML_DEVICE_NODE(stype, type)
#define PML_DEVICE_NODE_SIZE(stype, size)
#include "chain_sizes.inc"
#undef PML_PRESENT_NODE
#undef PML_PRESENT_NODE_SIZE
#undef PML_DEVICE_NODE
#undef PML_DEVICE_NODE_SIZE
	default: return 0;
	}
}

size_t knownDeviceNodeSize(VkStructureType sType)
{
	switch (sType)
	{
#define PML_PRESENT_NODE(stype, type)
#define PML_PRESENT_NODE_SIZE(stype, size)
#define PML_DEVICE_NODE(stype, type) case stype: return sizeof(type);
#define PML_DEVICE_NODE_SIZE(stype, size) case stype: return size;
#include "chain_sizes.inc"
#undef PML_PRESENT_NODE
#undef PML_PRESENT_NODE_SIZE
#undef PML_DEVICE_NODE
#undef PML_DEVICE_NODE_SIZE
	default: return 0;
	}
}

template <class T>
T *PresentChain::allocate(size_t bytes)
{
	size_t words = (bytes + 7) / 8;
	if (used_ + words > arena_.size())
		return nullptr;
	T *p = reinterpret_cast<T *>(&arena_[used_]);
	used_ += words;
	return p;
}

bool PresentChain::rebuild(const VkPresentInfoKHR &src, const VkPresentTimingInfoEXT *timings, const uint64_t *ids,
                           VkResult *results, VkPresentInfoKHR *out, VkStructureType *unknownType)
{
	used_ = 0;
	*out = src;
	if (results)
		out->pResults = results;

	const bool replaceTimings = timings != nullptr;
	const bool replaceIds = ids != nullptr;

	// The last node of the application's chain that we replace. Everything before it has to be
	// copied (its pNext must change); everything after it is shared by pointer.
	const VkBaseInStructure *lastReplaced = nullptr;
	for (auto *n = static_cast<const VkBaseInStructure *>(src.pNext); n; n = n->pNext)
	{
		if ((replaceTimings && n->sType == VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT) ||
		    (replaceIds && n->sType == VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR))
			lastReplaced = n;
	}

	const void *tail = src.pNext;
	VkBaseOutStructure *copiedHead = nullptr, *copiedTail = nullptr;
	if (lastReplaced)
	{
		for (auto *n = static_cast<const VkBaseInStructure *>(src.pNext);; n = n->pNext)
		{
			const bool isReplaced = (replaceTimings && n->sType == VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT) ||
			                        (replaceIds && n->sType == VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR);
			if (!isReplaced)
			{
				size_t size = knownPresentNodeSize(n->sType);
				if (!size)
				{
					if (unknownType)
						*unknownType = n->sType;
					return false;
				}
				auto *copy = allocate<VkBaseOutStructure>(size);
				if (!copy)
					return false;
				std::memcpy(copy, n, size);
				copy->pNext = nullptr;
				if (copiedTail)
					copiedTail->pNext = copy;
				else
					copiedHead = copy;
				copiedTail = copy;
			}
			if (n == lastReplaced)
			{
				tail = n->pNext;
				break;
			}
		}
	}

	// Chain: [timings] -> [ids] -> copied nodes -> shared tail
	const void *head = tail;
	if (copiedHead)
	{
		copiedTail->pNext = const_cast<VkBaseOutStructure *>(static_cast<const VkBaseOutStructure *>(tail));
		head = copiedHead;
	}
	if (replaceIds)
	{
		idNode_ = {};
		idNode_.sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR;
		idNode_.pNext = head;
		idNode_.swapchainCount = src.swapchainCount;
		idNode_.pPresentIds = ids;
		head = &idNode_;
	}
	if (replaceTimings)
	{
		timingsNode_ = {};
		timingsNode_.sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT;
		timingsNode_.pNext = head;
		timingsNode_.swapchainCount = src.swapchainCount;
		timingsNode_.pTimingInfos = timings;
		head = &timingsNode_;
	}
	out->pNext = head;
	return true;
}

bool DeviceFeatureChain::build(const void *pNext, const Request *requests, size_t count, Found *found,
                               const void **head, VkStructureType *unknownType)
{
	storage_.clear();
	*head = pNext;
	const VkBaseInStructure *last = nullptr;  // last node that needs its field switched on
	for (size_t r = 0; r < count; r++)
	{
		found[r] = Found::Missing;
		for (auto *n = static_cast<const VkBaseInStructure *>(pNext); n; n = n->pNext)
		{
			if (n->sType != requests[r].sType)
				continue;
			const auto *field = reinterpret_cast<const VkBool32 *>(reinterpret_cast<const char *>(n) + requests[r].fieldOffset);
			found[r] = *field != VK_FALSE ? Found::AlreadyOn : Found::Patched;
			break;
		}
	}
	// Position of the last node to patch.
	size_t lastIndex = 0;
	bool any = false;
	size_t index = 0;
	for (auto *n = static_cast<const VkBaseInStructure *>(pNext); n; n = n->pNext, index++)
		for (size_t r = 0; r < count; r++)
			if (found[r] == Found::Patched && n->sType == requests[r].sType)
			{
				last = n;
				lastIndex = index;
				any = true;
			}
	if (!any)
		return true;

	index = 0;
	VkBaseOutStructure *copiedHead = nullptr, *copiedTail = nullptr;
	for (auto *n = static_cast<const VkBaseInStructure *>(pNext); index <= lastIndex; n = n->pNext, index++)
	{
		const size_t size = knownDeviceNodeSize(n->sType);
		if (!size)
		{
			if (unknownType)
				*unknownType = n->sType;
			storage_.clear();
			return false;
		}
		storage_.emplace_back((size + 7) / 8);
		auto *copy = reinterpret_cast<VkBaseOutStructure *>(storage_.back().data());
		std::memcpy(copy, n, size);
		copy->pNext = nullptr;
		for (size_t r = 0; r < count; r++)
			if (found[r] == Found::Patched && n->sType == requests[r].sType)
				*reinterpret_cast<VkBool32 *>(reinterpret_cast<char *>(copy) + requests[r].fieldOffset) = VK_TRUE;
		if (copiedTail)
			copiedTail->pNext = copy;
		else
			copiedHead = copy;
		copiedTail = copy;
	}
	copiedTail->pNext = const_cast<VkBaseOutStructure *>(reinterpret_cast<const VkBaseOutStructure *>(last->pNext));
	*head = copiedHead;
	return true;
}

} // namespace pml::layer
