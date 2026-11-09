// SPDX-License-Identifier: MIT
// d3d11_truth: minimal D3D11 flip-model test program for presentmon-linux runs under Wine/Proton + DXVK.
//
//   d3d11_truth.exe [--seconds S] [--sync N] [--cpu-ms MS] [--fps F] [--buffers 2|3] [--log PATH]
//                   [--width W] [--height H] [--framestats] [--allow-uncapped] [--colour R,G,B]
//
// WINDOWED ONLY. Opens a WS_OVERLAPPEDWINDOW window with a W x H client area (default and maximum 1280x720),
// shown without activation, creates a D3D11 device and a flip-model swap chain (DXGI_SWAP_EFFECT_FLIP_DISCARD,
// 2-3 buffers) and, for S seconds, per frame:
//   1. pumps window messages,
//   2. waits for the frame's deadline when a frame cap is set (--fps; NtDelayExecution plus a short spin),
//   3. spins the CPU for --cpu-ms milliseconds (the "game work"),
//   4. clears the back buffer to a colour that changes every frame (or to the fixed --colour, 0-1 each),
//   5. calls Present(sync, 0) and, with --framestats, IDXGISwapChain::GetFrameStatistics.
// Sync interval 0 without a cap of at most 500 fps is refused unless --allow-uncapped is given.
//
// The game thread's ground truth is kept in memory and written to --log at exit (no I/O in the loop): one row per
// frame with QueryPerformanceCounter at the loop top, after the frame-cap wait (work start), right before Present()
// and right after it. The header holds the QPC frequency and the tightest QPC / NtQueryPerformanceCounter pairs taken
// at start and end (under Wine, NtQueryPerformanceCounter is CLOCK_MONOTONIC_RAW in 100 ns units), so the log can be
// put on the Linux CLOCK_MONOTONIC timeline of the presentmon-linux layer.
// The game thread is named "pml-game" (SetThreadDescription; Wine forwards it to /proc/<pid>/task/<tid>/comm).
// GUI subsystem (no console is ever allocated). A short summary goes to <log>.txt and to stdout (when Wine passes
// a standard output handle). Exit code 0 on success, 1 on a setup error, 2 when Present() failed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct FrameLog
{
	int64_t loopTop;       // QPC at the top of the loop iteration (before message pump and cap wait)
	int64_t workStart;     // QPC after the frame-cap wait: start of the frame's work
	int64_t presentBegin;  // QPC right before IDXGISwapChain::Present
	int64_t presentEnd;    // QPC right after it returned
	int32_t hr;            // Present's HRESULT
	int32_t fsHr;          // GetFrameStatistics' HRESULT (0 when not called)
	uint32_t fsPresentCount;
	uint32_t fsPresentRefreshCount;
	uint32_t fsSyncRefreshCount;
	uint32_t pad;
	int64_t fsSyncQpc;
};

bool g_quit = false;
std::string g_summaryPath;

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

using NtDelayExecutionFn = LONG(NTAPI *)(BOOLEAN, PLARGE_INTEGER);
using NtQueryPerformanceCounterFn = LONG(NTAPI *)(PLARGE_INTEGER, PLARGE_INTEGER);
NtDelayExecutionFn g_ntDelay = nullptr;
NtQueryPerformanceCounterFn g_ntQpc = nullptr;

void loadNtdll()
{
	if (HMODULE m = GetModuleHandleW(L"ntdll.dll"))
	{
		g_ntDelay = reinterpret_cast<NtDelayExecutionFn>(reinterpret_cast<void *>(GetProcAddress(m, "NtDelayExecution")));
		g_ntQpc = reinterpret_cast<NtQueryPerformanceCounterFn>(
		    reinterpret_cast<void *>(GetProcAddress(m, "NtQueryPerformanceCounter")));
	}
}

// Tightest of 200 brackets qpc() / NtQueryPerformanceCounter / qpc(): returns the midpoint QPC and the Nt value.
struct ClockPair
{
	int64_t qpc = 0, nt = 0, bracket = 0;
};

ClockPair clockPair()
{
	ClockPair best;
	best.bracket = INT64_MAX;
	if (!g_ntQpc)
		return ClockPair{};
	for (int i = 0; i < 200; i++)
	{
		LARGE_INTEGER nt, f;
		const int64_t a = qpc();
		g_ntQpc(&nt, &f);
		const int64_t b = qpc();
		if (b - a < best.bracket)
			best = ClockPair{a + (b - a) / 2, nt.QuadPart, b - a};
	}
	return best;
}

// Sleeps until QPC reaches `deadline`: NtDelayExecution (100 ns units, sub-millisecond under Wine) for all but the
// last 0.4 ms, then spins.
void waitUntil(int64_t deadline, double qpf)
{
	const int64_t spinTicks = int64_t(0.4e-3 * qpf);
	for (;;)
	{
		const int64_t now = qpc();
		const int64_t remaining = deadline - now;
		if (remaining <= 0)
			return;
		if (remaining > spinTicks)
		{
			const int64_t sleep100ns = int64_t(double(remaining - spinTicks) * 1.0e7 / qpf);
			if (g_ntDelay && sleep100ns > 0)
			{
				LARGE_INTEGER rel;
				rel.QuadPart = -sleep100ns;
				g_ntDelay(FALSE, &rel);
			}
			else
				Sleep(DWORD(std::max<int64_t>(sleep100ns / 10000, 0)));
		}
		else
			YieldProcessor();
	}
}

template <class T>
void release(T *&p)
{
	if (p)
		p->Release();
	p = nullptr;
}

// Names the calling thread; Wine also sets the Linux thread name (comm, 15 characters).
void nameThisThread(const wchar_t *name)
{
	using Fn = HRESULT(WINAPI *)(HANDLE, PCWSTR);
	for (const wchar_t *dll : {L"kernelbase.dll", L"kernel32.dll"})
		if (HMODULE m = GetModuleHandleW(dll))
			if (auto fn = reinterpret_cast<Fn>(reinterpret_cast<void *>(GetProcAddress(m, "SetThreadDescription"))))
			{
				fn(GetCurrentThread(), name);
				return;
			}
}

double percentile(std::vector<double> v, double p)
{
	if (v.empty())
		return NAN;
	std::sort(v.begin(), v.end());
	const double idx = p * double(v.size() - 1);
	const size_t lo = size_t(idx), hi = std::min(lo + 1, v.size() - 1);
	return v[lo] + (v[hi] - v[lo]) * (idx - double(lo));
}

// Message to stderr and to the summary file.
void report(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	std::fputs(buf, stdout);
	std::fflush(stdout);
	if (!g_summaryPath.empty())
		if (FILE *f = std::fopen(g_summaryPath.c_str(), "a"))
		{
			std::fputs(buf, f);
			std::fclose(f);
		}
}

struct Options
{
	double seconds = 10.0;
	UINT sync = 1;
	double cpuMs = 0.0;
	double fps = 0.0;
	UINT buffers = 2;
	UINT width = 1280, height = 720;
	bool frameStats = false;
	bool allowUncapped = false;
	bool fixedColour = false;
	float colour[3] = {0, 0, 0};
	std::string log = "d3d11_truth_log.csv";
};

bool parseOptions(int argc, char **argv, Options *o)
{
	for (int i = 1; i < argc; i++)
	{
		const std::string a = argv[i];
		auto value = [&]() -> const char * { return i + 1 < argc ? argv[++i] : nullptr; };
		const char *v = nullptr;
		if (a == "--framestats")
			o->frameStats = true;
		else if (a == "--allow-uncapped")
			o->allowUncapped = true;
		else if (a == "--colour" && (v = value()) != nullptr)
		{
			if (std::sscanf(v, "%f,%f,%f", &o->colour[0], &o->colour[1], &o->colour[2]) != 3)
				return false;
			for (float c : o->colour)
				if (c < 0.0f || c > 1.0f)
					return false;
			o->fixedColour = true;
		}
		else if ((a == "--seconds" || a == "--sync" || a == "--cpu-ms" || a == "--fps" || a == "--buffers" ||
		          a == "--log" || a == "--width" || a == "--height") &&
		         (v = value()) != nullptr)
		{
			if (a == "--seconds")
				o->seconds = std::atof(v);
			else if (a == "--sync")
				o->sync = UINT(std::atoi(v));
			else if (a == "--cpu-ms")
				o->cpuMs = std::atof(v);
			else if (a == "--fps")
				o->fps = std::atof(v);
			else if (a == "--buffers")
				o->buffers = UINT(std::atoi(v));
			else if (a == "--log")
				o->log = v;
			else if (a == "--width")
				o->width = UINT(std::atoi(v));
			else if (a == "--height")
				o->height = UINT(std::atoi(v));
		}
		else
			return false;
	}
	return o->seconds > 0 && o->seconds <= 120 && o->sync <= 4 && o->cpuMs >= 0 && o->cpuMs <= 50 && o->fps >= 0 &&
	       o->buffers >= 2 && o->buffers <= 3 && o->width >= 64 && o->width <= 1280 && o->height >= 64 &&
	       o->height <= 720;
}

} // namespace

int main(int argc, char **argv)
{
	Options opt;
	if (!parseOptions(argc, argv, &opt))
	{
		report("usage: d3d11_truth.exe [--seconds S<=120] [--sync 0-4] [--cpu-ms MS<=50] [--fps F] [--buffers 2|3] "
		       "[--log PATH] [--width W<=1280] [--height H<=720] [--framestats] [--allow-uncapped] [--colour R,G,B]\n");
		return 1;
	}
	g_summaryPath = opt.log + ".txt";
	std::remove(g_summaryPath.c_str());
	if (opt.sync == 0 && (opt.fps <= 0 || opt.fps > 500) && !opt.allowUncapped)
	{
		report("d3d11_truth: sync interval 0 needs --fps 1..500 (or --allow-uncapped)\n");
		return 1;
	}
	nameThisThread(L"pml-game");
	loadNtdll();

	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	const double qpf = double(freq.QuadPart);
	const ClockPair pairStart = clockPair();

	// Window: windowed only, shown without activation.
	HINSTANCE inst = GetModuleHandleW(nullptr);
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = windowProc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = L"d3d11_truth";
	RegisterClassExW(&wc);

	const DWORD style = WS_OVERLAPPEDWINDOW;
	RECT rc = {0, 0, LONG(opt.width), LONG(opt.height)};
	AdjustWindowRect(&rc, style, FALSE);
	HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"d3d11_truth", style, CW_USEDEFAULT, CW_USEDEFAULT,
	                            rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
	if (!hwnd)
	{
		report("d3d11_truth: CreateWindowEx failed (%lu)\n", GetLastError());
		return 1;
	}
	ShowWindow(hwnd, SW_SHOWNOACTIVATE);
	UpdateWindow(hwnd);
	GetClientRect(hwnd, &rc);
	const UINT width = UINT(std::max<LONG>(rc.right - rc.left, 1)), height = UINT(std::max<LONG>(rc.bottom - rc.top, 1));

	// Device and flip-model swap chain.
	ID3D11Device *device = nullptr;
	ID3D11DeviceContext *context = nullptr;
	const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	D3D_FEATURE_LEVEL got = {};
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
	                               2, D3D11_SDK_VERSION, &device, &got, &context);
	if (FAILED(hr))
	{
		report("d3d11_truth: D3D11CreateDevice failed (0x%08lx)\n", (unsigned long)hr);
		return 1;
	}
	IDXGIDevice1 *dxgiDevice = nullptr;
	IDXGIAdapter *adapter = nullptr;
	IDXGIFactory2 *factory = nullptr;
	device->QueryInterface(__uuidof(IDXGIDevice1), reinterpret_cast<void **>(&dxgiDevice));
	if (!dxgiDevice || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
	    FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory))))
	{
		report("d3d11_truth: no IDXGIFactory2\n");
		return 1;
	}
	DXGI_ADAPTER_DESC adesc = {};
	adapter->GetDesc(&adesc);
	UINT maxLatency = 0;
	dxgiDevice->GetMaximumFrameLatency(&maxLatency);

	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.Width = width;
	sd.Height = height;
	sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = opt.buffers;
	sd.Scaling = DXGI_SCALING_STRETCH;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
	IDXGISwapChain1 *swapchain = nullptr;
	hr = factory->CreateSwapChainForHwnd(device, hwnd, &sd, nullptr, nullptr, &swapchain);  // windowed: no fullscreen desc
	if (FAILED(hr))
	{
		report("d3d11_truth: CreateSwapChainForHwnd failed (0x%08lx)\n", (unsigned long)hr);
		return 1;
	}
	factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
	ID3D11Texture2D *backBuffer = nullptr;
	ID3D11RenderTargetView *rtv = nullptr;
	if (FAILED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&backBuffer))) ||
	    FAILED(device->CreateRenderTargetView(backBuffer, nullptr, &rtv)))
	{
		report("d3d11_truth: no render target view\n");
		return 1;
	}

	// Frame loop.
	std::vector<FrameLog> log;
	log.reserve(size_t(std::min(opt.seconds * 2000.0 + 1000.0, 1.0e6)));
	const int64_t cpuTicks = int64_t(opt.cpuMs * qpf / 1000.0);
	const int64_t runTicks = int64_t(opt.seconds * qpf);
	const int64_t periodTicks = opt.fps > 0 ? int64_t(qpf / opt.fps) : 0;
	const int64_t t0 = qpc();
	int64_t deadline = t0;
	HRESULT firstFailure = S_OK;
	uint64_t frame = 0, foregroundSamples = 0, foregroundHits = 0;
	while (!g_quit)
	{
		const int64_t loopTop = qpc();
		if (loopTop - t0 >= runTicks)
			break;
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
				g_quit = true;
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		if (g_quit)
			break;

		// Frame cap: one deadline per frame; resynchronise after falling more than a frame behind.
		if (periodTicks > 0)
		{
			if (frame > 0)
				deadline += periodTicks;
			const int64_t now = qpc();
			if (deadline < now - periodTicks)
				deadline = now;
			waitUntil(deadline, qpf);
		}
		const int64_t workStart = qpc();

		// "Game work": spin on the CPU.
		while (qpc() - workStart < cpuTicks)
		{
		}

		// A colour that changes every frame (hue wheel, one turn per 240 frames), or the fixed --colour.
		const float phase = float(frame % 240) / 240.0f * 6.2831853f;
		float colour[4] = {0.5f + 0.5f * std::sin(phase), 0.5f + 0.5f * std::sin(phase + 2.094f),
		                   0.5f + 0.5f * std::sin(phase + 4.189f), 1.0f};
		if (opt.fixedColour)
		{
			colour[0] = opt.colour[0];
			colour[1] = opt.colour[1];
			colour[2] = opt.colour[2];
		}
		context->OMSetRenderTargets(1, &rtv, nullptr);
		context->ClearRenderTargetView(rtv, colour);

		const int64_t before = qpc();
		hr = swapchain->Present(opt.sync, 0);
		const int64_t after = qpc();
		FrameLog e = {loopTop, workStart, before, after, int32_t(hr), 0, 0, 0, 0, 0, 0};
		if (opt.frameStats)
		{
			DXGI_FRAME_STATISTICS st = {};
			e.fsHr = int32_t(swapchain->GetFrameStatistics(&st));
			e.fsPresentCount = st.PresentCount;
			e.fsPresentRefreshCount = st.PresentRefreshCount;
			e.fsSyncRefreshCount = st.SyncRefreshCount;
			e.fsSyncQpc = st.SyncQPCTime.QuadPart;
		}
		log.push_back(e);
		if (FAILED(hr) && firstFailure == S_OK)
			firstFailure = hr;
		if ((frame & 63) == 0)
		{
			foregroundSamples++;
			if (GetForegroundWindow() == hwnd)
				foregroundHits++;
		}
		frame++;
	}
	const int64_t t1 = qpc();
	const ClockPair pairEnd = clockPair();
	RECT wr = {};
	GetWindowRect(hwnd, &wr);

	// Ground truth log.
	if (FILE *f = std::fopen(opt.log.c_str(), "w"))
	{
		char name[128] = {};
		WideCharToMultiByte(CP_UTF8, 0, adesc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
		std::fprintf(f, "# d3d11_truth 2\n# qpf=%lld\n", (long long)freq.QuadPart);
		std::fprintf(f, "# seconds=%.3f sync_interval=%u cpu_ms=%.3f fps_cap=%.3f buffers=%u width=%u height=%u framestats=%d\n",
		             opt.seconds, opt.sync, opt.cpuMs, opt.fps, opt.buffers, width, height, int(opt.frameStats));
		std::fprintf(f, "# adapter=%s feature_level=0x%x max_frame_latency=%u win_tid=%lu win_pid=%lu\n", name,
		             unsigned(got), maxLatency, GetCurrentThreadId(), GetCurrentProcessId());
		std::fprintf(f, "# clock_pair_start qpc=%lld ntqpc=%lld bracket=%lld\n", (long long)pairStart.qpc,
		             (long long)pairStart.nt, (long long)pairStart.bracket);
		std::fprintf(f, "# clock_pair_end qpc=%lld ntqpc=%lld bracket=%lld\n", (long long)pairEnd.qpc,
		             (long long)pairEnd.nt, (long long)pairEnd.bracket);
		std::fprintf(f, "# window_rect=%ld,%ld,%ld,%ld foreground_samples=%llu foreground_hits=%llu\n", wr.left, wr.top,
		             wr.right, wr.bottom, (unsigned long long)foregroundSamples, (unsigned long long)foregroundHits);
		std::fprintf(f, "# t0_qpc=%lld t1_qpc=%lld frames=%llu\n", (long long)t0, (long long)t1, (unsigned long long)frame);
		std::fprintf(f, "frame,loop_top_qpc,work_start_qpc,present_begin_qpc,present_end_qpc,hr,fs_hr,fs_present_count,"
		                "fs_present_refresh_count,fs_sync_refresh_count,fs_sync_qpc\n");
		for (size_t i = 0; i < log.size(); i++)
		{
			const FrameLog &l = log[i];
			std::fprintf(f, "%zu,%lld,%lld,%lld,%lld,0x%08lx,0x%08lx,%u,%u,%u,%lld\n", i + 1, (long long)l.loopTop,
			             (long long)l.workStart, (long long)l.presentBegin, (long long)l.presentEnd,
			             (unsigned long)uint32_t(l.hr), (unsigned long)uint32_t(l.fsHr), l.fsPresentCount,
			             l.fsPresentRefreshCount, l.fsSyncRefreshCount, (long long)l.fsSyncQpc);
		}
		std::fclose(f);
	}
	else
		report("d3d11_truth: cannot write %s\n", opt.log.c_str());

	// Summary.
	std::vector<double> inPresent, interval, busy, work;
	for (size_t i = 0; i < log.size(); i++)
	{
		inPresent.push_back(double(log[i].presentEnd - log[i].presentBegin) * 1000.0 / qpf);
		work.push_back(double(log[i].presentBegin - log[i].workStart) * 1000.0 / qpf);
		if (i > 0)
		{
			interval.push_back(double(log[i].presentBegin - log[i - 1].presentBegin) * 1000.0 / qpf);
			busy.push_back(double(log[i].presentBegin - log[i - 1].presentEnd) * 1000.0 / qpf);
		}
	}
	report("d3d11_truth: %llu frames in %.3f s (%.1f fps), sync %u, cpu %.2f ms, cap %.1f fps, %ux%u, %u buffers, max "
	       "latency %u, foreground %llu/%llu\n",
	       (unsigned long long)frame, double(t1 - t0) / qpf, double(frame) * qpf / double(std::max<int64_t>(t1 - t0, 1)),
	       opt.sync, opt.cpuMs, opt.fps, width, height, opt.buffers, maxLatency, (unsigned long long)foregroundHits,
	       (unsigned long long)foregroundSamples);
	report("d3d11_truth: interval median %.3f p95 %.3f ms; in Present median %.3f p95 %.3f ms; busy (present end to "
	       "next present) median %.3f ms; work median %.3f ms; first failure 0x%08lx\n",
	       percentile(interval, 0.5), percentile(interval, 0.95), percentile(inPresent, 0.5), percentile(inPresent, 0.95),
	       percentile(busy, 0.5), percentile(work, 0.5), (unsigned long)uint32_t(firstFailure));

	release(rtv);
	release(backBuffer);
	release(swapchain);
	release(factory);
	release(adapter);
	release(dxgiDevice);
	if (context)
		context->ClearState();
	release(context);
	release(device);
	if (!g_quit)
		DestroyWindow(hwnd);
	return firstFailure == S_OK ? 0 : 2;
}
