// SPDX-License-Identifier: MIT
// Tests for the pNext chain handling (present chain rebuild, feature enabling).
#include "present_chain.hpp"

#include <vulkan/vk_layer.h>

#include <gtest/gtest.h>

using namespace pml::layer;

namespace {

VkPresentInfoKHR basePresent(const VkSwapchainKHR *sc, const uint32_t *idx, uint32_t count = 1)
{
	VkPresentInfoKHR p = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	p.swapchainCount = count;
	p.pSwapchains = sc;
	p.pImageIndices = idx;
	return p;
}

std::vector<VkStructureType> chainTypes(const void *pNext)
{
	std::vector<VkStructureType> v;
	for (auto *n = static_cast<const VkBaseInStructure *>(pNext); n; n = n->pNext)
		v.push_back(n->sType);
	return v;
}

} // namespace

TEST(PresentChain, AddsNodesToAnEmptyChain)
{
	VkSwapchainKHR sc[1] = {VK_NULL_HANDLE};
	uint32_t idx[1] = {0};
	VkPresentInfoKHR src = basePresent(sc, idx);
	VkPresentTimingInfoEXT timing[1] = {{VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT}};
	timing[0].presentStageQueries = 5;
	uint64_t ids[1] = {77};

	PresentChain chain;
	VkPresentInfoKHR out;
	ASSERT_TRUE(chain.rebuild(src, timing, ids, nullptr, &out));
	EXPECT_EQ(src.pNext, nullptr) << "the application's struct is untouched";
	auto types = chainTypes(out.pNext);
	ASSERT_EQ(types.size(), 2u);
	EXPECT_EQ(types[0], VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT);
	EXPECT_EQ(types[1], VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR);
	auto *tn = static_cast<const VkPresentTimingsInfoEXT *>(out.pNext);
	EXPECT_EQ(tn->swapchainCount, 1u);
	EXPECT_EQ(tn->pTimingInfos[0].presentStageQueries, 5u);
	auto *in = static_cast<const VkPresentId2KHR *>(tn->pNext);
	EXPECT_EQ(in->pPresentIds[0], 77u);
}

TEST(PresentChain, ReplacesTheAppsTimingAndIdNodesAndCopiesTheOthers)
{
	VkSwapchainKHR sc[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	uint32_t idx[2] = {1, 2};
	// App chain (head first): fence info -> timings -> mode info -> present ids -> regions
	VkPresentRegionsKHR regions = {VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR};
	uint64_t appIds[2] = {10, 11};
	VkPresentId2KHR appId2 = {VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR, &regions, 2, appIds};
	VkPresentModeKHR modes[2] = {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR};
	VkSwapchainPresentModeInfoKHR modeInfo = {VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR, &appId2, 2, modes};
	VkPresentTimingInfoEXT appTimings[2] = {{VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT}, {VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT}};
	VkPresentTimingsInfoEXT appTimingsNode = {VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT, &modeInfo, 2, appTimings};
	VkFence fences[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	VkSwapchainPresentFenceInfoKHR fenceInfo = {VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR, &appTimingsNode, 2, fences};
	VkPresentInfoKHR src = basePresent(sc, idx, 2);
	src.pNext = &fenceInfo;

	VkPresentTimingInfoEXT ours[2] = {{VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT}, {VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT}};
	ours[0].presentStageQueries = 1;
	ours[1].presentStageQueries = 4;
	PresentChain chain;
	VkPresentInfoKHR out;
	// Replace only the timings: the id node and the fence/mode nodes survive.
	ASSERT_TRUE(chain.rebuild(src, ours, nullptr, nullptr, &out));
	auto types = chainTypes(out.pNext);
	std::vector<VkStructureType> expected = {VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT,
	                                         VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
	                                         VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR,
	                                         VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
	                                         VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR};
	EXPECT_EQ(types, expected);

	// The original chain is exactly as it was.
	EXPECT_EQ(src.pNext, &fenceInfo);
	EXPECT_EQ(fenceInfo.pNext, &appTimingsNode);
	EXPECT_EQ(appTimingsNode.pNext, &modeInfo);
	EXPECT_EQ(modeInfo.pNext, &appId2);
	EXPECT_EQ(appTimings[0].presentStageQueries, 0u);

	// The copies point at the application's arrays; the tail (after the last replaced node) is shared.
	auto *newTimings = static_cast<const VkPresentTimingsInfoEXT *>(out.pNext);
	EXPECT_EQ(newTimings->pTimingInfos, ours);
	auto *copiedFence = static_cast<const VkSwapchainPresentFenceInfoKHR *>(newTimings->pNext);
	EXPECT_NE(copiedFence, &fenceInfo);
	EXPECT_EQ(copiedFence->pFences, fences);
	auto *copiedMode = static_cast<const VkSwapchainPresentModeInfoKHR *>(copiedFence->pNext);
	EXPECT_EQ(copiedMode->pPresentModes, modes);
	EXPECT_EQ(copiedMode->pNext, &appId2) << "nodes after the last replaced one are shared untouched";

	// Replace the ids as well: now the tail is the regions node.
	uint64_t newIds[2] = {1, 2};
	ASSERT_TRUE(chain.rebuild(src, ours, newIds, nullptr, &out));
	types = chainTypes(out.pNext);
	expected = {VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT, VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
	            VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR,
	            VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR};
	EXPECT_EQ(types, expected);
	EXPECT_EQ(appId2.pPresentIds, appIds);
}

TEST(PresentChain, UnknownNodeBeforeAReplacedOneFailsInsteadOfGuessing)
{
	VkSwapchainKHR sc[1] = {VK_NULL_HANDLE};
	uint32_t idx[1] = {0};
	VkPresentTimingInfoEXT t[1] = {{VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT}};
	VkPresentTimingsInfoEXT appTimingsNode = {VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT, nullptr, 1, t};
	VkBaseInStructure unknown = {VkStructureType(1000999999), reinterpret_cast<const VkBaseInStructure *>(&appTimingsNode)};
	VkPresentInfoKHR src = basePresent(sc, idx);
	src.pNext = &unknown;

	PresentChain chain;
	VkPresentInfoKHR out;
	EXPECT_FALSE(chain.rebuild(src, t, nullptr, nullptr, &out));
}

TEST(PresentChain, UnknownNodeAfterTheReplacedOnesIsSharedByPointer)
{
	VkSwapchainKHR sc[1] = {VK_NULL_HANDLE};
	uint32_t idx[1] = {0};
	VkPresentTimingInfoEXT t[1] = {{VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT}};
	VkBaseInStructure unknown = {VkStructureType(1000999999), nullptr};
	VkPresentTimingsInfoEXT appTimingsNode = {VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT, &unknown, 1, t};
	VkPresentInfoKHR src = basePresent(sc, idx);
	src.pNext = &appTimingsNode;

	PresentChain chain;
	VkPresentInfoKHR out;
	ASSERT_TRUE(chain.rebuild(src, t, nullptr, nullptr, &out));
	auto *n = static_cast<const VkPresentTimingsInfoEXT *>(out.pNext);
	EXPECT_NE(n, &appTimingsNode);
	EXPECT_EQ(n->pNext, &unknown);
}

TEST(PresentChain, ResultsArrayCanBeSubstituted)
{
	VkSwapchainKHR sc[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	uint32_t idx[2] = {0, 0};
	VkPresentInfoKHR src = basePresent(sc, idx, 2);
	VkResult results[2];
	PresentChain chain;
	VkPresentInfoKHR out;
	ASSERT_TRUE(chain.rebuild(src, nullptr, nullptr, results, &out));
	EXPECT_EQ(out.pResults, results);
	EXPECT_EQ(src.pResults, nullptr);
	EXPECT_EQ(out.pNext, nullptr);
}

// The driver-QUEUE_FULL repeat rebuilds the chain with the timing stages cleared: the application's present
// fences must still go down (the rejected attempt never signals them on this driver).
TEST(PresentChain, RebuildKeepsThePresentFences)
{
	VkSwapchainKHR sc[1] = {VK_NULL_HANDLE};
	uint32_t idx[1] = {0};
	VkPresentRegionsKHR regions = {VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR};
	VkFence fences[1] = {VK_NULL_HANDLE};
	VkSwapchainPresentFenceInfoKHR fenceInfo = {VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR, &regions, 1, fences};
	VkPresentModeKHR modes[1] = {VK_PRESENT_MODE_FIFO_KHR};
	VkSwapchainPresentModeInfoKHR modeInfo = {VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR, &fenceInfo, 1, modes};
	VkPresentInfoKHR src = basePresent(sc, idx);
	src.pNext = &modeInfo;

	PresentChain chain;
	VkPresentInfoKHR out;
	VkPresentTimingInfoEXT timing[1] = {{VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT}};
	ASSERT_TRUE(chain.rebuild(src, timing, nullptr, nullptr, &out, nullptr));
	std::vector<VkStructureType> expected = {VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT,   // added in front
	                                         VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR,
	                                         VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
	                                         VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR};
	EXPECT_EQ(chainTypes(out.pNext), expected);
	for (auto *n = static_cast<const VkBaseInStructure *>(out.pNext); n; n = n->pNext)
	{
		if (n->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR)
		{
			EXPECT_EQ(reinterpret_cast<const VkSwapchainPresentFenceInfoKHR *>(n)->pFences, fences);
		}
	}
	EXPECT_EQ(src.pNext, &modeInfo);
	EXPECT_EQ(modeInfo.pNext, &fenceInfo) << "the application's chain is untouched";
}

TEST(ChainTables, EveryStructureThatExtendsVkPresentInfoKhrIsKnown)
{
	// Everything vk.xml 1.4.363 lists with structextends=VkPresentInfoKHR (the generated table is also
	// compared with vk.xml by the pml_chain_table_check test).
	const VkStructureType all[] = {
		VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR,    VK_STRUCTURE_TYPE_DISPLAY_PRESENT_INFO_KHR,
		VK_STRUCTURE_TYPE_FRAME_BOUNDARY_EXT,               VK_STRUCTURE_TYPE_FRAME_BOUNDARY_TENSORS_ARM,
		VK_STRUCTURE_TYPE_PRESENT_FRAME_TOKEN_GGP,          VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
		VK_STRUCTURE_TYPE_PRESENT_ID_KHR,                   VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR,
		VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE,        VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT,
		VK_STRUCTURE_TYPE_SET_PRESENT_CONFIG_NV,            VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
		VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR,
	};
	for (VkStructureType t : all)
		EXPECT_GE(knownPresentNodeSize(t), sizeof(VkBaseInStructure)) << "sType " << t;
	EXPECT_EQ(knownPresentNodeSize(VkStructureType(1000999999)), 0u);
	EXPECT_EQ(knownPresentNodeSize(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT), 0u)
		<< "device-chain structures are not present-chain nodes";
}

TEST(ChainTables, DeviceChainKnowsFeatureStructsAndTheLoaderNode)
{
	EXPECT_EQ(knownDeviceNodeSize(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2), sizeof(VkPhysicalDeviceFeatures2));
	EXPECT_EQ(knownDeviceNodeSize(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT),
	          sizeof(VkPhysicalDevicePresentTimingFeaturesEXT));
	EXPECT_EQ(knownDeviceNodeSize(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES), sizeof(VkPhysicalDeviceVulkan12Features));
	EXPECT_EQ(knownDeviceNodeSize(VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO), sizeof(VkLayerDeviceCreateInfo));
	EXPECT_EQ(knownDeviceNodeSize(VkStructureType(1000999999)), 0u);
}

namespace {
const VkPhysicalDevicePresentId2FeaturesKHR kConstId2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR, nullptr, VK_FALSE};
} // namespace

TEST(DeviceFeatureChain, PatchesACopyAndNeverWritesTheApplicationsStructs)
{
	// A const struct with static storage: the old in-place patching wrote into memory like this.
	static const VkPhysicalDevicePresentTimingFeaturesEXT kConstTiming = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT, const_cast<VkPhysicalDevicePresentId2FeaturesKHR *>(&kConstId2), VK_FALSE};
	VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, const_cast<VkPhysicalDevicePresentTimingFeaturesEXT *>(&kConstTiming)};

	const DeviceFeatureChain::Request req[2] = {
		{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT, offsetof(VkPhysicalDevicePresentTimingFeaturesEXT, presentTiming)},
		{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR, offsetof(VkPhysicalDevicePresentId2FeaturesKHR, presentId2)}};
	DeviceFeatureChain::Found found[2];
	const void *head = nullptr;
	DeviceFeatureChain chain;
	ASSERT_TRUE(chain.build(&features, req, 2, found, &head));
	EXPECT_EQ(found[0], DeviceFeatureChain::Found::Patched);
	EXPECT_EQ(found[1], DeviceFeatureChain::Found::Patched);
	EXPECT_NE(head, &features) << "the chain is a copy";

	auto *f2 = static_cast<const VkPhysicalDeviceFeatures2 *>(head);
	ASSERT_EQ(f2->sType, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
	auto *t = static_cast<const VkPhysicalDevicePresentTimingFeaturesEXT *>(f2->pNext);
	ASSERT_EQ(t->sType, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT);
	EXPECT_EQ(t->presentTiming, VK_TRUE);
	auto *i2 = static_cast<const VkPhysicalDevicePresentId2FeaturesKHR *>(t->pNext);
	ASSERT_EQ(i2->sType, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR);
	EXPECT_EQ(i2->presentId2, VK_TRUE);
	EXPECT_EQ(i2->pNext, nullptr);

	EXPECT_EQ(kConstTiming.presentTiming, VK_FALSE) << "the application's structs are untouched";
	EXPECT_EQ(kConstId2.presentId2, VK_FALSE);
	EXPECT_EQ(features.pNext, &kConstTiming);
}

TEST(DeviceFeatureChain, EnabledAndMissingStructsNeedNoCopy)
{
	VkPhysicalDevicePresentTimingFeaturesEXT timing = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT};
	timing.presentTiming = VK_TRUE;
	const DeviceFeatureChain::Request req[2] = {
		{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT, offsetof(VkPhysicalDevicePresentTimingFeaturesEXT, presentTiming)},
		{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR, offsetof(VkPhysicalDevicePresentId2FeaturesKHR, presentId2)}};
	DeviceFeatureChain::Found found[2];
	const void *head = nullptr;
	DeviceFeatureChain chain;
	ASSERT_TRUE(chain.build(&timing, req, 2, found, &head));
	EXPECT_EQ(found[0], DeviceFeatureChain::Found::AlreadyOn);
	EXPECT_EQ(found[1], DeviceFeatureChain::Found::Missing);
	EXPECT_EQ(head, &timing) << "nothing to patch: the application's chain is used as it is";

	ASSERT_TRUE(chain.build(nullptr, req, 2, found, &head));
	EXPECT_EQ(found[0], DeviceFeatureChain::Found::Missing);
	EXPECT_EQ(head, nullptr);
}

TEST(DeviceFeatureChain, UnknownStructureInTheWayFailsInsteadOfGuessing)
{
	VkPhysicalDevicePresentTimingFeaturesEXT timing = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT};
	VkBaseInStructure unknown = {VkStructureType(1000999999), reinterpret_cast<const VkBaseInStructure *>(&timing)};
	const DeviceFeatureChain::Request req[1] = {
		{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT, offsetof(VkPhysicalDevicePresentTimingFeaturesEXT, presentTiming)}};
	DeviceFeatureChain::Found found[1];
	const void *head = nullptr;
	VkStructureType unknownType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	DeviceFeatureChain chain;
	EXPECT_FALSE(chain.build(&unknown, req, 1, found, &head, &unknownType));
	EXPECT_EQ(unknownType, VkStructureType(1000999999));
	EXPECT_EQ(timing.presentTiming, VK_FALSE);

	// An unknown structure behind the patched one is shared by pointer, not a problem.
	VkPhysicalDevicePresentTimingFeaturesEXT timing2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT,
	                                                    reinterpret_cast<void *>(&unknown)};
	unknown.pNext = nullptr;
	ASSERT_TRUE(chain.build(&timing2, req, 1, found, &head));
	auto *copy = static_cast<const VkPhysicalDevicePresentTimingFeaturesEXT *>(head);
	EXPECT_EQ(copy->presentTiming, VK_TRUE);
	EXPECT_EQ(copy->pNext, &unknown);
}
