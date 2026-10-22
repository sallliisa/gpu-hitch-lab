# GPU Hitch Lab (minimal DirectX 12 build)

Portable Windows diagnostic renderer for investigating intermittent frame pacing and DXGI presentation stalls. The current build is a **minimal experiment**, not a complete capture of the Windows presentation pipeline. NVIDIA Reflex is planned for a later phase and **is not implemented yet**.

## Download (no compiler needed)

1. Open [Actions > Build Windows x64](https://github.com/sallliisa/gpu-hitch-lab/actions/workflows/build.yml).
2. Open the latest successful run on `main`.
3. Under **Artifacts**, download `GpuHitchLab-Windows-x64` and extract it.
4. Run `GpuHitchLab.exe`. Windows may show a SmartScreen warning because the executable is not code-signed.

Windows 11 x64 and a DirectX 12 GPU are required. There are no separate installers, SDKs, compilers, or NVIDIA libraries to download on your PC.

## Run an unattended experiment

Use PowerShell or Command Prompt in the extracted folder:

```powershell
.\GpuHitchLab.exe --vsync off --fps 60 --hitch-ms 30 --borderless --hours 4
```

Options:

| Option | Meaning |
| --- | --- |
| `--vsync on` or `--vsync off` | DXGI Present sync interval 1 or 0 (default: off) |
| `--fps N` | FPS cap; `0` is uncapped (default: 60) |
| `--hitch-ms N` | Interval threshold in milliseconds (default: 30) |
| `--borderless` | Cover the active monitor with a borderless window (useful for testing flip presentation) |
| `--hours N` | Automatically stop after N hours (default: run until closed) |
| `--width N --height N` | Initial windowed size (default: 1280 x 720) |
| `--help` | Show command-line help |

Keys: **V** toggles V-Sync, **F11** toggles borderless mode, **Esc** exits. The window title displays frame count, detected hitch count, recent timings, V-Sync and limiter settings. When inactive or minimized, the renderer pauses and ignores focus/resize transitions to avoid treating Alt-Tab as a graphics hitch.

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

Detection is intentionally simple: the interval exceeds `--hitch-ms`, or a Present call exceeds `max(22.5 ms, 75% of --hitch-ms)`, or a fence wait exceeds `max(12 ms, 50% of --hitch-ms)`. Measurements immediately after focus changes and resizes are excluded. Initial warmup is excluded.

## Important limitations / next steps

- **Independent Flip is not verified.** The app uses a flip-discard swap chain and enables the DXGI tearing flag in V-Sync Off when supported, but Windows decides the actual presentation mode. Borderless fullscreen can help test conditions similar to your games.
- This first renderer performs a tiny GPU workload. It can reveal system-level presentation stalls without game complexity, but cannot yet simulate Cyberpunk's ~13 ms GPU work.
- A 38.42 ms hitch with ~26.55 ms spent in Present and ~13 ms GPU Busy is **consistent with** a presentation-related wait; it does not prove a root cause.
- Planned follow-ups after a successful baseline test: optional calibrated GPU workload, PresentMon/ETW correlation for actual presentation mode, richer context around hitches, and **real NVIDIA Reflex SDK** integration (Off / On / On + Boost).

Source is built with GitHub-hosted Windows runners and MSVC. No local build toolchain is required.
