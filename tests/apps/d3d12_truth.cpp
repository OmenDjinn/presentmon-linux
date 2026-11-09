// SPDX-License-Identifier: MIT
// d3d12_truth: minimal D3D12 flip-model test program for presentmon-linux runs under Wine/Proton + vkd3d-proton.
//
//   d3d12_truth.exe [--seconds S] [--sync 0|1] [--switch-every N] [--fps F] [--log PATH] [--width W] [--height H]
//
// WINDOWED ONLY. Opens a WS_OVERLAPPEDWINDOW window with a W x H client area (default 640x360, at most 1280x720),
// shown without activation, creates a D3D12 device, a direct queue and a 3-buffer DXGI_SWAP_EFFECT_FLIP_DISCARD swap
// chain with DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING and, for S seconds, per frame:
//   1. pumps window messages,
//   2. waits for the frame's deadline (--fps, default 120; spins only for the last 2 ms),
//   3. clears the back buffer to a colour that changes every frame,
//   4. calls Present(sync, sync == 0 ? DXGI_PRESENT_ALLOW_TEARING : 0).
// --switch-every N flips the sync interval between 0 and 1 every N frames, as a game does when V-Sync is toggled.
// vkd3d-proton presents sync 0 frames as VK_PRESENT_MODE_IMMEDIATE_KHR without a present id and sync 1 frames as
// FIFO with one (swapchain.c, dxgi_vk_swap_chain_present_iteration), so both kinds reach the layer in one swapchain.
//
// --log PATH gets one CSV row per frame (no I/O in the loop): frame, sync interval, QueryPerformanceCounter right
// before and right after Present(), its HRESULT. The "# qpf=" header line gives the QPC frequency.
// Exit code 0 on success, 1 on a setup error, 2 when Present() failed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr UINT kBuffers = 3;

struct FrameLog
{
	int64_t presentBegin;
	int64_t presentEnd;
	int32_t hr;
	int32_t sync;
};

bool g_quit = false;

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
	case WM_CLOSE:
		g_quit = true;
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	case WM_KEYDOWN:
		if (wp == VK_ESCAPE)
			g_quit = true;
		return 0;
	default:
		return DefWindowProcW(hwnd, msg, wp, lp);
	}
}

int64_t qpc()
{
	LARGE_INTEGER v;
	QueryPerformanceCounter(&v);
	return v.QuadPart;
}

int fail(const char *what, HRESULT hr)
{
	std::printf("d3d12_truth: %s failed (0x%08lx)\n", what, static_cast<unsigned long>(hr));
	std::fflush(stdout);
	return 1;
}

template <typename T> void release(T *&p)
{
	if (p)
		p->Release();
	p = nullptr;
}

} // namespace

int main(int argc, char **argv)
{
	double seconds = 5.0, fps = 120.0;
	int sync = 0, switchEvery = 0, width = 640, height = 360;
	std::string logPath;
	for (int i = 1; i < argc; i++)
	{
		const std::string a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : nullptr;
		if (a == "--seconds" && v) { seconds = std::atof(v); i++; }
		else if (a == "--sync" && v) { sync = std::atoi(v) ? 1 : 0; i++; }
		else if (a == "--switch-every" && v) { switchEvery = std::atoi(v); i++; }
		else if (a == "--fps" && v) { fps = std::atof(v); i++; }
		else if (a == "--log" && v) { logPath = v; i++; }
		else if (a == "--width" && v) { width = std::atoi(v); i++; }
		else if (a == "--height" && v) { height = std::atoi(v); i++; }
		else
		{
			std::printf("d3d12_truth: unknown argument %s\n", a.c_str());
			return 1;
		}
	}
	width = width < 64 ? 64 : (width > 1280 ? 1280 : width);
	height = height < 64 ? 64 : (height > 720 ? 720 : height);
	if (!(fps > 0.0 && fps <= 500.0))
		fps = 120.0;

	HINSTANCE inst = GetModuleHandleW(nullptr);
	WNDCLASSW wc = {};
	wc.lpfnWndProc = windowProc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
	wc.lpszClassName = L"d3d12_truth";
	RegisterClassW(&wc);
	RECT rc = {0, 0, width, height};
	AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
	HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"d3d12_truth", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
	                            rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
	if (!hwnd)
		return fail("CreateWindowExW", HRESULT(GetLastError()));
	ShowWindow(hwnd, SW_SHOWNOACTIVATE);

	IDXGIFactory5 *factory = nullptr;
	ID3D12Device *device = nullptr;
	ID3D12CommandQueue *queue = nullptr;
	IDXGISwapChain3 *swap = nullptr;
	ID3D12DescriptorHeap *rtvHeap = nullptr;
	ID3D12Resource *buffers[kBuffers] = {};
	ID3D12CommandAllocator *allocs[kBuffers] = {};
	ID3D12GraphicsCommandList *list = nullptr;
	ID3D12Fence *fence = nullptr;
	HRESULT hr;

	if (FAILED(hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory5), reinterpret_cast<void **>(&factory))))
		return fail("CreateDXGIFactory2", hr);
	BOOL tearing = FALSE;
	factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
	if (FAILED(hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
	                                  reinterpret_cast<void **>(&device))))
		return fail("D3D12CreateDevice", hr);
	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	if (FAILED(hr = device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&queue))))
		return fail("CreateCommandQueue", hr);

	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.Width = UINT(width);
	sd.Height = UINT(height);
	sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = kBuffers;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
	IDXGISwapChain1 *swap1 = nullptr;
	if (FAILED(hr = factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &swap1)))
		return fail("CreateSwapChainForHwnd", hr);
	hr = swap1->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void **>(&swap));
	release(swap1);
	if (FAILED(hr))
		return fail("IDXGISwapChain3", hr);

	D3D12_DESCRIPTOR_HEAP_DESC hd = {};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	hd.NumDescriptors = kBuffers;
	if (FAILED(hr = device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void **>(&rtvHeap))))
		return fail("CreateDescriptorHeap", hr);
	const UINT rtvStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	D3D12_CPU_DESCRIPTOR_HANDLE rtvBase = rtvHeap->GetCPUDescriptorHandleForHeapStart();
	for (UINT i = 0; i < kBuffers; i++)
	{
		if (FAILED(hr = swap->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&buffers[i]))))
			return fail("GetBuffer", hr);
		D3D12_CPU_DESCRIPTOR_HANDLE h = rtvBase;
		h.ptr += SIZE_T(i) * rtvStep;
		device->CreateRenderTargetView(buffers[i], nullptr, h);
		if (FAILED(hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
		                                               reinterpret_cast<void **>(&allocs[i]))))
			return fail("CreateCommandAllocator", hr);
	}
	if (FAILED(hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocs[0], nullptr,
	                                          __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&list))))
		return fail("CreateCommandList", hr);
	list->Close();
	if (FAILED(hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&fence))))
		return fail("CreateFence", hr);
	HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	uint64_t fenceValue = 0, bufferFence[kBuffers] = {};
	auto waitFence = [&](uint64_t v) {
		if (fence->GetCompletedValue() < v)
		{
			fence->SetEventOnCompletion(v, fenceEvent);
			WaitForSingleObject(fenceEvent, 5000);
		}
	};

	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	const int64_t period = int64_t(double(freq.QuadPart) / fps);
	const int64_t start = qpc();
	const int64_t end = start + int64_t(seconds * double(freq.QuadPart));
	int64_t deadline = start;
	std::vector<FrameLog> log;
	log.reserve(size_t(seconds * fps * 1.2) + 16);
	int presentFailures = 0;

	for (uint64_t frame = 0; !g_quit; frame++)
	{
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		if (g_quit || qpc() >= end)
			break;
		deadline += period;
		for (int64_t now = qpc(); now < deadline; now = qpc())
		{
			if (deadline - now > freq.QuadPart / 500)
				Sleep(1);
		}
		if (qpc() - deadline > period * 4)
			deadline = qpc();

		const int frameSync = switchEvery > 0 && (frame / uint64_t(switchEvery)) % 2 ? 1 - sync : sync;
		const UINT idx = swap->GetCurrentBackBufferIndex();
		waitFence(bufferFence[idx]);
		allocs[idx]->Reset();
		list->Reset(allocs[idx], nullptr);
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = buffers[idx];
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		list->ResourceBarrier(1, &b);
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvBase;
		rtv.ptr += SIZE_T(idx) * rtvStep;
		const float t = float(frame % 240) / 240.0f;
		const float colour[4] = {0.1f + 0.5f * t, 0.2f, 0.6f - 0.4f * t, 1.0f};
		list->ClearRenderTargetView(rtv, colour, 0, nullptr);
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
		list->ResourceBarrier(1, &b);
		list->Close();
		ID3D12CommandList *lists[] = {list};
		queue->ExecuteCommandLists(1, lists);

		FrameLog f = {};
		f.sync = frameSync;
		f.presentBegin = qpc();
		f.hr = int32_t(swap->Present(UINT(frameSync), frameSync == 0 && tearing ? DXGI_PRESENT_ALLOW_TEARING : 0));
		f.presentEnd = qpc();
		log.push_back(f);
		if (FAILED(f.hr))
			presentFailures++;
		queue->Signal(fence, ++fenceValue);
		bufferFence[idx] = fenceValue;
	}
	waitFence(fenceValue);

	if (!logPath.empty())
	{
		if (FILE *out = std::fopen(logPath.c_str(), "w"))
		{
			std::fprintf(out, "# qpf=%lld tearing=%d\n", static_cast<long long>(freq.QuadPart), tearing ? 1 : 0);
			std::fprintf(out, "frame,sync,present_begin_qpc,present_end_qpc,hr,fs_hr\n");
			for (size_t i = 0; i < log.size(); i++)
				std::fprintf(out, "%zu,%d,%lld,%lld,0x%08lx,0\n", i, log[i].sync, static_cast<long long>(log[i].presentBegin),
				             static_cast<long long>(log[i].presentEnd), static_cast<unsigned long>(uint32_t(log[i].hr)));
			std::fclose(out);
		}
	}
	std::printf("d3d12_truth: %zu frames, %d Present failures, tearing %s\n", log.size(), presentFailures,
	            tearing ? "supported" : "not supported");
	std::fflush(stdout);

	release(fence);
	release(list);
	for (UINT i = 0; i < kBuffers; i++)
	{
		release(allocs[i]);
		release(buffers[i]);
	}
	release(rtvHeap);
	release(swap);
	release(queue);
	release(device);
	release(factory);
	CloseHandle(fenceEvent);
	DestroyWindow(hwnd);
	return presentFailures ? 2 : 0;
}
