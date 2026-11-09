// SPDX-License-Identifier: MIT
#include "gpu_timer.hpp"

#include "log.hpp"
#include "vk_dispatch.hpp"

#include "pml/clock.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace pml::layer {

namespace {

constexpr uint32_t kSlotsPerChunk = 64;            // a slot = one timed batch = two queries + two command buffers
constexpr uint32_t kMaxChunksPerFamily = 8;        // 512 batches in flight per queue family
constexpr uint64_t kAbandonNs = 5'000'000'000ull;  // a batch not done after this long is given up on
constexpr uint64_t kCalibrateEveryNs = 1'000'000'000ull;
constexpr uint64_t kOwnerStaleNs = 250'000'000ull;    // the GPU-numbers swapchain gave no ready time this long

#define PML_GPU_FUNCTIONS(X) \
	X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) \
	X(CmdResetQueryPool) X(CmdWriteTimestamp) X(CreateQueryPool) X(DestroyQueryPool) X(GetQueryPoolResults) \
	X(CreateFence) X(DestroyFence) X(GetFenceStatus) X(QueueSubmit)

struct Fns
{
#define X(name) PFN_vk##name name = nullptr;
	PML_GPU_FUNCTIONS(X)
#undef X
	PFN_vkGetCalibratedTimestampsKHR GetCalibratedTimestamps = nullptr;
};

template <class T> const T *findInChain(const void *pNext, VkStructureType type)
{
	for (auto *s = static_cast<const VkBaseInStructure *>(pNext); s; s = s->pNext)
		if (s->sType == type)
			return reinterpret_cast<const T *>(s);
	return nullptr;
}

bool timeable(const VkSubmitInfo &s)
{
	if (s.commandBufferCount == 0 || !s.pCommandBuffers)
		return false;
	if (findInChain<VkDeviceGroupSubmitInfo>(s.pNext, VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO))
		return false;
	if (auto *p = findInChain<VkProtectedSubmitInfo>(s.pNext, VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO))
		if (p->protectedSubmit)
			return false;
	return true;
}

bool timeable(const VkSubmitInfo2 &s)
{
	return s.commandBufferInfoCount > 0 && s.pCommandBufferInfos && !(s.flags & VK_SUBMIT_PROTECTED_BIT);
}

} // namespace

struct GpuTimer::Impl
{
	struct Chunk
	{
		VkQueryPool queries = VK_NULL_HANDLE;
		VkCommandBuffer pre[kSlotsPerChunk] = {};
		VkCommandBuffer post[kSlotsPerChunk] = {};
		uint64_t lastRawEnd[kSlotsPerChunk] = {};   // the slot's previous end tick: equal = stale result
		VkFence resetFence = VK_NULL_HANDLE;        // the one-time reset of every query (first use only)
		bool ready = false;
		std::vector<uint32_t> freeSlots;
	};
	struct Family
	{
		bool probed = false;
		bool usable = false;
		VkCommandPool pool = VK_NULL_HANDLE;
		std::vector<std::unique_ptr<Chunk>> chunks;
	};
	struct Flight
	{
		uint32_t family;
		uint32_t chunk;
		uint32_t slot;
		VkQueue queue;
		uint64_t submitNs;
		bool abandoned = false;   // not done after kAbandonNs: no longer attributed, its slot freed once it runs
	};
	struct Taken
	{
		uint32_t chunk = UINT32_MAX;
		uint32_t slot = 0;
	};

	DeviceContext ctx;
	Fns fn;
	std::vector<VkQueueFamilyProperties> familyProps;
	double periodNs = 1.0;

	std::mutex mutex;
	GpuClock clock;
	uint64_t lastCalibrationNs = 0;
	std::vector<Family> families;
	std::deque<Flight> inFlight;
	std::unordered_map<VkQueue, uint64_t> queueLastEnd;
	GpuFrameAttributor attributor;
	GpuTimerStats stats;
	// GPU numbers go to one swapchain per device (the first one with ready times; another one takes over when
	// it is destroyed or has given none for kOwnerStaleNs): the attributor has no notion of swapchains, and two
	// of them would take each other's work.
	uint64_t ownerSerial = 0;
	uint64_t ownerLastNs = 0;

	bool calibrate()
	{
		VkCalibratedTimestampInfoKHR infos[2] = {
			{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, nullptr, VK_TIME_DOMAIN_DEVICE_KHR},
			{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, nullptr, VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR}};
		uint64_t best[2] = {}, bestDev = UINT64_MAX;
		for (int i = 0; i < 3; i++)
		{
			uint64_t ts[2] = {}, dev = 0;
			if (fn.GetCalibratedTimestamps(ctx.device, 2, infos, ts, &dev) != VK_SUCCESS)
				continue;
			if (dev < bestDev)
			{
				bestDev = dev;
				best[0] = ts[0];
				best[1] = ts[1];
			}
		}
		if (bestDev == UINT64_MAX)
			return false;
		clock.calTicks = best[0];
		clock.calMonoNs = best[1];
		clock.calibrated = true;
		lastCalibrationNs = pml::monotonicNs();
		return true;
	}

	bool probe(uint32_t familyIndex)
	{
		if (familyIndex >= families.size())
			return false;
		Family &f = families[familyIndex];
		if (f.probed)
			return f.usable;
		f.probed = true;
		const VkQueueFamilyProperties &p = familyProps[familyIndex];
		if (p.timestampValidBits == 0 || !(p.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
			return false;
		VkCommandPoolCreateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		ci.queueFamilyIndex = familyIndex;
		if (fn.CreateCommandPool(ctx.device, &ci, nullptr, &f.pool) != VK_SUCCESS)
			return false;
		f.usable = true;
		return true;
	}

	void destroyChunk(Chunk &c)
	{
		if (c.queries)
			fn.DestroyQueryPool(ctx.device, c.queries, nullptr);
		if (c.resetFence)
			fn.DestroyFence(ctx.device, c.resetFence, nullptr);
		c.queries = VK_NULL_HANDLE;
		c.resetFence = VK_NULL_HANDLE;
	}

	// A new chunk: queries, recorded command buffers, and the one-time reset of its queries submitted on
	// `queue` (held by the calling thread). Its slots are used once that reset has completed.
	void addChunk(uint32_t familyIndex, VkQueue queue)
	{
		Family &f = families[familyIndex];
		auto c = std::make_unique<Chunk>();
		VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
		qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
		qi.queryCount = kSlotsPerChunk * 2;
		if (fn.CreateQueryPool(ctx.device, &qi, nullptr, &c->queries) != VK_SUCCESS)
		{
			f.usable = false;
			return;
		}
		VkCommandBuffer cmds[kSlotsPerChunk * 2 + 1] = {};
		VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		ai.commandPool = f.pool;
		ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ai.commandBufferCount = kSlotsPerChunk * 2 + 1;
		VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		if (fn.AllocateCommandBuffers(ctx.device, &ai, cmds) != VK_SUCCESS ||
		    fn.CreateFence(ctx.device, &fi, nullptr, &c->resetFence) != VK_SUCCESS)
		{
			destroyChunk(*c);
			f.usable = false;
			return;
		}
		for (VkCommandBuffer cmd : cmds)
			ctx.setDeviceLoaderData(ctx.device, cmd);

		bool ok = true;
		VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		// Reused for every batch of the slot: recorded once, resubmitted while a previous submission may
		// formally still be pending (its timestamps were already read).
		bi.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
		for (uint32_t i = 0; i < kSlotsPerChunk && ok; i++)
		{
			c->pre[i] = cmds[2 * i];
			c->post[i] = cmds[2 * i + 1];
			ok = fn.BeginCommandBuffer(c->pre[i], &bi) == VK_SUCCESS;
			if (ok)
			{
				fn.CmdResetQueryPool(c->pre[i], c->queries, 2 * i, 2);
				fn.CmdWriteTimestamp(c->pre[i], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, c->queries, 2 * i);
				ok = fn.EndCommandBuffer(c->pre[i]) == VK_SUCCESS;
			}
			if (ok)
				ok = fn.BeginCommandBuffer(c->post[i], &bi) == VK_SUCCESS;
			if (ok)
			{
				fn.CmdWriteTimestamp(c->post[i], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->queries, 2 * i + 1);
				ok = fn.EndCommandBuffer(c->post[i]) == VK_SUCCESS;
			}
		}
		VkCommandBuffer reset = cmds[kSlotsPerChunk * 2];
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if (ok)
			ok = fn.BeginCommandBuffer(reset, &bi) == VK_SUCCESS;
		if (ok)
		{
			fn.CmdResetQueryPool(reset, c->queries, 0, kSlotsPerChunk * 2);
			ok = fn.EndCommandBuffer(reset) == VK_SUCCESS;
		}
		if (ok)
		{
			VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
			si.commandBufferCount = 1;
			si.pCommandBuffers = &reset;
			ok = fn.QueueSubmit(queue, 1, &si, c->resetFence) == VK_SUCCESS;
		}
		if (!ok)
		{
			// The command buffers go with the pool at destruction.
			destroyChunk(*c);
			f.usable = false;
			PML_LOG(LogLevel::Warn, "GPU timing: setting up queue family %u failed, its submissions stay untimed",
			        familyIndex);
			return;
		}
		for (uint32_t i = kSlotsPerChunk; i-- > 0;)
			c->freeSlots.push_back(i);
		f.chunks.push_back(std::move(c));
	}

	// A free slot of the family for a batch about to go to `queue`, or chunk == UINT32_MAX.
	Taken acquire(uint32_t familyIndex, VkQueue queue)
	{
		Taken t;
		if (!probe(familyIndex))
			return t;
		Family &f = families[familyIndex];
		bool settingUp = false;
		for (uint32_t i = 0; i < f.chunks.size(); i++)
		{
			Chunk &c = *f.chunks[i];
			if (!c.ready)
				c.ready = fn.GetFenceStatus(ctx.device, c.resetFence) == VK_SUCCESS;
			if (!c.ready)
				settingUp = true;
			if (c.ready && !c.freeSlots.empty())
			{
				t.chunk = i;
				t.slot = c.freeSlots.back();
				c.freeSlots.pop_back();
				return t;
			}
		}
		// One chunk at a time: while a new chunk's reset is still queued, the batches go untimed.
		if (f.usable && !settingUp && f.chunks.size() < kMaxChunksPerFamily)
			addChunk(familyIndex, queue);  // usable once its reset is done: this batch goes untimed
		return t;
	}

	void release(uint32_t familyIndex, const Taken &t)
	{
		families[familyIndex].chunks[t.chunk]->freeSlots.push_back(t.slot);
	}

	// Reads every finished batch without waiting. Per queue in submission order: a batch starts no
	// earlier than the previous one on its queue ended (TOP_OF_PIPE is written when the command processor
	// reaches it, possibly while earlier work still runs).
	void poll(uint64_t nowNs)
	{
		if (nowNs - lastCalibrationNs > kCalibrateEveryNs)
			calibrate();
		VkQueue blocked[16];
		uint32_t blockedCount = 0;
		auto isBlocked = [&](VkQueue q) { return std::find(blocked, blocked + blockedCount, q) != blocked + blockedCount; };
		for (auto it = inFlight.begin(); it != inFlight.end();)
		{
			if (isBlocked(it->queue))
			{
				++it;
				continue;
			}
			Chunk &c = *families[it->family].chunks[it->chunk];
			uint64_t data[4] = {};
			const VkResult r = fn.GetQueryPoolResults(ctx.device, c.queries, 2 * it->slot, 2, sizeof(data), data,
			                                          2 * sizeof(uint64_t),
			                                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
			// Until the GPU runs this batch's reset, the slot still shows its previous (available) results.
			const bool done = (r == VK_SUCCESS || r == VK_NOT_READY) && data[1] && data[3] &&
			                  data[2] != c.lastRawEnd[it->slot];
			if (!done)
			{
				if (!it->abandoned && nowNs > it->submitNs + kAbandonNs)
				{
					// Waiting on something for seconds (a semaphore of a paused game): its time would belong to no
					// frame. It stops holding up its queue; the slot comes back once the batch has run.
					it->abandoned = true;
					stats.abandoned++;
				}
				if (!it->abandoned && blockedCount < 16)
					blocked[blockedCount++] = it->queue;
				++it;
				continue;
			}
			c.lastRawEnd[it->slot] = data[2];
			c.freeSlots.push_back(it->slot);
			if (it->abandoned)
			{
				it = inFlight.erase(it);
				continue;
			}
			uint64_t start = clock.toMonotonic(data[0]);
			const uint64_t end = clock.toMonotonic(data[2]);
			// Batches of one queue run in order: one starts no earlier than the previous one ended. Only then:
			// a previous end after this one's (a bad conversion) must not shrink every later batch to nothing.
			uint64_t &last = queueLastEnd[it->queue];
			if (end && start < last && last <= end)
				start = last;
			if (end)
				last = end;
			attributor.add({start, end, it->submitNs});
			stats.completed++;
			it = inFlight.erase(it);
		}
	}
};

std::unique_ptr<GpuTimer> GpuTimer::create(const DeviceContext &ctx)
{
	if (const char *env = std::getenv("PRESENTMON_GPU"); env && (!std::strcmp(env, "0") || !std::strcmp(env, "off")))
		return nullptr;
	if (!ctx.setDeviceLoaderData || !ctx.getDeviceProcAddr || !ctx.instanceTable)
		return nullptr;
	const InstanceDispatch &it = *ctx.instanceTable;
	if (!it.GetPhysicalDeviceQueueFamilyProperties || !it.GetPhysicalDeviceProperties ||
	    !it.GetPhysicalDeviceCalibrateableTimeDomainsKHR)
		return nullptr;

	auto impl = std::make_unique<Impl>();
	impl->ctx = ctx;
	Fns &fn = impl->fn;
	bool ok = true;
#define X(name)                                                                                                   \
	fn.name = reinterpret_cast<PFN_vk##name>(ctx.getDeviceProcAddr(ctx.device, "vk" #name));                    \
	ok = ok && fn.name;
	PML_GPU_FUNCTIONS(X)
#undef X
	fn.GetCalibratedTimestamps =
		reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(ctx.getDeviceProcAddr(ctx.device, "vkGetCalibratedTimestampsKHR"));
	if (!fn.GetCalibratedTimestamps)
		fn.GetCalibratedTimestamps = reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(
			ctx.getDeviceProcAddr(ctx.device, "vkGetCalibratedTimestampsEXT"));
	if (!ok || !fn.GetCalibratedTimestamps)
	{
		PML_LOG(LogLevel::Info, "GPU timing unavailable: device functions missing (VK_KHR_calibrated_timestamps?)");
		return nullptr;
	}

	uint32_t domainCount = 0;
	std::vector<VkTimeDomainKHR> domains;
	if (it.GetPhysicalDeviceCalibrateableTimeDomainsKHR(ctx.physicalDevice, &domainCount, nullptr) == VK_SUCCESS)
	{
		domains.resize(domainCount);
		if (it.GetPhysicalDeviceCalibrateableTimeDomainsKHR(ctx.physicalDevice, &domainCount, domains.data()) < 0)
			domains.clear();
		domains.resize(std::min<size_t>(domains.size(), domainCount));
	}
	auto has = [&](VkTimeDomainKHR d) { return std::find(domains.begin(), domains.end(), d) != domains.end(); };
	if (!has(VK_TIME_DOMAIN_DEVICE_KHR) || !has(VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR))
	{
		PML_LOG(LogLevel::Info, "GPU timing unavailable: the device cannot calibrate its timestamps to CLOCK_MONOTONIC");
		return nullptr;
	}

	uint32_t familyCount = 0;
	it.GetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &familyCount, nullptr);
	impl->familyProps.resize(familyCount);
	it.GetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &familyCount, impl->familyProps.data());
	impl->familyProps.resize(familyCount);
	impl->families.resize(familyCount);
	VkPhysicalDeviceProperties props = {};
	it.GetPhysicalDeviceProperties(ctx.physicalDevice, &props);
	impl->periodNs = double(props.limits.timestampPeriod);
	impl->clock.periodNs = impl->periodNs;
	uint32_t validBits = 64;
	for (const auto &p : impl->familyProps)
		if (p.timestampValidBits)
			validBits = std::min(validBits, p.timestampValidBits);
	impl->clock.validBits = validBits;
	if (!(impl->periodNs > 0.0) || !impl->calibrate())
	{
		PML_LOG(LogLevel::Info, "GPU timing unavailable: timestamp calibration failed");
		return nullptr;
	}
	PML_LOG(LogLevel::Debug, "GPU timing on: %u queue families, timestamp period %.3f ns", familyCount, impl->periodNs);
	return std::unique_ptr<GpuTimer>(new GpuTimer(std::move(impl)));
}

GpuTimer::GpuTimer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

GpuTimer::~GpuTimer()
{
	Impl &m = *impl_;
	for (auto &f : m.families)
	{
		for (auto &c : f.chunks)
			m.destroyChunk(*c);
		if (f.pool)
			m.fn.DestroyCommandPool(m.ctx.device, f.pool, nullptr);
	}
	PML_LOG(LogLevel::Debug,
	        "GPU timing: %llu batches timed, %llu untimed, %llu read, %llu abandoned; frames %llu with GPU data, %llu without",
	        (unsigned long long)m.stats.timedBatches, (unsigned long long)m.stats.untimedBatches,
	        (unsigned long long)m.stats.completed, (unsigned long long)m.stats.abandoned,
	        (unsigned long long)m.stats.framesWithGpu, (unsigned long long)m.stats.framesWithoutGpu);
}

VkResult GpuTimer::submit(PFN_vkQueueSubmit next, VkQueue queue, uint32_t family, uint32_t count,
                          const VkSubmitInfo *submits, VkFence fence)
{
	if (count == 0 || !submits)
		return next(queue, count, submits, fence);
	Impl &m = *impl_;
	thread_local std::vector<VkSubmitInfo> infos;
	thread_local std::vector<VkCommandBuffer> cmds;
	thread_local std::vector<Impl::Taken> taken;
	const uint64_t nowNs = pml::monotonicNs();

	taken.assign(count, Impl::Taken{});
	bool any = false;
	{
		std::lock_guard<std::mutex> lock(m.mutex);
		for (uint32_t i = 0; i < count; i++)
		{
			if (timeable(submits[i]))
				taken[i] = m.acquire(family, queue);
			if (taken[i].chunk != UINT32_MAX)
			{
				any = true;
				m.stats.timedBatches++;
			}
			else if (submits[i].commandBufferCount)
			{
				m.stats.untimedBatches++;
			}
		}
	}
	if (!any)
		return next(queue, count, submits, fence);

	size_t total = 0;
	for (uint32_t i = 0; i < count; i++)
		total += submits[i].commandBufferCount + 2;
	cmds.clear();
	cmds.reserve(total);  // no reallocation below: the infos point into it
	infos.assign(submits, submits + count);
	{
		std::lock_guard<std::mutex> lock(m.mutex);
		for (uint32_t i = 0; i < count; i++)
		{
			if (taken[i].chunk == UINT32_MAX)
				continue;
			Impl::Chunk &c = *m.families[family].chunks[taken[i].chunk];
			const size_t first = cmds.size();
			cmds.push_back(c.pre[taken[i].slot]);
			cmds.insert(cmds.end(), submits[i].pCommandBuffers, submits[i].pCommandBuffers + submits[i].commandBufferCount);
			cmds.push_back(c.post[taken[i].slot]);
			infos[i].commandBufferCount = submits[i].commandBufferCount + 2;
			infos[i].pCommandBuffers = cmds.data() + first;
		}
	}

	const VkResult result = next(queue, count, infos.data(), fence);

	std::lock_guard<std::mutex> lock(m.mutex);
	for (uint32_t i = 0; i < count; i++)
	{
		if (taken[i].chunk == UINT32_MAX)
			continue;
		if (result == VK_SUCCESS)
			m.inFlight.push_back({family, taken[i].chunk, taken[i].slot, queue, nowNs});
		else
			m.release(family, taken[i]);
	}
	return result;
}

VkResult GpuTimer::submit2(PFN_vkQueueSubmit2 next, VkQueue queue, uint32_t family, uint32_t count,
                           const VkSubmitInfo2 *submits, VkFence fence)
{
	if (count == 0 || !submits)
		return next(queue, count, submits, fence);
	Impl &m = *impl_;
	thread_local std::vector<VkSubmitInfo2> infos;
	thread_local std::vector<VkCommandBufferSubmitInfo> cmds;
	thread_local std::vector<Impl::Taken> taken;
	const uint64_t nowNs = pml::monotonicNs();

	taken.assign(count, Impl::Taken{});
	bool any = false;
	{
		std::lock_guard<std::mutex> lock(m.mutex);
		for (uint32_t i = 0; i < count; i++)
		{
			if (timeable(submits[i]))
				taken[i] = m.acquire(family, queue);
			if (taken[i].chunk != UINT32_MAX)
			{
				any = true;
				m.stats.timedBatches++;
			}
			else if (submits[i].commandBufferInfoCount)
			{
				m.stats.untimedBatches++;
			}
		}
	}
	if (!any)
		return next(queue, count, submits, fence);

	size_t total = 0;
	for (uint32_t i = 0; i < count; i++)
		total += submits[i].commandBufferInfoCount + 2;
	cmds.clear();
	cmds.reserve(total);
	infos.assign(submits, submits + count);
	{
		std::lock_guard<std::mutex> lock(m.mutex);
		for (uint32_t i = 0; i < count; i++)
		{
			if (taken[i].chunk == UINT32_MAX)
				continue;
			Impl::Chunk &c = *m.families[family].chunks[taken[i].chunk];
			const size_t first = cmds.size();
			VkCommandBufferSubmitInfo ours = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
			ours.commandBuffer = c.pre[taken[i].slot];
			cmds.push_back(ours);
			cmds.insert(cmds.end(), submits[i].pCommandBufferInfos,
			            submits[i].pCommandBufferInfos + submits[i].commandBufferInfoCount);
			ours.commandBuffer = c.post[taken[i].slot];
			cmds.push_back(ours);
			infos[i].commandBufferInfoCount = submits[i].commandBufferInfoCount + 2;
			infos[i].pCommandBufferInfos = cmds.data() + first;
		}
	}

	const VkResult result = next(queue, count, infos.data(), fence);

	std::lock_guard<std::mutex> lock(m.mutex);
	for (uint32_t i = 0; i < count; i++)
	{
		if (taken[i].chunk == UINT32_MAX)
			continue;
		if (result == VK_SUCCESS)
			m.inFlight.push_back({family, taken[i].chunk, taken[i].slot, queue, nowNs});
		else
			m.release(family, taken[i]);
	}
	return result;
}

void GpuTimer::fill(pml::FrameRecord &record)
{
	Impl &m = *impl_;
	std::lock_guard<std::mutex> lock(m.mutex);
	const uint64_t nowNs = pml::monotonicNs();
	m.poll(nowNs);
	if (record.swapchainSerial != m.ownerSerial)
	{
		if (record.readyNs == 0 || (m.ownerSerial != 0 && nowNs < m.ownerLastNs + kOwnerStaleNs))
			return;  // another swapchain of this device gets the GPU numbers
		m.ownerSerial = record.swapchainSerial;
	}
	if (record.readyNs != 0)
		m.ownerLastNs = nowNs;
	if (record.readyNs == 0)
	{
		m.attributor.discardBefore(record.presentEndNs);
		m.stats.framesWithoutGpu++;
		return;
	}
	const GpuFrameWork w = m.attributor.take(record.readyNs, record.presentStartNs);
	if (!w.valid || w.startNs == 0 || w.startNs > record.readyNs)
	{
		m.stats.framesWithoutGpu++;
		return;
	}
	record.gpuStartNs = w.startNs;
	record.gpuBusyNs = std::min(w.busyNs, record.readyNs - w.startNs);
	m.stats.framesWithGpu++;
}

void GpuTimer::forgetSwapchain(uint64_t serial)
{
	std::lock_guard<std::mutex> lock(impl_->mutex);
	if (impl_->ownerSerial == serial)
		impl_->ownerSerial = 0;
}

GpuTimerStats GpuTimer::stats() const
{
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->stats;
}

} // namespace pml::layer
