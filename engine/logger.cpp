#include "logger.h"
#include <windows.h>
#include <cstdio>
#include <ctime>
#include <cstring>

Logger& Logger::Instance() {
    static Logger s_instance;
    return s_instance;
}

bool Logger::Init(const wchar_t* filePath, LogLevel minLevel) {
    if (m_initialized) return true;
    InitializeCriticalSection(&m_lock);
    m_minLevel = minLevel;

    if (filePath && filePath[0]) {
        _wfopen_s(&m_file, filePath, L"w");
        if (!m_file) {
            // Non-fatal: continue with console only
            printf("[Logger] WARNING: Cannot open log file, console only\n");
        }
    }
    m_initialized = true;
    return true;
}

void Logger::Shutdown() {
    if (!m_initialized) return;
    EnterCriticalSection(&m_lock);
    if (m_file) {
        fclose(m_file);
        m_file = nullptr;
    }
    m_initialized = false;
    LeaveCriticalSection(&m_lock);
    DeleteCriticalSection(&m_lock);
}

void Logger::Log(LogLevel level, const char* format, ...) {
    if (!m_initialized) return;
    if (level < m_minLevel) return;

    char buf[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';

    LogImpl(level, buf);
}

void Logger::LogImpl(LogLevel level, const char* message) {
    EnterCriticalSection(&m_lock);

    // Timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);
    const char* levelStr = "???";
    switch (level) {
        case LogLevel::Debug:   levelStr = "DBG"; break;
        case LogLevel::Info:    levelStr = "INF"; break;
        case LogLevel::Warning: levelStr = "WRN"; break;
        case LogLevel::Error:   levelStr = "ERR"; break;
    }

    char line[4608];
    int n = snprintf(line, sizeof(line),
        "[%02d:%02d:%02d.%03d] %s %s\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        levelStr, message);
    if (n < 0) n = 0;

    // Console
    printf("%s", line);
    fflush(stdout);

    // File (if open)
    if (m_file) {
        fputs(line, m_file);
        fflush(m_file);
    }

    LeaveCriticalSection(&m_lock);
}

void Logger::Debug(const char* fmt, ...) {
    if (m_minLevel > LogLevel::Debug) return;
    char buf[4096]; va_list args; va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    LogImpl(LogLevel::Debug, buf);
}
void Logger::Info(const char* fmt, ...) {
    if (m_minLevel > LogLevel::Info) return;
    char buf[4096]; va_list args; va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    LogImpl(LogLevel::Info, buf);
}
void Logger::Warn(const char* fmt, ...) {
    if (m_minLevel > LogLevel::Warning) return;
    char buf[4096]; va_list args; va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    LogImpl(LogLevel::Warning, buf);
}
void Logger::Error(const char* fmt, ...) {
    char buf[4096]; va_list args; va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    LogImpl(LogLevel::Error, buf);
}
