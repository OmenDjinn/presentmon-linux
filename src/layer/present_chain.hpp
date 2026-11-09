// SPDX-License-Identifier: MIT
// pNext-chain handling for the layer. Vulkan hands us const chains owned by the application, so
// nothing here ever writes into them: modified chains (the present chain, the device-creation
// feature chain) are rebuilt as copies. Chain nodes we cannot copy safely (structure types missing
// from the tables generated out of vk.xml, see chain_sizes.inc) make the rebuild fail and the
// caller falls back to an untouched present / to not enabling present timing.
#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pml::layer {

template <class T>
inline const T *findStruct(const void *pNext, VkStructureType sType)
{
	while (pNext)
	{
		const auto *s = static_cast<const VkBaseInStructure *>(pNext);
		if (s->sType == sType)
			return static_cast<const T *>(pNext);
		pNext = s->pNext;
	}
	return nullptr;
}

// Size of a VkPresentInfoKHR-extending structure we know how to copy, 0 if unknown. Every one of
// these holds only plain data and pointers into application memory, so a byte copy is a valid copy.
size_t knownPresentNodeSize(VkStructureType sType);
// The same for structures extending VkDeviceCreateInfo (plus the loader's own chain node).
size_t knownDeviceNodeSize(VkStructureType sType);

// Builds a modified copy of a VkPresentInfoKHR whose chain carries our VkPresentTimingsInfoEXT and
// VkPresentId2KHR instead of the application's (if it has any). One instance per present call; the
// result stays valid as long as the instance and the input arrays do.
class PresentChain
{
public:
	// `timings` (one entry per swapchain) replaces/adds VkPresentTimingsInfoEXT when non-null.
	// `ids` (one per swapchain) replaces/adds VkPresentId2KHR when non-null.
	// `results` replaces pResults when non-null (must have swapchainCount entries).
	// Every other node of the application's chain (present fences included) goes down unchanged.
	// Returns false if the chain cannot be rebuilt; `out` is then undefined.
	bool rebuild(const VkPresentInfoKHR &src, const VkPresentTimingInfoEXT *timings, const uint64_t *ids,
	             VkResult *results, VkPresentInfoKHR *out, VkStructureType *unknownType = nullptr);

private:
	template <class T>
	T *allocate(size_t bytes);

	std::array<uint64_t, 160> arena_;  // 1280 bytes, 8-byte aligned
	size_t used_ = 0;
	VkPresentTimingsInfoEXT timingsNode_ = {};
	VkPresentId2KHR idNode_ = {};
};

// Switches on VkBool32 members of feature structures in the application's device-creation chain
// without writing into it. A structure whose field is already TRUE is left alone, a missing one is
// reported (the caller chains its own in front), and one with the field FALSE is replaced by a
// patched copy: the chain is copied up to and including the last such structure, the rest is shared.
class DeviceFeatureChain
{
public:
	struct Request
	{
		VkStructureType sType;
		size_t fieldOffset;
	};
	enum class Found
	{
		Missing,
		AlreadyOn,
		Patched,
	};

	// `found[i]` tells what happened to `request[i]`; `*head` is the chain to use. Returns false if a
	// copy was needed but a structure in the way has an unknown size; nothing is usable then.
	bool build(const void *pNext, const Request *requests, size_t count, Found *found, const void **head,
	           VkStructureType *unknownType = nullptr);

private:
	std::vector<std::vector<uint64_t>> storage_;  // stable heap blocks, 8-byte aligned
};

} // namespace pml::layer
