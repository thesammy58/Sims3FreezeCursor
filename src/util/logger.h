#pragma once
#include <Windows.h>
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <atomic>

namespace S3FC {

// LOGGER - Thread-safe file logger, uses CRITICAL_SECTION to avoid std::mutex
// static init issues. Trimmed from S3MM's util/logger.h: drops LogHookTrace/
// LogRaw (no per-hook-invocation tracing or raw data channel needed here -
// this mod has exactly one hook to log about).
class Logger {
  public:
    enum class Level { Debug, Info, Warning, Error };

    static Logger& Get() {
        static Logger instance;
        return instance;
    }

    void Initialize(const std::string& filename = "Sims3FreezeCursor.log") {
        if (m_initialized) return;

        if (!m_csInitialized) {
            InitializeCriticalSection(&m_cs);
            m_csInitialized = true;
        }

        EnterCriticalSection(&m_cs);

        m_file.open(filename, std::ios::out | std::ios::trunc);
        m_initialized = m_file.is_open();

        LeaveCriticalSection(&m_cs);
    }

    void Shutdown() {
        if (!m_csInitialized) return;

        EnterCriticalSection(&m_cs);
        if (m_file.is_open()) {
            m_file.flush();
            m_file.close();
        }
        m_initialized = false;
        LeaveCriticalSection(&m_cs);
    }

    void Log(Level level, const std::string& message) {
        if (level < m_minLevel.load(std::memory_order_relaxed)) return;
        Write(level, message);
    }

  private:
    void Write(Level level, const std::string& message) {
        if (!m_initialized || !m_csInitialized) return;

        EnterCriticalSection(&m_cs);
        if (!m_initialized) {
            LeaveCriticalSection(&m_cs);
            return;
        }

        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::tm tm_buf;
        localtime_s(&tm_buf, &time);

        m_file << std::put_time(&tm_buf, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count() << " [" << LevelToString(level) << "] " << message << '\n';

        // FLUSH POLICY. This was std::endl, which is '\n' plus an
        // unconditional flush - so every line, however trivial, cost a
        // synchronous WriteFile. Not free: the log sits next to the .asi in
        // the game folder where a real-time scanner may inspect each write,
        // and the cost lands on whichever thread did the logging.
        //
        // Errors still flush immediately. A crash capture that loses its last
        // lines to the crash it is reporting is worth nothing, and a fault is
        // not the moment to economise on syscalls. Everything else coalesces
        // to at most one flush every 250ms, so a burst costs one write rather
        // than one write each, and a hard exit loses at most a quarter second
        // of INFO.
        const DWORD nowTick = GetTickCount();
        if (level >= Level::Error || (nowTick - m_lastFlushTick) >= kFlushIntervalMs) {
            m_file.flush();
            m_lastFlushTick = nowTick;
        }

        LeaveCriticalSection(&m_cs);
    }

    Logger() = default;
    ~Logger() {
        Shutdown();
        if (m_csInitialized) { DeleteCriticalSection(&m_cs); }
    }

  public:
    void SetMinLevel(Level level) { m_minLevel.store(level, std::memory_order_relaxed); }
    bool IsLevelEnabled(Level level) const { return level >= m_minLevel.load(std::memory_order_relaxed); }

  private:
    static const char* LevelToString(Level level) {
        switch (level) {
        case Level::Debug:
            return "DEBUG";
        case Level::Info:
            return "INFO";
        case Level::Warning:
            return "WARN";
        case Level::Error:
            return "ERROR";
        default:
            return "?";
        }
    }

    static constexpr DWORD kFlushIntervalMs = 250;

    std::ofstream m_file;
    DWORD m_lastFlushTick = 0;
    CRITICAL_SECTION m_cs;
    bool m_csInitialized = false;
    bool m_initialized = false;
#ifdef NDEBUG
    std::atomic<Level> m_minLevel{Level::Info};
#else
    std::atomic<Level> m_minLevel{Level::Debug};
#endif
};

#define LOG_DEBUG(msg)                                                                          \
    do {                                                                                        \
        if (S3FC::Logger::Get().IsLevelEnabled(S3FC::Logger::Level::Debug)) { S3FC::Logger::Get().Log(S3FC::Logger::Level::Debug, msg); } \
    } while (0)
#define LOG_INFO(msg)                                                                          \
    do {                                                                                        \
        if (S3FC::Logger::Get().IsLevelEnabled(S3FC::Logger::Level::Info)) { S3FC::Logger::Get().Log(S3FC::Logger::Level::Info, msg); } \
    } while (0)
#define LOG_WARN(msg)                                                                             \
    do {                                                                                           \
        if (S3FC::Logger::Get().IsLevelEnabled(S3FC::Logger::Level::Warning)) { S3FC::Logger::Get().Log(S3FC::Logger::Level::Warning, msg); } \
    } while (0)
#define LOG_ERROR(msg)                                                                          \
    do {                                                                                         \
        if (S3FC::Logger::Get().IsLevelEnabled(S3FC::Logger::Level::Error)) { S3FC::Logger::Get().Log(S3FC::Logger::Level::Error, msg); } \
    } while (0)

// HEX FORMATTING HELPER
inline std::string ToHex(uintptr_t value) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << value;
    return ss.str();
}

} // namespace S3FC
