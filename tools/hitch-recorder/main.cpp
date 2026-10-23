#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

std::atomic<bool> stopRequested{false};

BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT ||
        signal == CTRL_CLOSE_EVENT || signal == CTRL_SHUTDOWN_EVENT) {
        stopRequested.store(true);
        return TRUE;
    }
    return FALSE;
}

struct Config {
    int historySeconds = 20; // requested, not a guarantee: circular ETW is bounded by bytes
    int afterSeconds = 5;
    int bufferMb = 256;       // custom WPR kernel session budget; GPU.light adds its own
    int maxFileMb = 500;      // advisory; WPR cannot enforce an output size in advance
    int maxTotalMb = 2000;    // prevent further captures after this total is reached
    bool gpu = true;
    bool selfTest = false;
};

std::wstring Quote(const std::wstring& input) {
    return L"\"" + input + L"\"";
}

std::string TimestampUtc() {
    SYSTEMTIME t{};
    GetSystemTime(&t);
    char buffer[80]{};
    sprintf_s(buffer, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return buffer;
}

std::wstring FileTimestampUtc() {
    SYSTEMTIME t{};
    GetSystemTime(&t);
    wchar_t b[80]{};
    swprintf_s(b, L"%04u%02u%02u-%02u%02u%02u-%03u",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return b;
}

std::uint64_t QpcTicks() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

std::uint64_t QpcHz() {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return static_cast<std::uint64_t>(frequency.QuadPart);
}

fs::path ExeDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size == path.size()) throw std::runtime_error("Cannot read executable path.");
    path.resize(size);
    return fs::path(path).parent_path();
}

bool Elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &returned);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

void Help() {
    std::cout <<
        "Hitch Recorder - compact circular Windows ETW capture\n\n"
        "Run as Administrator. Leave it open while playing.\n"
        "F11 (shared with CapFrameX): mark hitch, save after delay, resume.\n"
        "Ctrl+C: stop, discard uncaptured buffer.\n\n"
        "Options:\n"
        "  --history N       Requested seconds before key (default 20)\n"
        "  --after N         Seconds after key before saving (default 5)\n"
        "  --buffer-mb N     CPU circular memory buffers (default 256)\n"
        "  --max-file-mb N   Warn if saved ETL exceeds target (default 500)\n"
        "  --max-total-mb N  Stop new saves at this total (default 2000)\n"
        "  --no-gpu          CPU/DPC/disk/hard-fault only; omit GPU.light profile\n"
        "  --self-test       Check profile generation without recording\n"
        "  --help            This help\n"
        "\nNOTE: History is limited by buffer bytes / event rate, not exact seconds.\n";
}

Config Parse(int argc, wchar_t** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg(argv[i]);
        if (arg == L"--help" || arg == L"-h") {
            Help();
            std::exit(0);
        }
        if (arg == L"--self-test") { cfg.selfTest = true; continue; }
        if (arg == L"--no-gpu") { cfg.gpu = false; continue; }
        if (i + 1 >= argc) throw std::runtime_error("Missing command-line option value.");
        const std::wstring val(argv[++i]);
        const int n = std::stoi(val);
        if (arg == L"--history") cfg.historySeconds = n;
        else if (arg == L"--after") cfg.afterSeconds = n;
        else if (arg == L"--buffer-mb") cfg.bufferMb = n;
        else if (arg == L"--max-file-mb") cfg.maxFileMb = n;
        else if (arg == L"--max-total-mb") cfg.maxTotalMb = n;
        else throw std::runtime_error("Unknown option (use --help).");
    }
    if (cfg.historySeconds < 5 || cfg.historySeconds > 60 ||
        cfg.afterSeconds < 0 || cfg.afterSeconds > 30 ||
        cfg.bufferMb < 32 || cfg.bufferMb > 1024 ||
        cfg.maxFileMb < 50 || cfg.maxFileMb > 2048 ||
        cfg.maxTotalMb < cfg.maxFileMb || cfg.maxTotalMb > 8192) {
        throw std::runtime_error("An option is out of range; run --help.");
    }
    return cfg;
}

class Logger {
public:
    explicit Logger(const fs::path& folder) : folder_(folder) {
        fs::create_directories(folder_);
        notes_.open(folder_ / "recorder.log", std::ios::out | std::ios::app);
        markers_.open(folder_ / "markers.csv", std::ios::out | std::ios::trunc);
        if (!notes_ || !markers_) throw std::runtime_error("Cannot open recorder logs.");
        markers_ << "utc,qpc_ticks,qpc_frequency,capture_id,result\n";
        markers_.flush();
    }
    const fs::path& Folder() const { return folder_; }
    void Note(const std::string& s) {
        const std::string line = TimestampUtc() + "  " + s;
        std::cout << line << std::endl;
        notes_ << line << '\n';
        notes_.flush();
    }
    void Mark(const std::string& utc, std::uint64_t qpc, std::uint64_t hz,
              int id, const std::string& result) {
        markers_ << utc << ',' << qpc << ',' << hz << ',' << id << ',' << result << '\n';
        markers_.flush();
    }
private:
    fs::path folder_;
    std::ofstream notes_;
    std::ofstream markers_;
};

fs::path WprPath() {
    wchar_t systemPath[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(systemPath, MAX_PATH);
    if (!n || n >= MAX_PATH) throw std::runtime_error("GetSystemDirectory failed.");
    const fs::path path = fs::path(systemPath) / "wpr.exe";
    if (!fs::exists(path))
        throw std::runtime_error("Windows Performance Recorder (wpr.exe) not found.");
    return path;
}

// The executable is fixed (System32\wpr.exe), and all variable paths are quoted.
// Child stdout/stderr go to a per-command file, never to a running game's console.
struct CommandResult { bool launched = false; DWORD exitCode = 0xffffffffu; };

CommandResult RunWpr(const fs::path& wpr, const std::wstring& args,
                     const fs::path& logPath, DWORD timeoutMs = 240000) {
    SECURITY_ATTRIBUTES sec{};
    sec.nLength = sizeof(sec);
    sec.bInheritHandle = TRUE;
    const HANDLE stdoutFile = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        &sec, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (stdoutFile == INVALID_HANDLE_VALUE) return {};
    const HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &sec, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullInput == INVALID_HANDLE_VALUE) { CloseHandle(stdoutFile); return {}; }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullInput;
    startup.hStdOutput = stdoutFile;
    startup.hStdError = stdoutFile;
    PROCESS_INFORMATION proc{};
    std::wstring command = Quote(wpr.wstring()) + L" " + args;
    std::vector<wchar_t> commandLine(command.begin(), command.end());
    commandLine.push_back(L'\0');
    CommandResult result{};
    if (CreateProcessW(wpr.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup, &proc)) {
        result.launched = true;
        const DWORD waitResult = WaitForSingleObject(proc.hProcess, timeoutMs);
        if (waitResult == WAIT_OBJECT_0) {
            GetExitCodeProcess(proc.hProcess, &result.exitCode);
        } else if (waitResult == WAIT_TIMEOUT) {
            TerminateProcess(proc.hProcess, 0xdead);
            WaitForSingleObject(proc.hProcess, INFINITE);
            result.exitCode = WAIT_TIMEOUT;
        }
        CloseHandle(proc.hThread);
        CloseHandle(proc.hProcess);
    }
    CloseHandle(nullInput);
    CloseHandle(stdoutFile);
    return result;
}

std::string ReadText(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Missing HitchCPU.wprp near the executable.");
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}

fs::path GenerateProfile(const fs::path& folder, int mb, const fs::path& source) {
    auto xml = ReadText(source);
    const std::string oldText = "<Buffers Value=\"256\"/>";
    const size_t index = xml.find(oldText);
    if (index == std::string::npos)
        throw std::runtime_error("HitchCPU.wprp missing expected buffer placeholder.");
    xml.replace(index, oldText.size(), "<Buffers Value=\"" + std::to_string(mb) + "\"/>");
    const fs::path target = folder / "HitchCPU.wprp";
    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    file << xml;
    file.close();
    if (!file) throw std::runtime_error("Cannot generate ETW profile.");
    return target;
}

std::uint64_t FileBytes(const fs::path& path) {
    std::error_code ec;
    const auto n = fs::file_size(path, ec);
    return ec ? 0 : static_cast<std::uint64_t>(n);
}

std::uint64_t TotalEtlBytes(const fs::path& folder) {
    std::uint64_t sum = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (ec) break;
        if (entry.is_regular_file() && entry.path().extension() == ".etl")
            sum += FileBytes(entry.path());
    }
    return sum;
}

class Recorder {
public:
    Recorder(Config cfg, fs::path dir, fs::path profile, fs::path wpr, Logger& log)
        : cfg_(cfg), dir_(std::move(dir)), profile_(std::move(profile)),
          wpr_(std::move(wpr)), log_(log) {
        session_ = L"HitchRec_" + std::to_wstring(GetCurrentProcessId());
    }
    ~Recorder() {
        if (active_) Cancel();
    }
    bool Active() const { return active_; }
    void Start() {
        const std::wstring custom = Quote(profile_.wstring() + L"!HitchCPU.Light");
        const std::wstring gpuPart = cfg_.gpu ? L" -start GPU.light" : L"";
        const auto cmd = L"-start " + custom + gpuPart + L" -instancename " + session_;
        CommandResult r = RunWpr(wpr_, cmd, dir_ / "wpr-start.txt");
        if ((!r.launched || r.exitCode != 0) && cfg_.gpu) {
            log_.Note("GPU.light or combined profile start failed; trying CPU-only. See wpr-start.txt.");
            // WPR can partially initialize a session before returning failure.
            RunWpr(wpr_, L"-cancel -instancename " + session_, dir_ / "wpr-start-recovery.txt", 30000);
            r = RunWpr(wpr_, L"-start " + custom + L" -instancename " + session_,
                       dir_ / "wpr-start-cpu-only.txt");
            cfg_.gpu = false;
        }
        if (!r.launched || r.exitCode != 0) {
            log_.Note("WPR did not start. Check wpr-start.txt / wpr-start-cpu-only.txt.");
            throw std::runtime_error("ETW start failed. No tracing is running.");
        }
        active_ = true;
        log_.Note(cfg_.gpu
            ? "ETW memory recording active: CPU stacks + GPU.light."
            : "ETW memory recording active: CPU stacks only.");
    }
    void MarkKey(int id) {
        // Marker appears in the ETL; timestamp in CSV is taken BEFORE spawning WPR.
        const auto marker = L"-marker " + Quote(L"F11_HITCH_" + std::to_wstring(id))
                            + L" -instancename " + session_;
        const CommandResult r = RunWpr(wpr_, marker, dir_ / ("wpr-marker-" + std::to_string(id) + ".txt"), 10000);
        if (!r.launched || r.exitCode != 0)
            log_.Note("WPR ETL marker unavailable; precise hotkey UTC/QPC still in markers.csv.");
    }
    bool Save(int id) {
        if (!active_) return false;
        const auto base = "hitch-" + std::to_string(id);
        const fs::path statusFile = dir_ / (base + "-status.txt");
        const auto statusCmd = L"-status -instancename " + session_;
        RunWpr(wpr_, statusCmd, statusFile, 15000);
        const fs::path trace = dir_ / (base + ".etl");
        log_.Note("Exporting circular history now. This may take time and perturb the game.");
        const auto command = L"-stop " + Quote(trace.wstring()) +
            L" -compress -skipPdbGen -instancename " + session_;
        const CommandResult r = RunWpr(wpr_, command, dir_ / (base + "-export.txt"), 240000);
        if (!r.launched || r.exitCode != 0 || !fs::exists(trace)) {
            log_.Note("Trace export failed; see " + base + "-export.txt.");
            // A failed stop can leave a WPR kernel session running.
            Cancel();
            return false;
        }
        active_ = false;
        const std::uint64_t bytes = FileBytes(trace);
        log_.Note(base + ".etl saved: " + std::to_string(bytes / 1024) + " KiB");
        if (bytes > static_cast<std::uint64_t>(cfg_.maxFileMb) * 1024 * 1024)
            log_.Note("WARNING: ETL exceeded per-file target; kept to avoid losing evidence.");
        return true;
    }
    void Cancel() {
        if (!active_) return;
        log_.Note("Cancelling our WPR session without exporting extra data.");
        const auto cmd = L"-cancel -instancename " + session_;
        RunWpr(wpr_, cmd, dir_ / "wpr-cancel.txt", 30000);
        active_ = false;
    }
private:
    Config cfg_;
    fs::path dir_, profile_, wpr_;
    Logger& log_;
    std::wstring session_;
    bool active_ = false;
};

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const Config cfg = Parse(argc, argv);
        const fs::path root = ExeDirectory();
        const fs::path profileSource = root / "HitchCPU.wprp";

        if (cfg.selfTest) {
            const auto scratch = fs::temp_directory_path() /
                ("HitchRecorder-check-" + std::to_string(GetCurrentProcessId()));
            fs::create_directories(scratch);
            const auto generated = GenerateProfile(scratch, cfg.bufferMb, profileSource);
            std::wcout << L"Profile generation OK: " << generated.wstring() << L'\n';
            fs::remove_all(scratch);
            return 0;
        }
        if (!Elevated()) {
            std::cerr << "ERROR: Run HitchRecorder.exe in an Administrator terminal.\n"
                      << "Or use Run-HitchRecorder.cmd to request elevation.\n";
            return 2;
        }
        const fs::path wpr = WprPath();
        const fs::path runDir = root / "logs" / (L"hitch-recorder-" + FileTimestampUtc());
        Logger log(runDir);
        const fs::path profile = GenerateProfile(runDir, cfg.bufferMb, profileSource);

        log.Note("Started. F11 marks hitches; Ctrl+C exits. F11 is observed, not intercepted.");
        log.Note("Requested history=" + std::to_string(cfg.historySeconds)
            + "s; post=" + std::to_string(cfg.afterSeconds) + "s; kernel buffer="
            + std::to_string(cfg.bufferMb) + " MiB; GPU additional buffers not counted.");
        log.Note("QPC frequency=" + std::to_string(QpcHz()));
        log.Note("ETW contains system activity and may be privacy-sensitive.");
        log.Note("Actual retained history duration is not guaranteed; inspect ETL time range.");
        log.Note("Do not run another WPR or full ETW collector in parallel.");
        SetConsoleCtrlHandler(ConsoleHandler, TRUE);
        Recorder rec(cfg, runDir, profile, wpr, log);
        rec.Start();

        bool wasPressed = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        bool pending = false;
        int eventId = 0;
        std::string markerUtc;
        std::uint64_t markerQpc = 0;
        auto captureAt = Clock::now();
        while (!stopRequested.load()) {
            const bool pressed = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
            if (pressed && !wasPressed) {
                if (!pending && rec.Active()) {
                    ++eventId;
                    markerUtc = TimestampUtc();
                    markerQpc = QpcTicks();
                    log.Mark(markerUtc, markerQpc, QpcHz(), eventId, "triggered");
                    log.Note("F11 event #" + std::to_string(eventId)
                        + ": will export " + std::to_string(cfg.afterSeconds) + "s after key.");
                    pending = true;
                    captureAt = Clock::now() + std::chrono::seconds(cfg.afterSeconds);
                    rec.MarkKey(eventId);
                } else {
                    log.Note("F11 ignored while an earlier capture is pending.");
                }
            }
            wasPressed = pressed;

            if (pending && Clock::now() >= captureAt) {
                const bool saved = rec.Save(eventId);
                log.Mark(TimestampUtc(), QpcTicks(), QpcHz(), eventId,
                         saved ? "saved" : "export_failed");
                pending = false;
                if (TotalEtlBytes(runDir) >=
                    static_cast<std::uint64_t>(cfg.maxTotalMb) * 1024 * 1024) {
                    log.Note("Total ETL budget reached. Stopping to preserve disk space.");
                    break;
                }
                if (stopRequested.load()) break;
                log.Note("Resuming ETW after export. Frames during export may be disturbed.");
                rec.Start();
            }
            Sleep(10); // Detects F11 without monopolizing it or installing keyboard hooks.
        }
        if (pending && rec.Active()) {
            log.Note("Ctrl+C during pending capture: exporting available buffer now.");
            const bool saved = rec.Save(eventId);
            log.Mark(TimestampUtc(), QpcTicks(), QpcHz(), eventId,
                     saved ? "saved_early" : "export_failed");
        }
        log.Note("Stopping; other (unmarked) buffered data will not be saved.");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "HitchRecorder error: " << e.what() << '\n';
        return 1;
    }
}
