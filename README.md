# GPU Hitch Lab (two DirectX 12 editions)

Portable Windows diagnostic renderer for investigating intermittent frame pacing and DXGI presentation stalls. There are now **two separately downloadable executables**:

- **Baseline — `GpuHitchLab.exe`:** the original minimal test with no NVIDIA SDK calls.
- **NVIDIA Reflex — `GpuHitchLabReflex.exe`:** the same renderer plus real NVIDIA NVAPI Reflex sleep-mode calls and latency markers, with **Off**, **On**, and **On + Boost** modes. This is an early integration, not yet runtime-validated on the target RTX 4070 Super.

Both are diagnostics, not complete captures of the Windows presentation pipeline.

## Download (no compiler needed)

1. Open [Actions > Build Windows x64](https://github.com/sallliisa/gpu-hitch-lab/actions/workflows/build.yml).
2. Open the latest successful run on `main`.
3. Under **Artifacts**, choose one or both:
   - `GpuHitchLab-Baseline-Windows-x64`
   - `GpuHitchLab-Reflex-Windows-x64`
4. Extract each ZIP into a different folder. Run its executable. Windows may show a SmartScreen warning because these executables are not code-signed.

Windows 11 x64 and a DirectX 12 GPU are required. The Reflex edition also requires an NVIDIA GPU and an installed NVIDIA driver with Reflex support. There are no separate compilers, SDKs, or NVAPI DLL downloads needed on your PC; NVAPI functionality is provided by the installed driver.

## Run an unattended experiment

Use PowerShell or Command Prompt in each extracted folder. For an initial A/B comparison, run identical duration, V-Sync, FPS cap, window mode, and system settings.

**Baseline, four hours:**

```powershell
.\GpuHitchLab.exe --vsync off --fps 60 --hitch-ms 30 --borderless --hours 4
```

**Reflex On, four hours:**

```powershell
.\GpuHitchLabReflex.exe --reflex on --vsync off --fps 60 --hitch-ms 30 --borderless --hours 4
```

You can also launch the Reflex executable with `--reflex off` or `--reflex boost` (On + Boost). On + Boost requests higher GPU clocks and may increase power use. The Reflex build defaults to **On**, and **R** cycles Off → On → On + Boost. The baseline build never calls NVIDIA's Reflex APIs.

Options:

| Option | Meaning |
| --- | --- |
| `--vsync on` or `--vsync off` | DXGI Present sync interval 1 or 0 (default: off) |
| `--fps N` | FPS cap; `0` is uncapped (default: 60) |
| `--hitch-ms N` | Interval threshold in milliseconds (default: 30) |
| `--borderless` | Cover the active monitor with a borderless window (useful for testing flip presentation) |
| `--hours N` | Automatically stop after N hours (default: run until closed) |
| `--width N --height N` | Initial windowed size (default: 1280 x 720) |
| `--reflex off`, `--reflex on`, or `--reflex boost` | Reflex edition only (default: on) |
| `--help` | Show command-line help |

Keys: **V** toggles V-Sync, **F11** toggles borderless mode, **R** cycles Reflex mode in the Reflex edition, and **Esc** exits. The window title displays frame count, detected hitch count, recent timings, V-Sync, limiter, and Reflex state when applicable. When inactive or minimized, the renderer pauses and ignores focus/resize transitions to avoid treating Alt-Tab as a graphics hitch.

## Logged data

Logs are written beside the executable under `logs/`, with one `hitches-*.csv` file and one `session-*.txt` file per run. The CSV contains **only detected hitches**, not every frame, so unattended runs use little disk space. A row is written once the corresponding GPU timestamp result is available, usually a few frames later. Closing normally flushes pending events.

CSV columns include:

- `frame_interval_ms`: wall time between the starts of consecutive frames, including any previous Present block, CPU wait and FPS limiter.
- `previous_present_ms`: time spent inside the **previous** frame's `IDXGISwapChain::Present` call; particularly useful for inspecting the source of a frame-interval hitch.
- `present_ms`: CPU time inside the **current** frame's `Present` call; analogous to, but not identical to, CapFrameX's `MsInPresentAPI`.
- `fence_wait_ms`: CPU wait for a reusable back buffer's command allocator.
- `record_submit_ms` / `cpu_work_ms`: CPU work on the current frame, excluding/including Present respectively.
- `gpu_command_ms`: GPU timestamp difference surrounding this app's render commands (a clear and barriers), **not** total GPU Busy or display latency.
- `reason`: whether the interval, Present time, or fence wait exceeded the detection threshold.
- Reflex edition only: `reflex_mode` and `reflex_sleep_ms`, showing which SDK mode was requested and time spent in the real `NvAPI_D3D_Sleep` call. That time can be intentional pacing rather than a GPU stall.

Detection is intentionally simple: the interval exceeds `--hitch-ms`, or a Present call exceeds `max(22.5 ms, 75% of --hitch-ms)`, or a fence wait exceeds `max(12 ms, 50% of --hitch-ms)`. Measurements immediately after focus changes and resizes are excluded. Initial warmup is excluded.

## Reflex implementation notes

The Reflex executable compiles against the [official NVIDIA NVAPI SDK](https://github.com/NVIDIA/nvapi) (MIT-licensed) on GitHub Actions, pinned to a specific SDK commit. It initializes NVAPI and requires successful driver calls for `NvAPI_D3D_SetSleepMode`, `NvAPI_D3D_Sleep`, and `NvAPI_D3D_SetLatencyMarker`. Each rendered frame has simulation, render-submit, and Present markers. The session text file records the driver's reported low-latency state when that query is available; an API failure produces a visible error rather than a fake Reflex success.

For comparison, both binaries use the same existing CPU-based `--fps` limiter; NVAPI's `minimumIntervalUs` is zero. NVIDIA recommends calling its sleep function once per rendered frame, even when Reflex is Off, so **Reflex Off is not identical to the non-NVAPI baseline**. This distinction matters when interpreting differences.

## Important limitations / next steps

- **Independent Flip is not verified.** The app uses a flip-discard swap chain and enables the DXGI tearing flag in V-Sync Off when supported, but Windows decides the actual presentation mode. Borderless fullscreen can help test conditions similar to your games.
- This first renderer performs a tiny GPU workload. It can reveal system-level presentation stalls without game complexity, but cannot yet simulate Cyberpunk's ~13 ms GPU work.
- A 38.42 ms hitch with ~26.55 ms spent in Present and ~13 ms GPU Busy is **consistent with** a presentation-related wait; it does not prove a root cause.
- Planned follow-ups after validating both executables: optional calibrated GPU workload, PresentMon/ETW correlation for actual presentation mode, richer context around hitches, and on-device NVIDIA Reflex verification.

Source is built with GitHub-hosted Windows runners and MSVC, including the NVIDIA SDK checkout. No local build toolchain is required.
