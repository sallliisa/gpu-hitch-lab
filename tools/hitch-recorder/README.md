# Hitch Recorder — compact, hotkey-triggered Windows ETW tracing

**Separate CLI-only utility** from GPU Hitch Lab's DX12 renderer. It records activity in real games, including Deadlock, Cyberpunk 2077 and Indiana Jones, without injection, overlays, services, hooks, or a local compiler.

## Download

1. Go to [Build Hitch Recorder (Windows x64)](https://github.com/sallliisa/gpu-hitch-lab/actions/workflows/hitch-recorder.yml).
2. Open the latest **successful** run on `main`.
3. Download the `HitchRecorder-Windows-x64` artifact and unzip it anywhere writable.
4. Keep `HitchRecorder.exe` and `HitchCPU.wprp` together. Double-click `Run-HitchRecorder.cmd` and approve the Administrator prompt, or use an elevated terminal.

## Play and capture

Run the tool first, then start CapFrameX and the game. **F11** is observed using Windows `GetAsyncKeyState` and is **not registered or intercepted**; CapFrameX can continue using F11 for its normal capture shortcut. When you **feel** the hitch, press F11 once. The recorder stores the precise key observation time, attempts to insert a WPR marker, gathers the next five seconds, saves the in-memory circular ETW buffer as a **compressed `.etl`**, and resumes recording. If CapFrameX uses F11 to stop/start a capture, that action still occurs independently.

Press **Ctrl+C** in Hitch Recorder's console to exit and discard any unmarked in-memory history. Recording export and restart leave a small **trace gap**, and the export can cause its own hitches.

Default settings:
- **20-second pre-hitch target** (not guaranteed; retention depends on event rate and memory).
- **5 seconds after F11**, **256 MiB** CPU circular buffers, plus WPR's separate GPU buffers.
- **500 MiB target per ETL** and **2,000 MiB total ETL budget**. The per-file size is **advisory**: a single exported file may exceed the target. The tool warns and preserves the evidence; it stops starting new recordings after the total limit. Temporary export requires additional free space.
- Full kernel thread scheduling: process/thread lifecycle, sampled CPU, CPU context switches and ready-thread events (with sampled, context-switch and wakeup call stacks), thread priority, DPC/ISR, hard faults and basic disk I/O.
- **`GPU.light`** WPR profile for graphics kernel/scheduling context; if its combination with CPU cannot start, retry CPU-only with a warning.

Run from an elevated terminal:

```powershell
.\HitchRecorder.exe --history 20 --after 5 --buffer-mb 256 --max-file-mb 500 --max-total-mb 2000
```

Optional: `--no-gpu` reduces tracing to CPU + disk/page fault/interrupt events. `--help` lists the flags. `--self-test` validates profile-file generation without requiring administrator privileges.

## Captured files

Each run creates a new `logs/hitch-recorder-<UTC>` directory containing:

| File | Purpose |
|---|---|
| `hitch-1.etl` | Compressed ETW events surrounding the F11 marker; analyze with WPA/GPUView |
| `markers.csv` | Precise F11 detection UTC timestamp, QPC ticks and frequency, capture status |
| `hitch-1-status.txt` | WPR status immediately before saving, including dropped-event counts when reported |
| `wpr-marker-1.txt` | Attempt to insert a visible event marker in the trace |
| `hitch-1-export.txt` | WPR stop/export output or failure details |
| `recorder.log` | Capture events, warnings, saved sizes, and tracing gaps |
| `HitchCPU.wprp` | Exact CPU profile and buffer size used |

This tool records **system-wide** ETW activity; it does **not** restrict kernel scheduling traces to just a game's PID. That is necessary for studying competing processes, DPC/ISR and wakeups. The recorded PID can be filtered in Windows Performance Analyzer later. In some cases, proprietary drivers or game modules won't have symbols, limiting root-cause attribution.

**Do not run two WPR recorders simultaneously.** CapFrameX's normal lightweight frame capture typically uses a separate PresentMon ETW session, but compatibility is not guaranteed. If WPR fails to start, the console says so and the command logs should be inspected. If the game is configured to reserve F11 for a separate fullscreen action, test the key behavior with a short session first.

### Limitations of circular ETW tracing

Unlike a ring buffer based on time, WPR's memory mode keeps a finite number of bytes. The requested `--history` setting documents the intended window but **does not configure an exact 20-second cutoff**. If your gaming workload fills 256 MiB in 6 seconds, the older context will already be gone. Increasing `--buffer-mb` can help; actual coverage must be verified from the resulting ETL timestamps in WPA.

A hotkey press **does not itself guarantee that the precise frame hitch occurred at that timestamp**; correlate the nearby CapFrameX frame timings. F11 key detection is sampled every 10 ms without global keyboard hook or keyboard-event swallowing, so very short or otherwise blocked key presses might be missed. The .etl can contain process names, loaded DLLs, file activity and other private system information; review before sharing.

### Windows Performance Toolkit

The collector uses `wpr.exe`, built into supported Windows 11 systems. Windows Performance Analyzer (WPA) is needed for detailed local inspection but **not** for collecting; analysis can happen separately after you share the trace.

The WPR profile includes CPU context switch/ready-thread stack walking and a bounded in-memory kernel ETW session. The built-in `GPU.light` profile adds DxgKrnl and graphics scheduling events through Microsoft's supported tooling. No Visual Studio, compiler, injected game DLL, or new kernel driver is required on your PC.

**Validation status:** GitHub Actions verifies that MSVC compiles the console utility and that WPR accepts the shipped `.wprp` profile. Live capture and simultaneous F11 use with your CapFrameX version must still be tested on your Windows installation.
