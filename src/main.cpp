#include <windows.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#ifdef GPU_HITCH_LAB_REFLEX
#include <nvapi.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kBufferCount = 3;
constexpr wchar_t kWindowClass[] = L"GpuHitchLabWindow";

#ifdef GPU_HITCH_LAB_REFLEX
enum class ReflexMode { Off, On, Boost };

const char* ReflexName(ReflexMode mode) {
    switch (mode) {
        case ReflexMode::Off: return "off";
        case ReflexMode::On: return "on";
        case ReflexMode::Boost: return "on+boost";
    }
    return "unknown";
}

const wchar_t* ReflexTitle(ReflexMode mode) {
    switch (mode) {
        case ReflexMode::Off: return L"OFF";
        case ReflexMode::On: return L"ON";
        case ReflexMode::Boost: return L"ON+BOOST";
    }
    return L"UNKNOWN";
}

void CheckNv(NvAPI_Status status, const char* operation) {
    if (status != NVAPI_OK) {
        NvAPI_ShortString description{};
        NvAPI_GetErrorMessage(status, description);
        throw std::runtime_error(std::string(operation) + " failed: " + description);
    }
}
#endif

void Check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        std::ostringstream message;
        message << operation << " failed (HRESULT 0x"
                << std::hex << static_cast<unsigned long>(hr) << ")";
        throw std::runtime_error(message.str());
    }
}

double NowMs() {
    static const double frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    LARGE_INTEGER current{};
    QueryPerformanceCounter(&current);
    return 1000.0 * static_cast<double>(current.QuadPart) / frequency;
}

std::string UtcStamp(bool filename) {
    SYSTEMTIME t{};
    GetSystemTime(&t);
    std::ostringstream out;
    out << std::setfill('0') << std::setw(4) << t.wYear;
    if (filename) {
        out << std::setw(2) << t.wMonth << std::setw(2) << t.wDay
            << "-" << std::setw(2) << t.wHour << std::setw(2) << t.wMinute
            << std::setw(2) << t.wSecond << "-" << std::setw(3) << t.wMilliseconds;
    } else {
        out << "-" << std::setw(2) << t.wMonth << "-" << std::setw(2) << t.wDay
            << "T" << std::setw(2) << t.wHour << ":" << std::setw(2) << t.wMinute
            << ":" << std::setw(2) << t.wSecond << "." << std::setw(3)
            << t.wMilliseconds << "Z";
    }
    return out.str();
}

std::string Utf8(const wchar_t* value) {
    const int count = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) return {};
    std::string output(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, output.data(), count, nullptr, nullptr);
    output.pop_back(); // Remove the terminator written by WideCharToMultiByte.
    return output;
}

struct Options {
    bool vsync = false;
    bool borderless = false;
    bool help = false;
    int fps = 60;
    int width = 1280;
    int height = 720;
    double hitchMs = 30.0;
    double hours = 0.0;
    double minutes = 0.0;
    double seconds = 0.0;
    double durationSeconds = 0.0;
    int maxLogMb = 384;
#ifdef GPU_HITCH_LAB_REFLEX
    ReflexMode reflex = ReflexMode::On;
#endif
};

Options ParseOptions() {
    Options result;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) throw std::runtime_error("CommandLineToArgvW failed");
    try {
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg(argv[i]);
            if (arg == L"--help") {
                result.help = true;
                continue;
            }
            if (arg == L"--borderless") {
                result.borderless = true;
                continue;
            }
            if (i + 1 >= argc) throw std::runtime_error("Missing value for " + Utf8(arg.c_str()));
            const std::wstring value(argv[++i]);
            if (arg == L"--vsync") {
                if (value == L"on") result.vsync = true;
                else if (value == L"off") result.vsync = false;
                else throw std::runtime_error("--vsync must be on or off");
            } else if (arg == L"--fps") {
                result.fps = std::stoi(value);
                if (result.fps < 0 || result.fps > 1000)
                    throw std::runtime_error("--fps must be between 0 and 1000");
            } else if (arg == L"--hitch-ms") {
                result.hitchMs = std::stod(value);
                if (!(result.hitchMs >= 5.0 && result.hitchMs <= 1000.0))
                    throw std::runtime_error("--hitch-ms must be between 5 and 1000");
            } else if (arg == L"--hours") {
                result.hours = std::stod(value);
                if (!std::isfinite(result.hours) || result.hours < 0.0 || result.hours > 168.0)
                    throw std::runtime_error("--hours must be between 0 and 168");
            } else if (arg == L"--minutes") {
                result.minutes = std::stod(value);
                if (!std::isfinite(result.minutes) || result.minutes < 0.0 || result.minutes > 10080.0)
                    throw std::runtime_error("--minutes must be between 0 and 10080");
            } else if (arg == L"--seconds") {
                result.seconds = std::stod(value);
                if (!std::isfinite(result.seconds) || result.seconds < 0.0 || result.seconds > 604800.0)
                    throw std::runtime_error("--seconds must be between 0 and 604800");
            } else if (arg == L"--max-log-mb") {
                result.maxLogMb = std::stoi(value);
                if (result.maxLogMb < 16 || result.maxLogMb > 2048)
                    throw std::runtime_error("--max-log-mb must be between 16 and 2048");
            } else if (arg == L"--width") {
                result.width = std::stoi(value);
                if (result.width < 320 || result.width > 16384)
                    throw std::runtime_error("--width is out of range");
            } else if (arg == L"--height") {
                result.height = std::stoi(value);
                if (result.height < 240 || result.height > 16384)
                    throw std::runtime_error("--height is out of range");
#ifdef GPU_HITCH_LAB_REFLEX
            } else if (arg == L"--reflex") {
                if (value == L"off") result.reflex = ReflexMode::Off;
                else if (value == L"on") result.reflex = ReflexMode::On;
                else if (value == L"boost" || value == L"on+boost")
                    result.reflex = ReflexMode::Boost;
                else throw std::runtime_error("--reflex must be off, on, or boost");
#endif
            } else {
                throw std::runtime_error("Unknown option: " + Utf8(arg.c_str()));
            }
        }
    } catch (...) {
        LocalFree(argv);
        throw;
    }
    LocalFree(argv);
    result.durationSeconds = 3600.0 * result.hours + 60.0 * result.minutes + result.seconds;
    if (result.durationSeconds > 604800.0)
        throw std::runtime_error("Combined duration cannot exceed seven days");
    return result;
}

struct FrameSample {
    uint64_t frameId = 0;
    std::string utc;
    std::string reason;
    double intervalMs = 0.0;
    double previousPresentMs = 0.0;
    double previousCpuWorkMs = 0.0;
    double previousFenceWaitMs = 0.0;
    double presentMs = 0.0;
    double fenceWaitMs = 0.0;
    double recordSubmitMs = 0.0;
    double cpuWorkMs = 0.0;
    double frameStartQpcMs = 0.0;
    double previousPaceMs = 0.0;
    double previousPaceOverrunMs = 0.0;
    double commandRecordMs = 0.0;
    double queueSubmitMs = 0.0;
    uint64_t cpuThreadCycles = 0; // raw thread cycles, not CPU Busy milliseconds
    bool vsync = false;
    int fps = 0;
    UINT width = 0;
    UINT height = 0;
#ifdef GPU_HITCH_LAB_REFLEX
    ReflexMode reflexMode = ReflexMode::Off;
    double reflexSleepMs = 0.0;
#endif
    bool hitch = false;
};

class Logger {
public:
    Logger() {
        wchar_t module[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, module, MAX_PATH))
            throw std::runtime_error("Cannot locate executable directory");
        const std::filesystem::path folder = std::filesystem::path(module).parent_path() / L"logs";
        std::filesystem::create_directories(folder);
        const auto runId = UtcStamp(true);
        csv_.open(folder / ("hitches-" + runId + ".csv"), std::ios::out | std::ios::trunc);
        frames_.open(folder / ("frames-" + runId + ".csv"), std::ios::out | std::ios::trunc);
        session_.open(folder / ("session-" + runId + ".txt"), std::ios::out | std::ios::trunc);
        if (!csv_ || !frames_ || !session_)
            throw std::runtime_error("Cannot create logs beside the executable");
        const char* header = "utc,frame_id,reason,frame_interval_ms,previous_present_ms,"
            "previous_cpu_work_ms,previous_fence_wait_ms,present_ms,"
            "fence_wait_ms,record_submit_ms,cpu_work_ms,gpu_command_ms,"
            "vsync,fps_limit,width,height,frame_start_qpc_ms,previous_pace_ms,"
            "previous_pace_overrun_ms,command_record_ms,queue_submit_ms,cpu_thread_cycles";
        csv_ << header;
        frames_ << header;
#ifdef GPU_HITCH_LAB_REFLEX
        csv_ << ",reflex_mode,reflex_sleep_ms";
        frames_ << ",reflex_mode,reflex_sleep_ms";
#endif
        csv_ << '\n';
        frames_ << '\n';
        csv_ << std::fixed << std::setprecision(3);
        frames_ << std::fixed << std::setprecision(3);
        session_ << "GPU Hitch Lab - minimal DirectX 12\n"
                 << "Started UTC: " << UtcStamp(false) << "\n";
        // Ensure zero-hitch sessions have a visible CSV header immediately.
        // A forced termination should not leave newly created logs empty.
        csv_.flush();
        frames_.flush();
        session_.flush();
        if (!csv_ || !frames_ || !session_)
            throw std::runtime_error("Could not flush log headers to disk");
    }

    void Configuration(const std::string& adapter, bool tearing, const Options& options) {
        session_ << "Adapter: " << adapter << "\n"
                 << "DXGI tearing supported: " << (tearing ? "yes" : "no") << "\n"
                 << "Initial V-Sync: " << (options.vsync ? "on" : "off") << "\n"
                 << "FPS limit: " << options.fps << "\n"
                 << "Borderless initial: " << (options.borderless ? "yes" : "no") << "\n"
                 << "Frame interval hitch threshold (ms): " << options.hitchMs << "\n"
                 << "Requested duration in seconds (zero=indefinite): " << options.durationSeconds << "\n"
                 << "Every completed frame logged to frames CSV; only hits to hitches CSV.\n"
                 << "Frame CSV size limit (MB): " << options.maxLogMb << "\n"
                 << "cpu_thread_cycles is a cycle count, not CPU time in milliseconds.\n"
                 << "Note: GPU timestamp covers only the app's rendering commands, not "
                    "display time or all GPU work.\n";
#ifdef GPU_HITCH_LAB_REFLEX
        session_ << "NVIDIA Reflex edition: official NVAPI driver functions\n"
                 << "Initial Reflex mode: " << ReflexName(options.reflex) << "\n"
                 << "NvAPI minimumIntervalUs: 0 (the same CPU FPS limiter as baseline)\n"
                 << "NvAPI sleep called once per rendered frame, including Reflex Off.\n";
#endif
        maxFrameLogBytes_ = static_cast<uint64_t>(options.maxLogMb) * 1024ULL * 1024ULL;
        session_.flush();
    }

    void Note(const std::string& message) {
        session_ << UtcStamp(false) << " " << message << '\n';
        session_.flush();
    }

    void Frame(const FrameSample& sample, double gpuMs) {
        if (framesFull_) return;
        WriteRow(frames_, sample, gpuMs);
        if (++framesWritten_ % 240 == 0) {
            frames_.flush();
            // A conservative safety ceiling to prevent multi-day tests filling a disk.
            const auto current = frames_.tellp();
            if (current >= 0 && static_cast<uint64_t>(current) >= maxFrameLogBytes_) {
                framesFull_ = true;
                session_ << UtcStamp(false) << " Frame CSV size limit reached; continuing hitch-only logging.\n";
                session_.flush();
            }
        }
    }

    void Hitch(const FrameSample& sample, double gpuMs) {
        WriteRow(csv_, sample, gpuMs);
        csv_.flush();
        frames_.flush();
    }

private:
    static void WriteRow(std::ofstream& stream, const FrameSample& sample, double gpuMs) {
        stream << sample.utc << ',' << sample.frameId << ',' << sample.reason << ','
               << sample.intervalMs << ',' << sample.previousPresentMs << ','
               << sample.previousCpuWorkMs << ',' << sample.previousFenceWaitMs << ','
               << sample.presentMs << ',' << sample.fenceWaitMs << ','
               << sample.recordSubmitMs << ',' << sample.cpuWorkMs << ','
               << gpuMs << ',' << (sample.vsync ? "on" : "off") << ','
               << sample.fps << ',' << sample.width << ',' << sample.height << ','
               << sample.frameStartQpcMs << ',' << sample.previousPaceMs << ','
               << sample.previousPaceOverrunMs << ',' << sample.commandRecordMs << ','
               << sample.queueSubmitMs << ',' << sample.cpuThreadCycles;
#ifdef GPU_HITCH_LAB_REFLEX
        stream << ',' << ReflexName(sample.reflexMode) << ',' << sample.reflexSleepMs;
#endif
        stream << '\n';
    }

    std::ofstream csv_;
    std::ofstream frames_;
    std::ofstream session_;
    uint64_t framesWritten_ = 0;
    uint64_t maxFrameLogBytes_ = 384ULL * 1024ULL * 1024ULL;
    bool framesFull_ = false;
};

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource,
                                  D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

class App {
public:
    App(HINSTANCE instance, Options options)
        : instance_(instance), options_(options) {}

    ~App() {
        if (fence_) {
            try { Drain(); } catch (...) {}
        }
#ifdef GPU_HITCH_LAB_REFLEX
        if (nvapiInitialized_) NvAPI_Unload();
#endif
        if (readback_ && mapped_) readback_->Unmap(0, nullptr);
        if (fenceEvent_) CloseHandle(fenceEvent_);
        if (timerResolutionSet_) timeEndPeriod(1);
    }

    void Initialize() {
        if (timeBeginPeriod(1) == TIMERR_NOERROR) timerResolutionSet_ = true;
        CreateAppWindow();
        CreateGraphics();
#ifdef GPU_HITCH_LAB_REFLEX
        InitializeReflex();
#endif
        logger_.Configuration(adapterName_, tearingSupported_, options_);
#ifdef GPU_HITCH_LAB_REFLEX
        LogDriverReflexStatus();
#endif
        startedMs_ = NowMs();
        ignoreUntilMs_ = startedMs_ + 1500.0; // Ignore shader/driver/window warmup.
        UpdateTitle();
    }

    int Run() {
        MSG message{};
        while (true) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    logger_.Note("Normal window exit. Frames=" + std::to_string(frameId_) +
                                 ", detected_hitches=" + std::to_string(hitchCount_));
                    return static_cast<int>(message.wParam);
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (options_.durationSeconds > 0.0 &&
                NowMs() - startedMs_ >= options_.durationSeconds * 1000.0) {
                logger_.Note("Timed session completed. Frames=" + std::to_string(frameId_) +
                             ", detected_hitches=" + std::to_string(hitchCount_));
                return 0;
            }
            if (!active_ || sizing_ || IsIconic(hwnd_)) {
                previousFrameStartMs_ = -1.0;
                Sleep(50);
                continue;
            }
#ifdef GPU_HITCH_LAB_REFLEX
            if (reflexModeChangePending_) {
                reflexModeChangePending_ = false;
                ApplyReflexMode();
                LogDriverReflexStatus();
                ignoreUntilMs_ = NowMs() + 750.0;
                UpdateTitle();
            }
#endif
            if (resizeWidth_ && resizeHeight_) Resize(resizeWidth_, resizeHeight_);
            RenderFrame();
            if (NowMs() - lastTitleMs_ >= 1000.0) UpdateTitle();
        }
    }

private:
    struct Slot {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12Resource> target;
        uint64_t fenceValue = 0;
        bool pending = false;
        FrameSample sample;
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            app = static_cast<App*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (app) {
            switch (message) {
                case WM_ACTIVATEAPP:
                    app->logger_.Note(wparam ? "Window activated" : "Window deactivated");
                    app->active_ = wparam != 0;
                    app->previousFrameStartMs_ = -1.0;
                    app->ignoreUntilMs_ = NowMs() + 2000.0;
                    return 0;
                case WM_ENTERSIZEMOVE:
                    app->logger_.Note("Entered interactive window move/resize");
                    app->sizing_ = true;
                    return 0;
                case WM_EXITSIZEMOVE:
                    app->logger_.Note("Finished interactive window move/resize");
                    app->sizing_ = false;
                    app->ignoreUntilMs_ = NowMs() + 1000.0;
                    return 0;
                case WM_SIZE:
                    if (wparam != SIZE_MINIMIZED) {
                        app->resizeWidth_ = LOWORD(lparam);
                        app->resizeHeight_ = HIWORD(lparam);
                    }
                    return 0;
                case WM_KEYDOWN:
                    if (wparam == VK_ESCAPE) {
                        DestroyWindow(hwnd);
                        return 0;
                    }
                    if (wparam == 'V' && !(lparam & (1L << 30))) {
                        app->options_.vsync = !app->options_.vsync;
                        app->ignoreUntilMs_ = NowMs() + 750.0;
                        app->UpdateTitle();
                        return 0;
                    }
#ifdef GPU_HITCH_LAB_REFLEX
                    if (wparam == 'R' && !(lparam & (1L << 30))) {
                        switch (app->options_.reflex) {
                            case ReflexMode::Off: app->options_.reflex = ReflexMode::On; break;
                            case ReflexMode::On: app->options_.reflex = ReflexMode::Boost; break;
                            case ReflexMode::Boost: app->options_.reflex = ReflexMode::Off; break;
                        }
                        app->reflexModeChangePending_ = true;
                        return 0;
                    }
#endif
                    if (wparam == VK_F11 && !(lparam & (1L << 30))) {
                        app->ToggleBorderless();
                        return 0;
                    }
                    break;
                case WM_DISPLAYCHANGE:
                    app->logger_.Note("Windows display configuration changed");
                    app->ignoreUntilMs_ = NowMs() + 2000.0;
                    return 0;
                case WM_POWERBROADCAST:
                    app->logger_.Note("Windows power broadcast event code=" + std::to_string(wparam));
                    return TRUE;
                case WM_DESTROY:
                    PostQuitMessage(0);
                    return 0;
                default:
                    break;
            }
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void CreateAppWindow() {
        WNDCLASSEXW klass{};
        klass.cbSize = sizeof(klass);
        klass.hInstance = instance_;
        klass.lpfnWndProc = WindowProc;
        klass.lpszClassName = kWindowClass;
        klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        klass.style = CS_HREDRAW | CS_VREDRAW;
        if (!RegisterClassExW(&klass))
            throw std::runtime_error("RegisterClassExW failed");

        RECT desired{0, 0, options_.width, options_.height};
        AdjustWindowRect(&desired, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd_ = CreateWindowExW(0, kWindowClass, L"GPU Hitch Lab",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                desired.right - desired.left, desired.bottom - desired.top,
                                nullptr, nullptr, instance_, this);
        if (!hwnd_) throw std::runtime_error("CreateWindowExW failed");
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        if (options_.borderless) ToggleBorderless();
        RECT client{};
        GetClientRect(hwnd_, &client);
        width_ = std::max(1L, client.right - client.left);
        height_ = std::max(1L, client.bottom - client.top);
        resizeWidth_ = resizeHeight_ = 0;
    }

    void ToggleBorderless() {
        if (!borderless_) {
            windowStyle_ = GetWindowLongPtrW(hwnd_, GWL_STYLE);
            savedPlacement_.length = sizeof(savedPlacement_);
            GetWindowPlacement(hwnd_, &savedPlacement_);
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor);
            SetWindowLongPtrW(hwnd_, GWL_STYLE, windowStyle_ & ~WS_OVERLAPPEDWINDOW);
            const RECT& r = monitor.rcMonitor;
            SetWindowPos(hwnd_, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            borderless_ = true;
        } else {
            SetWindowLongPtrW(hwnd_, GWL_STYLE, windowStyle_);
            SetWindowPlacement(hwnd_, &savedPlacement_);
            SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            borderless_ = false;
        }
        previousFrameStartMs_ = -1.0;
        ignoreUntilMs_ = NowMs() + 1250.0;
    }

#ifdef GPU_HITCH_LAB_REFLEX
    void InitializeReflex() {
        CheckNv(NvAPI_Initialize(), "NvAPI_Initialize");
        nvapiInitialized_ = true;
        ApplyReflexMode();
    }

    void ApplyReflexMode() {
        NV_SET_SLEEP_MODE_PARAMS mode{};
        mode.version = NV_SET_SLEEP_MODE_PARAMS_VER;
        mode.bLowLatencyMode = options_.reflex != ReflexMode::Off ? 1 : 0;
        mode.bLowLatencyBoost = options_.reflex == ReflexMode::Boost ? 1 : 0;
        mode.minimumIntervalUs = 0; // Retain the same CPU FPS limiter as baseline.
        mode.bUseMarkersToOptimize = 1;
        CheckNv(NvAPI_D3D_SetSleepMode(device_.Get(), &mode), "NvAPI_D3D_SetSleepMode");
    }

    void LogDriverReflexStatus() {
        NV_GET_SLEEP_STATUS_PARAMS status{};
        status.version = NV_GET_SLEEP_STATUS_PARAMS_VER;
        const NvAPI_Status result = NvAPI_D3D_GetSleepStatus(device_.Get(), &status);
        if (result == NVAPI_OK) {
            logger_.Note(std::string("Reflex requested: ") + ReflexName(options_.reflex) +
                         "; driver low-latency state: " +
                         (status.bLowLatencyMode ? "on" : "off") +
                         "; driver full-screen VRR: " + (status.bFsVrr ? "yes" : "no"));
        } else {
            NvAPI_ShortString description{};
            NvAPI_GetErrorMessage(result, description);
            logger_.Note(std::string("Reflex requested: ") + ReflexName(options_.reflex) +
                         "; driver sleep-status query unavailable: " + description);
        }
    }

    void MarkReflex(NV_LATENCY_MARKER_TYPE type, uint64_t frameId) {
        NV_LATENCY_MARKER_PARAMS marker{};
        marker.version = NV_LATENCY_MARKER_PARAMS_VER;
        marker.frameID = frameId;
        marker.markerType = type;
        CheckNv(NvAPI_D3D_SetLatencyMarker(device_.Get(), &marker),
                "NvAPI_D3D_SetLatencyMarker");
    }

    double SleepReflex() {
        const double before = NowMs();
        CheckNv(NvAPI_D3D_Sleep(device_.Get()), "NvAPI_D3D_Sleep");
        return NowMs() - before;
    }
#endif

    void CreateGraphics() {
        Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)), "CreateDXGIFactory2");
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(factory_.As(&factory5))) {
            BOOL allowed = FALSE;
            if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                         &allowed, sizeof(allowed))))
                tearingSupported_ = allowed != FALSE;
        }

        ComPtr<IDXGIFactory6> factory6;
        factory_.As(&factory6);
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            HRESULT hr = factory6
                ? factory6->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                        IID_PPV_ARGS(&adapter))
                : factory_->EnumAdapters1(index, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            Check(hr, "Enumerate DXGI adapters");
            DXGI_ADAPTER_DESC1 desc{};
            Check(adapter->GetDesc1(&desc), "GetDesc1");
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                   IID_PPV_ARGS(&device_));
            if (SUCCEEDED(hr)) {
                adapterName_ = Utf8(desc.Description);
                break;
            }
        }
        if (!device_)
            throw std::runtime_error("No compatible hardware DirectX 12 GPU was found");

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_)),
              "CreateCommandQueue");
        Check(queue_->GetTimestampFrequency(&timestampFrequency_), "GetTimestampFrequency");
        if (!timestampFrequency_)
            throw std::runtime_error("GPU timestamp frequency was zero");

        DXGI_SWAP_CHAIN_DESC1 swapDesc{};
        swapDesc.Width = width_;
        swapDesc.Height = height_;
        swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapDesc.Stereo = FALSE;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = kBufferCount;
        swapDesc.Scaling = DXGI_SCALING_STRETCH;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        swapDesc.Flags = tearingSupported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        ComPtr<IDXGISwapChain1> swap1;
        Check(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd_, &swapDesc,
                                                nullptr, nullptr, &swap1),
              "CreateSwapChainForHwnd");
        Check(swap1.As(&swapChain_), "Query IDXGISwapChain3");
        Check(factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER),
              "MakeWindowAssociation");

        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.NumDescriptors = kBufferCount;
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        Check(device_->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap_)),
              "Create RTV descriptor heap");
        rtvIncrement_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        CreateRenderTargets();

        for (Slot& slot : slots_) {
            Check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(&slot.allocator)),
                  "CreateCommandAllocator");
        }
        Check(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          slots_[0].allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&commandList_)),
              "CreateCommandList");
        Check(commandList_->Close(), "Initial command list close");
        Check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)),
              "CreateFence");
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent_) throw std::runtime_error("CreateEventW failed");

        D3D12_QUERY_HEAP_DESC queryDesc{};
        queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        queryDesc.Count = 2 * kBufferCount;
        Check(device_->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&queryHeap_)),
              "CreateQueryHeap");

        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_READBACK;
        heapProperties.CreationNodeMask = 1;
        heapProperties.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC readbackDesc{};
        readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readbackDesc.Width = sizeof(uint64_t) * 2 * kBufferCount;
        readbackDesc.Height = 1;
        readbackDesc.DepthOrArraySize = 1;
        readbackDesc.MipLevels = 1;
        readbackDesc.SampleDesc.Count = 1;
        readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Check(device_->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE,
                                                &readbackDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr, IID_PPV_ARGS(&readback_)),
              "Create timestamp readback");
        Check(readback_->Map(0, nullptr, reinterpret_cast<void**>(&mapped_)),
              "Map timestamp readback");
    }

    void CreateRenderTargets() {
        for (UINT i = 0; i < kBufferCount; ++i) {
            Check(swapChain_->GetBuffer(i, IID_PPV_ARGS(&slots_[i].target)),
                  "Get swap chain buffer");
            D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(i) * rtvIncrement_;
            device_->CreateRenderTargetView(slots_[i].target.Get(), nullptr, handle);
        }
    }

    void WaitForFence(uint64_t value) {
        if (value && fence_->GetCompletedValue() < value) {
            Check(fence_->SetEventOnCompletion(value, fenceEvent_), "SetEventOnCompletion");
            if (WaitForSingleObject(fenceEvent_, INFINITE) != WAIT_OBJECT_0)
                throw std::runtime_error("GPU fence wait failed");
        }
    }

    void Retire(UINT index) {
        Slot& slot = slots_[index];
        if (!slot.pending) return;
        const uint64_t begin = mapped_[2 * index];
        const uint64_t end = mapped_[2 * index + 1];
        const double gpuMs = (end >= begin)
            ? 1000.0 * static_cast<double>(end - begin) /
                  static_cast<double>(timestampFrequency_)
            : 0.0;
        logger_.Frame(slot.sample, gpuMs);
        if (slot.sample.hitch) logger_.Hitch(slot.sample, gpuMs);
        slot.pending = false;
    }

    void Drain() {
        for (UINT i = 0; i < kBufferCount; ++i) {
            WaitForFence(slots_[i].fenceValue);
            Retire(i);
        }
    }

    void Resize(UINT width, UINT height) {
        resizeWidth_ = resizeHeight_ = 0;
        if (width == width_ && height == height_) return;
        Drain();
        for (Slot& slot : slots_) slot.target.Reset();
        const UINT flags = tearingSupported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        Check(swapChain_->ResizeBuffers(kBufferCount, width, height,
                                        DXGI_FORMAT_R8G8B8A8_UNORM, flags),
              "ResizeBuffers");
        width_ = width;
        height_ = height;
        CreateRenderTargets();
        previousFrameStartMs_ = -1.0;
        ignoreUntilMs_ = NowMs() + 1000.0;
    }

    void Pace(double frameStart) {
        const double paceBegin = NowMs();
        if (options_.fps == 0) {
            previousPaceMs_ = 0.0;
            previousPaceOverrunMs_ = 0.0;
            return;
        }
        const double target = frameStart + 1000.0 / static_cast<double>(options_.fps);
        for (;;) {
            const double remaining = target - NowMs();
            if (remaining <= 0.0) break;
            if (remaining > 2.0)
                Sleep(static_cast<DWORD>(remaining - 0.75));
            else
                YieldProcessor();
        }
        previousPaceMs_ = NowMs() - paceBegin;
        previousPaceOverrunMs_ = std::max(0.0, NowMs() - target);
    }

    void RenderFrame() {
#ifdef GPU_HITCH_LAB_REFLEX
        const double reflexSleepMs = SleepReflex();
#endif
        const double start = NowMs();
        ULONG64 startCycles = 0;
        const bool cyclesKnown = QueryThreadCycleTime(GetCurrentThread(), &startCycles) != 0;
        const double interval = previousFrameStartMs_ >= 0.0
            ? start - previousFrameStartMs_ : 0.0;
        previousFrameStartMs_ = start;

#ifdef GPU_HITCH_LAB_REFLEX
        const uint64_t reflexFrameId = frameId_ + 1;
        MarkReflex(SIMULATION_START, reflexFrameId);
#endif
        const float hue = static_cast<float>((frameId_ % 600) / 600.0);
        const float color[] = {0.08f + 0.16f * hue, 0.11f,
                               0.25f - 0.12f * hue, 1.0f};
#ifdef GPU_HITCH_LAB_REFLEX
        MarkReflex(SIMULATION_END, reflexFrameId);
        MarkReflex(RENDERSUBMIT_START, reflexFrameId);
#endif
        const UINT index = swapChain_->GetCurrentBackBufferIndex();
        Slot& slot = slots_[index];
        const double beforeWait = NowMs();
        WaitForFence(slot.fenceValue);
        const double afterWait = NowMs();
        Retire(index);

        const double recordBegin = NowMs();
        Check(slot.allocator->Reset(), "Reset command allocator");
        Check(commandList_->Reset(slot.allocator.Get(), nullptr), "Reset command list");
        commandList_->EndQuery(queryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * index);
        D3D12_RESOURCE_BARRIER toRender = Transition(
            slot.target.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        commandList_->ResourceBarrier(1, &toRender);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(index) * rtvIncrement_;

        commandList_->ClearRenderTargetView(rtv, color, 0, nullptr);
        D3D12_RESOURCE_BARRIER toPresent = Transition(
            slot.target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        commandList_->ResourceBarrier(1, &toPresent);
        commandList_->EndQuery(queryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * index + 1);
        commandList_->ResolveQueryData(queryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                       2 * index, 2, readback_.Get(),
                                       sizeof(uint64_t) * 2 * index);
        Check(commandList_->Close(), "Close command list");
        const double recordEnd = NowMs();
        ID3D12CommandList* lists[] = {commandList_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        const uint64_t value = ++nextFenceValue_;
        Check(queue_->Signal(fence_.Get(), value), "Signal GPU fence");
        slot.fenceValue = value;
        const double submitEnd = NowMs();

#ifdef GPU_HITCH_LAB_REFLEX
        MarkReflex(RENDERSUBMIT_END, reflexFrameId);
        MarkReflex(PRESENT_START, reflexFrameId);
#endif
        const double beforePresent = NowMs();
        const UINT presentFlags = (!options_.vsync && tearingSupported_)
            ? DXGI_PRESENT_ALLOW_TEARING : 0;
        Check(swapChain_->Present(options_.vsync ? 1 : 0, presentFlags), "DXGI Present");
        const double afterPresent = NowMs();
        ULONG64 endCycles = 0;
        const bool cyclesEnded = QueryThreadCycleTime(GetCurrentThread(), &endCycles) != 0;
#ifdef GPU_HITCH_LAB_REFLEX
        MarkReflex(PRESENT_END, reflexFrameId);
#endif

        FrameSample sample{};
        sample.utc = UtcStamp(false);
        sample.frameId = ++frameId_;
        sample.intervalMs = interval;
        sample.previousPresentMs = lastPresentMs_;
        sample.previousCpuWorkMs = lastCpuWorkMs_;
        sample.previousFenceWaitMs = lastFenceWaitMs_;
        sample.presentMs = afterPresent - beforePresent;
        sample.fenceWaitMs = afterWait - beforeWait;
        sample.recordSubmitMs = beforePresent - afterWait;
        sample.cpuWorkMs = afterPresent - beforeWait;
        sample.frameStartQpcMs = start;
        sample.previousPaceMs = previousPaceMs_;
        sample.previousPaceOverrunMs = previousPaceOverrunMs_;
        sample.commandRecordMs = recordEnd - recordBegin;
        sample.queueSubmitMs = submitEnd - recordEnd;
        sample.cpuThreadCycles = (cyclesKnown && cyclesEnded && endCycles >= startCycles)
            ? endCycles - startCycles : 0;
        sample.vsync = options_.vsync;
        sample.fps = options_.fps;
        sample.width = width_;
        sample.height = height_;
#ifdef GPU_HITCH_LAB_REFLEX
        sample.reflexMode = options_.reflex;
        sample.reflexSleepMs = reflexSleepMs;
#endif

        if (interval > 0.0 && start >= ignoreUntilMs_) {
            if (interval > options_.hitchMs) sample.reason += "interval|";
            if (sample.presentMs > std::max(22.5, options_.hitchMs * 0.75))
                sample.reason += "present|";
            if (sample.fenceWaitMs > std::max(12.0, options_.hitchMs * 0.50))
                sample.reason += "fence_wait|";
        }
        sample.hitch = !sample.reason.empty();
        if (sample.hitch) {
            sample.reason.pop_back(); // Remove trailing '|'.
            ++hitchCount_;
        }
        slot.sample = std::move(sample);
        slot.pending = true;
        lastPresentMs_ = afterPresent - beforePresent;
        lastCpuWorkMs_ = afterPresent - beforeWait;
        lastFenceWaitMs_ = afterWait - beforeWait;
        lastIntervalMs_ = interval;
        Pace(start);
    }

    void UpdateTitle() {
        std::wostringstream title;
        title << L"GPU Hitch Lab | " << frameId_ << L" frames, "
              << hitchCount_ << L" hitches | V-Sync "
              << (options_.vsync ? L"ON" : L"OFF") << L" | FPS "
              << (options_.fps ? std::to_wstring(options_.fps) : L"uncapped");
#ifdef GPU_HITCH_LAB_REFLEX
        title << L" | Reflex " << ReflexTitle(options_.reflex);
#endif
        title
              << L" | interval " << std::fixed << std::setprecision(1)
              << lastIntervalMs_ << L" ms | Present " << lastPresentMs_
              << L" ms | V/F11/"
#ifdef GPU_HITCH_LAB_REFLEX
              << L"R/"
#endif
              << L"Esc";
        SetWindowTextW(hwnd_, title.str().c_str());
        lastTitleMs_ = NowMs();
    }

    HINSTANCE instance_ = nullptr;
    Options options_;
    Logger logger_;
    HWND hwnd_ = nullptr;
    bool active_ = true;
    bool sizing_ = false;
    bool borderless_ = false;
    bool tearingSupported_ = false;
    bool timerResolutionSet_ = false;
    LONG_PTR windowStyle_ = 0;
    WINDOWPLACEMENT savedPlacement_{};
    UINT width_ = 0;
    UINT height_ = 0;
    UINT resizeWidth_ = 0;
    UINT resizeHeight_ = 0;
    std::string adapterName_;
    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swapChain_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    ComPtr<ID3D12GraphicsCommandList> commandList_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12QueryHeap> queryHeap_;
    ComPtr<ID3D12Resource> readback_;
    std::array<Slot, kBufferCount> slots_{};
    uint64_t* mapped_ = nullptr;
    uint64_t timestampFrequency_ = 0;
    uint64_t nextFenceValue_ = 0;
    UINT rtvIncrement_ = 0;
    HANDLE fenceEvent_ = nullptr;
    uint64_t frameId_ = 0;
    uint64_t hitchCount_ = 0;
    double startedMs_ = 0.0;
    double lastTitleMs_ = 0.0;
    double ignoreUntilMs_ = 0.0;
    double previousFrameStartMs_ = -1.0;
    double lastPresentMs_ = 0.0;
    double lastCpuWorkMs_ = 0.0;
    double lastFenceWaitMs_ = 0.0;
    double lastIntervalMs_ = 0.0;
    double previousPaceMs_ = 0.0;
    double previousPaceOverrunMs_ = 0.0;
#ifdef GPU_HITCH_LAB_REFLEX
    bool nvapiInitialized_ = false;
    bool reflexModeChangePending_ = false;
#endif
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try {
        Options options = ParseOptions();
        if (options.help) {
            MessageBoxW(nullptr,
                L"GPU Hitch Lab\n\n"
                L"--vsync on|off   (default: off)\n"
                L"--fps N          (0 = uncapped; default: 60)\n"
                L"--hitch-ms N     (default: 30)\n"
                L"--borderless     (monitor-sized borderless window)\n"
                L"--hours N --minutes N --seconds N  (additive duration)\n"
                L"--max-log-mb N  (frame CSV cap; default 384)\n"
                L"--width N --height N\n\n"
#ifdef GPU_HITCH_LAB_REFLEX
                L"--reflex off|on|boost  (default: on)\n\n"
                L"R = cycle Reflex Off/On/On+Boost.\n"
#endif
                L"V = toggle V-Sync; F11 = toggle borderless; Esc = exit.\n"
                L"Hitches are logged in a logs folder beside the executable.",
                L"GPU Hitch Lab - Help", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        App app(instance, options);
        app.Initialize();
        return app.Run();
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "GPU Hitch Lab - Error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
