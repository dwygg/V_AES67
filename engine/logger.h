#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>

enum class LogLevel { Debug = 0, Info = 1, Warning = 2, Error = 3 };

// Thread-safe singleton logger. Audio thread must NOT call this — only main thread.
class Logger {
public:
    static Logger& Instance();

    bool Init(const wchar_t* filePath = nullptr, LogLevel minLevel = LogLevel::Info);
    void SetLevel(LogLevel level) { m_minLevel = level; }
    void Shutdown();

    void Log(LogLevel level, const char* format, ...);

    // Convenience
    void Debug(const char* fmt, ...);
    void Info(const char* fmt, ...);
    void Warn(const char* fmt, ...);
    void Error(const char* fmt, ...);

private:
    Logger() = default;
    ~Logger() { Shutdown(); }
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void LogImpl(LogLevel level, const char* message);

    FILE*      m_file       = nullptr;
    LogLevel   m_minLevel   = LogLevel::Info;
    CRITICAL_SECTION m_lock;
    bool       m_initialized = false;
};
