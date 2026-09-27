# DWM crashes when a D3D12 app on WARP 1.0.13+ reparents its swap chain window (IDD display)

A D3D12 app renders with the app-local WARP from the `Microsoft.Direct3D.WARP` NuGet package, and presents a flip-model swap chain. When the swap chain's window is moved into another window with `SetParent`, `dwm.exe` crashes about once per second for as long as the app runs, whenever the display is an indirect display (IDD) virtual monitor.

- **First bad version:** WARP 1.0.13 (`1.0.13.0.20240822.1`). Every release since then, up to 1.0.21 (`1.0.21.0.20260922.2`), crashes.
- **Last good version:** WARP 1.0.12 (`1.0.12.0.20240517.3`).
- **The OS WARP never crashes.** Tested with `10.0.26100.33438` on Windows Server 2025.
- **The crash loop stops when the app exits.**

We found this in a game editor that hosts its game view this way: the game thread creates the view's window and swap chain, and the UI thread then moves the window into the editor window. Our CI renders with a pinned WARP, so that screenshots don't depend on the machine.

## Crash

- **Process:** `dwm.exe` 10.0.26100.7309, faulting module `dwmcore.dll` 10.0.26100.33438.
- **Exception code:** `0xC00001AD` (fatal memory exhaustion). The failure bucket is `OOM_c00001ad_dwm.exe!out_of_memory`.
- **Stack:** `dwmcore!CD3DDevice::CreateShaderResourceView`, under `CWarpRenderingEffect`.
- **Not a real memory shortage.** In the full dump, DWM's own WARP device was already lost, with `E_OUTOFMEMORY` stored on it. `CreateShaderResourceView` then refuses, and DWM terminates. System commit and DWM's private memory stay low, and GPU memory counters stay flat.
- **Effect on apps:** WPF apps on the same desktop fail with `UCEERR_RENDERTHREADFAILURE` (`0x88980406`). `IDXGISwapChain::Present` in the app starts to return `E_INVALIDARG`, while the device status stays normal.

## Repro

[`main.cpp`](main.cpp) creates a D3D12 device on the WARP adapter (`IDXGIFactory4::EnumWarpAdapter`). It presents a `DXGI_SWAP_EFFECT_FLIP_DISCARD` swap chain with 2 `B8G8R8A8_UNORM` buffers, clearing with `ClearRenderTargetView` and presenting with `Present(1, 0)`. The swap chain's window is owned by a render thread. The `--mode` option picks the window setup:

| `--mode` | Window setup |
|---|---|
| `reparent` (default) | The render thread creates a hidden child window under a hidden "parking" window, creates the swap chain, and presents 3 frames. The main thread then calls `SetParent` to move the child into the visible top-level window, and shows it. |
| `direct-child` | The render thread creates the child directly under the visible top-level window. |
| `top-level` | The swap chain is on the visible top-level window itself. |

### Run it on GitHub Actions

[`.github/workflows/repro.yml`](.github/workflows/repro.yml) runs on the `windows-2025` runner, which has no GPU. It:
1. installs the Amyuni `usbmmidd_v2` IDD virtual monitor at 3840x2160, and disables the Hyper-V display so the virtual monitor is the only one;
2. builds `main.cpp`;
3. runs each mode for 10 seconds with each of three WARPs: the OS WARP, a good NuGet WARP and a bad one. Run the workflow manually to choose the versions (`good-warp`, default 1.0.12; `bad-warp`, default 1.0.21); a push uses the defaults;
4. counts the DWM crashes in the Application event log;
5. uploads the results table and a full `dwm.exe` dump.

### Run it locally

1. Use a machine or VM whose display is an IDD virtual monitor. The workflow's install step works on any Windows machine.
2. Build: `cl /O2 /EHsc /std:c++17 main.cpp d3d12.lib dxgi.lib user32.lib`
3. Put `d3d10warp.dll` from `Microsoft.Direct3D.WARP` 1.0.13 or later (`build\native\bin\x64`) next to the exe.
4. Run `warp-dwm-repro.exe --mode reparent`.
5. Check the Application event log for `Application Error` events from `dwm.exe`.

## Results

Windows Server 2025 (`windows-2025` runner), Amyuni IDD at 3840x2160, `--mode reparent`, 10 seconds per WARP version. "After exit" counts DWM crashes in the 8 seconds after the app exited.

| WARP | DWM crashes | After exit |
|---|---|---|
| OS (`10.0.26100.33438`) | 0 | 0 |
| 1.0.12 | 0 | 0 |
| 1.0.13 | 6 | 0 |
| 1.0.14, 1.0.14.1, 1.0.14.2 | 7, 6, 6 | 0 |
| 1.0.15, 1.0.15.1 | 6, 6 | 0 |
| 1.0.16, 1.0.16.1 | 6, 6 | 0 |
| 1.0.17, 1.0.18, 1.0.19 | 6, 7, 7 | 0 |
| 1.0.20 | 6 | 0 |
| 1.0.21 | 7 | 0 |

With the stock Hyper-V display instead of an IDD, DWM didn't crash. That was tested in the editor, with WARP 1.0.20.
