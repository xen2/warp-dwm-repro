// DWM crash repro: a D3D12 app on WARP (Microsoft.Direct3D.WARP 1.0.13 or later, app-local
// d3d10warp.dll) presents a flip-model swap chain to a window. On an indirect display (IDD)
// virtual monitor, dwm.exe then crashes with 0xC00001AD in dwmcore.dll about once per second
// while the app runs. The OS WARP and WARP 1.0.12 do not crash.
//
//   warp-dwm-repro [--mode top-level|direct-child|reparent] [--seconds N]
//
// All three window setups crash; the swap chain's window is owned by a render thread:
//   top-level     (default) the swap chain is on the visible top-level window itself
//   direct-child  the render thread creates a child window directly under the visible window
//   reparent      the render thread creates a hidden child window under a hidden parking window,
//                 creates its swap chain and presents a few frames; the main thread then moves it
//                 into the visible window with SetParent and shows it
//
// Build: cl /O2 /EHsc /std:c++17 main.cpp d3d12.lib dxgi.lib user32.lib

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using Microsoft::WRL::ComPtr;

static const int BufferCount = 2;
static ComPtr<ID3D12Device> g_device;
static ComPtr<ID3D12CommandQueue> g_queue;
static ComPtr<ID3D12CommandAllocator> g_allocator;
static ComPtr<ID3D12GraphicsCommandList> g_list;
static ComPtr<ID3D12Fence> g_fence;
static HANDLE g_fenceEvent;
static UINT64 g_fenceValue;

[[noreturn]] static void Fail(const char* what, HRESULT hr)
{
    std::printf("FAILED %s: hr=0x%08X removed-reason=0x%08X\n", what, (unsigned)hr,
        g_device ? (unsigned)g_device->GetDeviceRemovedReason() : 0u);
    std::fflush(stdout);
    std::exit(2);
}

#define CHECK(expr) do { HRESULT hr_ = (expr); if (FAILED(hr_)) Fail(#expr, hr_); } while (0)

static void Log(const char* what)
{
    SYSTEMTIME time;
    GetSystemTime(&time);
    std::printf("%02u:%02u:%02u.%03u UTC %s\n", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, what);
    std::fflush(stdout);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    return message == WM_CLOSE ? 0 : DefWindowProcW(hwnd, message, wParam, lParam);
}

static void PumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// Pumps this thread's messages until the handle is signaled or the timeout expires
static bool PumpUntil(HANDLE handle, DWORD timeout)
{
    ULONGLONG end = GetTickCount64() + timeout;
    for (;;)
    {
        ULONGLONG now = GetTickCount64();
        DWORD wait = timeout == INFINITE ? INFINITE : now >= end ? 0 : (DWORD)(end - now);
        DWORD result = MsgWaitForMultipleObjects(1, &handle, FALSE, wait, QS_ALLINPUT);
        PumpMessages();
        if (result == WAIT_OBJECT_0)
            return true;
        if (result == WAIT_TIMEOUT)
            return false;
    }
}

static void WaitForGpu()
{
    CHECK(g_queue->Signal(g_fence.Get(), ++g_fenceValue));
    if (g_fence->GetCompletedValue() < g_fenceValue)
    {
        CHECK(g_fence->SetEventOnCompletion(g_fenceValue, g_fenceEvent));
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
}

struct SwapChain
{
    HWND hwnd{};
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12Resource> buffers[BufferCount];
    UINT rtvSize{};
    int width{};
    int height{};

    void Create(IDXGIFactory4* factory, HWND window)
    {
        hwnd = window;
        RECT rect;
        GetClientRect(hwnd, &rect);
        width = rect.right;
        height = rect.bottom;

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = BufferCount;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> swapChain1;
        CHECK(factory->CreateSwapChainForHwnd(g_queue.Get(), hwnd, &desc, nullptr, nullptr, &swapChain1));
        CHECK(swapChain1.As(&swapChain));

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = BufferCount;
        CHECK(g_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap)));
        rtvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        CreateViews();
    }

    void CreateViews()
    {
        for (UINT i = 0; i < BufferCount; ++i)
        {
            CHECK(swapChain->GetBuffer(i, IID_PPV_ARGS(&buffers[i])));
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += (SIZE_T)i * rtvSize;
            g_device->CreateRenderTargetView(buffers[i].Get(), nullptr, rtv);
        }
    }

    // Follows the window size, clears and presents one frame
    void Present(UINT64 frame)
    {
        RECT rect;
        GetClientRect(hwnd, &rect);
        if (rect.right > 0 && rect.bottom > 0 && (rect.right != width || rect.bottom != height))
        {
            WaitForGpu();
            for (auto& buffer : buffers)
                buffer.Reset();
            CHECK(swapChain->ResizeBuffers(BufferCount, rect.right, rect.bottom, DXGI_FORMAT_UNKNOWN, 0));
            width = rect.right;
            height = rect.bottom;
            CreateViews();
        }

        UINT index = swapChain->GetCurrentBackBufferIndex();
        CHECK(g_allocator->Reset());
        CHECK(g_list->Reset(g_allocator.Get(), nullptr));
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = buffers[index].Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_list->ResourceBarrier(1, &barrier);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += (SIZE_T)index * rtvSize;
        float color[4] = { (frame % 256) / 255.0f, 0.3f, 0.6f, 1.0f };
        g_list->ClearRenderTargetView(rtv, color, 0, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        g_list->ResourceBarrier(1, &barrier);
        CHECK(g_list->Close());
        ID3D12CommandList* lists[] = { g_list.Get() };
        g_queue->ExecuteCommandLists(1, lists);
        CHECK(swapChain->Present(1, 0));
        WaitForGpu();
    }
};

int main(int argc, char** argv)
{
    const char* mode = "top-level";
    int seconds = 10;
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atoi(argv[++i]);
    }
    bool reparent = !std::strcmp(mode, "reparent");
    bool directChild = !std::strcmp(mode, "direct-child");
    bool topLevel = !std::strcmp(mode, "top-level");
    if (!reparent && !directChild && !topLevel)
    {
        std::printf("unknown mode: %s\n", mode);
        return 1;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    ComPtr<IDXGIFactory4> factory;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> adapter;
    CHECK(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    CHECK(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device)));

    wchar_t warpPath[MAX_PATH] = L"(not loaded)";
    if (HMODULE warp = GetModuleHandleW(L"d3d10warp.dll"))
        GetModuleFileNameW(warp, warpPath, MAX_PATH);
    std::printf("mode: %s, d3d10warp.dll: %ls\n", mode, warpPath);

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK(g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_queue)));
    CHECK(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)));
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_allocator)));
    CHECK(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocator.Get(), nullptr, IID_PPV_ARGS(&g_list)));
    CHECK(g_list->Close());

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"WarpDwmRepro";
    RegisterClassW(&windowClass);
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    HWND top = CreateWindowExW(0, windowClass.lpszClassName, L"WARP DWM repro", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        work.left, work.top, work.right - work.left, work.bottom - work.top, nullptr, nullptr, windowClass.hInstance, nullptr);
    PumpMessages();

    // The render thread owns the swap chain's window, as in an editor hosting a game view
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SwapChain target;
    std::thread render([&]
    {
        HWND hwnd = top;
        if (reparent)
        {
            HWND parking = CreateWindowExW(0, windowClass.lpszClassName, L"Parking", WS_OVERLAPPED,
                0, 0, 300, 300, nullptr, nullptr, windowClass.hInstance, nullptr);
            hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"", WS_CHILD,
                0, 0, 512, 512, parking, nullptr, windowClass.hInstance, nullptr);
        }
        else if (directChild)
        {
            RECT rect;
            GetClientRect(top, &rect);
            hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"", WS_CHILD | WS_VISIBLE,
                0, 0, rect.right, rect.bottom, top, nullptr, windowClass.hInstance, nullptr);
        }
        target.Create(factory.Get(), hwnd);
        UINT64 frame = 0;
        for (; frame < 3; ++frame)
            target.Present(frame);
        Log("swap chain created and presented");
        SetEvent(ready);

        ULONGLONG end = GetTickCount64() + (ULONGLONG)seconds * 1000;
        while (GetTickCount64() < end)
        {
            PumpMessages();
            target.Present(frame++);
        }
        std::printf("presented %llu frames\n", frame);
        std::fflush(stdout);
    });
    PumpUntil(ready, INFINITE);

    if (reparent)
    {
        RECT rect;
        GetClientRect(top, &rect);
        SetParent(target.hwnd, top);
        Log("SetParent done");
        SetWindowPos(target.hwnd, HWND_TOP, 0, 0, rect.right, rect.bottom, SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOZORDER);
        ShowWindow(target.hwnd, SW_SHOWNOACTIVATE);
        Log("child shown");
    }

    PumpUntil(render.native_handle(), INFINITE);
    render.join();
    return 0;
}
