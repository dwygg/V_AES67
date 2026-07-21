#include "aes67_engine.h"  // pulls in winsock2.h before windows.h
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include "logger.h"

// ---- Ctrl+C handler ----
static Aes67Engine* g_pEngine = nullptr;

static BOOL WINAPI CtrlHandler(DWORD fdwCtrlType) {
    if (fdwCtrlType == CTRL_C_EVENT || fdwCtrlType == CTRL_BREAK_EVENT) {
        if (g_pEngine) g_pEngine->SignalStop();
        return TRUE;  // Handled
    }
    return FALSE;
}

// ---- Command-line parsing ----
struct CmdLineArgs {
    AudioConfig config;
    NetworkConfig netConfig;
    const wchar_t* logFile = nullptr;
    bool showHelp = false;
};

static void PrintUsage() {
    puts("");
    puts("AES67 Audio Engine - M4 Loopback Capture");
    puts("");
    puts("Usage: aes67_engine.exe [options]");
    puts("");
    puts("Options:");
    puts("  -d, --duration N     Run duration in seconds, 0 = indefinite (default 10)");
    puts("  -l, --log FILE       Log file path");
    puts("      --no-autostart   Panel-hosted mode: init + listen, wait for panel START");
    puts("      --managed        Alias of --no-autostart");
    puts("  -h, --help           Show this help");
    puts("");
    puts("Audio: L24 / 48kHz / 2ch / ptime=1ms (AES67 locked)");
    puts("  --dest ADDR          RTP destination IP (default 239.69.1.128)");
    puts("  --port N             RTP destination port (default 5004)");
    puts("  --no-tx              Disable AES67 transmit (capture only)");
    puts("");
    puts("Network (AES67 Receive):");
    puts("  --rx                 Enable AES67 receive + render");
    puts("  --source ADDR        RTP source multicast IP (default 239.69.1.128)");
    puts("  --rxport N           RTP receive port (default 5004)");
    puts("");
    puts("Examples:");
    puts("  aes67_engine.exe --duration 30");
    puts("  aes67_engine.exe --rx --duration 30           (full duplex: TX+RX loopback)");
    puts("  aes67_engine.exe --duration 0 --no-autostart  (panel-hosted: wait for panel START)");
    puts("");
}

static CmdLineArgs ParseArgs(int argc, wchar_t* argv[]) {
    CmdLineArgs args;

    for (int i = 1; i < argc; i++) {
        wchar_t* arg = argv[i];

        if (wcscmp(arg, L"-h") == 0 || wcscmp(arg, L"--help") == 0) {
            args.showHelp = true;
            return args;
        }

        auto nextVal = [&](const wchar_t* name) -> const wchar_t* {
            if (i + 1 < argc && argv[i + 1][0] != L'-') {
                i++;
                return argv[i];
            }
            Logger::Instance().Warn("%ls requires a value, using default", name);
            return nullptr;
        };

        if (wcscmp(arg, L"-d") == 0 || wcscmp(arg, L"--duration") == 0) {
            auto* v = nextVal(L"duration");
            if (v) args.config.durationSec = (UINT32)_wtoi(v);
        }
        else if (wcscmp(arg, L"-l") == 0 || wcscmp(arg, L"--log") == 0) {
            auto* v = nextVal(L"log");
            if (v) args.logFile = v;
        }
        else if (wcscmp(arg, L"--dest") == 0) {
            auto* v = nextVal(L"dest");
            if (v) {
                char buf[64];
                WideCharToMultiByte(CP_UTF8, 0, v, -1, buf, sizeof(buf), nullptr, nullptr);
                args.netConfig.destAddr = buf;
            }
        }
        else if (wcscmp(arg, L"--port") == 0) {
            auto* v = nextVal(L"port");
            if (v) args.netConfig.destPort = (uint16_t)_wtoi(v);
        }
        else if (wcscmp(arg, L"--no-tx") == 0) {
            args.netConfig.enableTx = false;
        }
        else if (wcscmp(arg, L"--no-autostart") == 0 || wcscmp(arg, L"--managed") == 0) {
            // Panel-hosted mode: don't auto-start audio; wait for panel START.
            args.config.autoStart = false;
        }
        else if (wcscmp(arg, L"--rx") == 0) {
            args.netConfig.enableRx = true;
        }
        else if (wcscmp(arg, L"--source") == 0) {
            auto* v = nextVal(L"source");
            if (v) {
                char buf[64];
                WideCharToMultiByte(CP_UTF8, 0, v, -1, buf, sizeof(buf), nullptr, nullptr);
                args.netConfig.sourceAddr = buf;
                args.netConfig.enableRx = true;  // --source implies --rx
            }
        }
        else if (wcscmp(arg, L"--rxport") == 0) {
            auto* v = nextVal(L"rxport");
            if (v) args.netConfig.sourcePort = (uint16_t)_wtoi(v);
        }
        else {
            Logger::Instance().Warn("Unknown option: %ls", arg);
        }
    }
    return args;
}

// ---- Entry point ----
int wmain(int argc, wchar_t* argv[]) {
    // Quick-parse: need --help before logger to avoid file creation
    bool showHelp = false;
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"-h") == 0 || wcscmp(argv[i], L"--help") == 0) {
            showHelp = true; break;
        }
    }

    // Extract log file early
    const wchar_t* logFile = nullptr;
    for (int i = 1; i < argc - 1; i++) {
        if (wcscmp(argv[i], L"-l") == 0 || wcscmp(argv[i], L"--log") == 0) {
            logFile = argv[i + 1]; break;
        }
    }

    Logger::Instance().Init(logFile, LogLevel::Info);

    if (showHelp) {
        PrintUsage();
        Logger::Instance().Shutdown();
        return 0;
    }

    CmdLineArgs args = ParseArgs(argc, argv);

    // Log configuration (format locked to AES67: L24/48kHz/2ch/ptime=1ms)
    Logger::Instance().Info("AES67 Audio Engine - P3 (format locked)");
    Logger::Instance().Info("  Format:      L24 / 48kHz / 2ch / ptime=1ms");
    Logger::Instance().Info("  Duration:    %u s (0=indefinite)", args.config.durationSec);
    if (args.logFile) {
        Logger::Instance().Info("  Log file:    %s", WideToNarrow(args.logFile).c_str());
    }
    if (args.netConfig.enableTx) {
        Logger::Instance().Info("  TX dest:     %s:%u", args.netConfig.destAddr.c_str(), args.netConfig.destPort);
    }
    if (args.netConfig.enableRx) {
        Logger::Instance().Info("  RX source:   %s:%u", args.netConfig.sourceAddr.c_str(), args.netConfig.sourcePort);
    }
    if (!args.netConfig.enableTx && !args.netConfig.enableRx) {
        Logger::Instance().Info("  Network:     disabled");
    }

    // Register Ctrl+C handler
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    DWORD glitches = 0;

    {
        // Engine scope — must destruct before Logger::Shutdown()
        Aes67Engine engine;
        g_pEngine = &engine;
        AudioThreadStats stats;
        engine.RunBlocking(args.config, stats, args.netConfig);
        g_pEngine = nullptr;

        // Final stats
        DWORD total = stats.totalFrames.load(std::memory_order_relaxed);
        DWORD active = stats.nonSilentFrames.load(std::memory_order_relaxed);
        glitches = stats.glitchCount.load(std::memory_order_relaxed);
        DWORD periods = stats.periodCount.load(std::memory_order_relaxed);
        DWORD overflows = stats.bufferOverflows.load(std::memory_order_relaxed);

        float activePct = total > 0 ? (100.0f * active / total) : 0.0f;
        float runtimeSec = (float)total / (float)(48000 * 2);

        Logger::Instance().Info("--- Final ---");
        Logger::Instance().Info("Total frames:   %lu (%.1f s of audio)", total, runtimeSec);
        Logger::Instance().Info("Non-silent:     %lu (%.1f%%)", active, activePct);
        Logger::Instance().Info("Periods:        %lu", periods);
        Logger::Instance().Info("Glitches:       %lu %s", glitches,
            glitches == 0 ? "(clean!)" : "");
        Logger::Instance().Info("Buf overflows:  %lu %s", overflows,
            overflows == 0 ? "(clean!)" : "");
        Logger::Instance().Info("Packets sent:   %llu", engine.GetPacketsSent());
        Logger::Instance().Info("Bytes dropped:  %llu", engine.GetBytesDropped());
        if (args.netConfig.enableRx) {
            Logger::Instance().Info("Packets rcvd:   %llu", engine.GetPacketsRcvd());
            Logger::Instance().Info("Parse errors:   %llu", engine.GetParseErrors());
            Logger::Instance().Info("Rend frames:    %lu", engine.GetRenderStats().totalFrames.load());
        }
    } // Engine destroyed here, before logger shutdown

    Logger::Instance().Shutdown();
    return glitches > 0 ? 2 : 0;
}
