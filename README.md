# GPU Hitch Lab — Baseline and NVIDIA Reflex diagnostics

Portable Windows 11 x64 / DirectX 12 test software for intermittent presentation and synchronization stalls. Source builds entirely on **GitHub Actions**; no local compiler, Visual Studio, or SDK installation is needed.

## Download

Open [Windows Builds](https://github.com/sallliisa/gpu-hitch-lab/actions/workflows/build.yml), open the latest **successful** run on `main`, and download **both** independently built ZIP artifacts:

- `GpuHitchLab-Baseline-Windows-x64` — DX12 without NVIDIA Reflex.
- `GpuHitchLab-Reflex-Windows-x64` — same renderer plus the **real NVAPI Reflex SDK** calls, Off / On / On+Boost (default On).

Extract ZIPs to **separate folders** so their logs cannot be mixed. The official standalone **Intel PresentMon 2.6.0 console** is included in each ZIP, downloaded in Actions with a pinned SHA-256. Windows may warn that our unsigned DX12 binaries are unrecognized.

## Recommended: one-click full tracing

**Double-click `Run-Diagnostics.cmd`**, approve the Windows administrator prompt, and leave the screen awake and the renderer foregrounded. The default supervised run is **30 minutes, 60 FPS, V-Sync Off, borderless**, with a 30 ms hitch threshold. Both Baseline and Reflex ZIPs have the same diagnostic launcher.

The launcher automatically:
1. Starts Windows Performance Recorder (WPR) **CPU.light + GPU** in in-memory circular mode (when possible).
2. Starts our selected DX12 test app; it logs **every frame**, plus a small hitch-only CSV.
3. Starts the **standalone PresentMon console** attached to our exact application PID, to record actual presentation mode, timing, and frame activity.
4. On a detected hitch, waits 10 seconds to include post-event data, exports the WPR buffer to an ETL file, then restarts circular tracing after a short cooldown.
5. Monitors an **approximately 2 GiB run budget**, stopping external collectors when near the budget. The renderer also caps its own frame CSV to 384 MiB. Saving an ETL can temporarily exceed the budget, in which case the latest oversized ETL is discarded.

**Important limitations:** Circular ETW buffer retention depends on event rate, so the script cannot guarantee exactly 10 seconds *before* every hitch. WPR and PresentMon are separate ETW consumers; if Windows refuses one session or they conflict, the console and `trace-notes.txt` report it. ETW trace exporting can itself cause hitches, which may appear in CSVs; capture/restart intervals are logged and a 15-second cooldown reduces retriggering. This integration is compiled/packaged by CI but needs a runtime test on your PC before we can call it validated.

For custom duration or settings, open **PowerShell as Administrator** in the extracted folder, then run:

```powershell
.\Launch-Diagnostics.ps1 -Hours 0 -Minutes 15 -Seconds 30 -Fps 60 -Vsync off -HitchMs 30 -Reflex on
```

Parameters: `-Hours`, `-Minutes`, `-Seconds` (add together), `-Fps`, `-Vsync off|on`, `-Reflex off|on|boost` (only used for Reflex binary), `-HitchMs`, `-BudgetMB` (128–2048), `-PostHitchSeconds`, `-SkipEtw`, `-SkipPresentMon`, and `-Windowed`. To investigate PowerShell issues, review `logs/diagnostics-*/trace-notes.txt`.

## Direct app run (no admin, basic logging)

You can run either executable directly, without external tracing. Examples:

```powershell
.\GpuHitchLab.exe --vsync off --fps 60 --hitch-ms 30 --borderless --minutes 15 --seconds 30
.\GpuHitchLabReflex.exe --reflex on --vsync off --fps 60 --hitch-ms 30 --borderless --hours 4
```

Use only the executable present in your extracted ZIP. `--hours`, `--minutes`, and `--seconds` are additive; all zero means run until closed. `--max-log-mb N` sets the frame CSV limit (16–2048; default 384). Other options: `--width`, `--height`, `--help`. Hotkeys: **V** toggles V-Sync; **F11** toggles borderless; **R** cycles Reflex Off / On / On+Boost (Reflex edition only); **Esc** quits.

## Output files

Alongside the executable in `logs/`:

- `frames-<time>.csv`: **each completed frame**, kept in buffered CSV for low overhead.
- `hitches-<time>.csv`: only detected hitches (flushed immediately).
- `session-<time>.txt`: adapter, test configuration, Reflex driver state when available, window/focus/display/power events.

When supervised tracing runs, `logs/diagnostics-<time>/` also contains:

- `presentmon-all-frames.csv`: independent PresentMon per-frame data with GPU Busy/Wait and actual presentation details **when supported by OS/driver**.
- `hitch-001.etl`, etc.: CPU scheduling and GPU events stored by WPR from circular buffers.
- `trace-notes.txt`: UTC times, trace lifecycle, errors, disk budget, and important caveats.

The renderer's per-frame CSV includes wall-clock frame-start interval, **previous** Present duration, current Present duration, fence wait, command record time, queue submission time, previous limiter pacing and overshoot, GPU command timestamp duration, Reflex sleep time, and thread CPU **cycles**. Thread cycles are **not** a reliable conversion to CPU Busy milliseconds. Actual blocked-versus-running CPU analysis requires the WPR ETL. Likewise, `gpu_command_ms` is duration between the DX12 GPU timestamps, **not full GPU Busy**.

Frames flagged by interval > `--hitch-ms`, Present duration > max(22.5ms, 0.75 × threshold), or fence wait > max(12ms, 0.5 × threshold). Focus and resize warmup are ignored. A row may be flushed several frames after its GPU work is completed.

## What remains unknown

Our Cyberpunk capture showed an approximately **38.42 ms frame** following a **26.55 ms Present API call** while GPU Busy was about **13 ms**. That supports investigating CPU-side presentation blocking but **does not prove a Windows, driver, VRR, or hardware defect**. A long Present call can indicate normal queue backpressure.

The renderer uses a DXGI flip-discard swap chain; **Independent Flip is not guaranteed**. Use the bundled PresentMon stream to verify the actual mode during your run. GPU workload remains deliberately tiny because this is primarily a presentation experiment. Keep FreeSync/G-Sync settings unchanged, keep the display on, and avoid minimizing or Alt-Tabbing during the test.

### Trace privacy and analysis

ETL files may reveal process names, timing, loaded modules, and other local system data. Review before uploading. You do **not** need the Windows Performance Analyzer locally just to collect traces; the ETLs can be analyzed separately afterward. If a capture fails, share `trace-notes.txt`, `session-*.txt`, and `hitches-*.csv` first.

PresentMon: https://github.com/GameTechDev/PresentMon (official standalone console). WPR: native Microsoft Windows Performance Recorder.
