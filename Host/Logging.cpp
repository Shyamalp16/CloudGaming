#include "Logging.h"
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <iomanip>

namespace Logging {

namespace {
// Thread-safe static variables
std::atomic<Level> g_currentLevel{LEVEL_WARN_LOG}; // Default to WARN in production
std::mutex g_logMutex;
bool g_initialized = false;

// Color codes for console output (optional)
#ifdef _WIN32
    #define LOG_COLOR_RED    ""
    #define LOG_COLOR_YELLOW ""
    #define LOG_COLOR_BLUE   ""
    #define LOG_COLOR_RESET  ""
#else
    #define LOG_COLOR_RED    "\033[31m"
    #define LOG_COLOR_YELLOW "\033[33m"
    #define LOG_COLOR_BLUE   "\033[34m"
    #define LOG_COLOR_RESET  "\033[0m"
#endif

// Get timestamp string
std::string getTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    ss << "." << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

// Get level prefix with color
std::string getLevelPrefix(Level level) {
    switch (level) {
        case LEVEL_ERROR_LOG:
            return std::string(LOG_COLOR_RED) + "[ERROR]" + LOG_COLOR_RESET;
        case LEVEL_WARN_LOG:
            return std::string(LOG_COLOR_YELLOW) + "[WARN] " + LOG_COLOR_RESET;
        case LEVEL_INFO_LOG:
            return std::string(LOG_COLOR_BLUE) + "[INFO] " + LOG_COLOR_RESET;
        case LEVEL_DEBUG_LOG:
            return "[DEBUG]";
        case LEVEL_TRACE_LOG:
            return "[TRACE]";
        default:
            return "[UNKNOWN]";
    }
}

} // anonymous namespace

Level stringToLevel(const std::string& level) {
    std::string upperLevel = level;
    for (char& c : upperLevel) {
        c = std::toupper(c);
    }

    if (upperLevel == "NONE") return LEVEL_NONE;
    if (upperLevel == "ERROR") return LEVEL_ERROR_LOG;
    if (upperLevel == "WARN") return LEVEL_WARN_LOG;
    if (upperLevel == "INFO") return LEVEL_INFO_LOG;
    if (upperLevel == "DEBUG") return LEVEL_DEBUG_LOG;
    if (upperLevel == "TRACE") return LEVEL_TRACE_LOG;

    // Default to WARN for invalid levels
    return LEVEL_WARN_LOG;
}

std::string levelToString(Level level) {
    switch (level) {
        case LEVEL_NONE: return "NONE";
        case LEVEL_ERROR_LOG: return "ERROR";
        case LEVEL_WARN_LOG: return "WARN";
        case LEVEL_INFO_LOG: return "INFO";
        case LEVEL_DEBUG_LOG: return "DEBUG";
        case LEVEL_TRACE_LOG: return "TRACE";
        default: return "UNKNOWN";
    }
}

Level getCurrentLevel() {
    return g_currentLevel.load();
}

void setLevel(Level level) {
    g_currentLevel.store(level);
}

void initialize() {
    if (g_initialized) {
        return;
    }

    // Initialize from environment variable
    const char* envLevel = std::getenv("INPUT_LOG_LEVEL");
    if (envLevel) {
        Level level = stringToLevel(envLevel);
        setLevel(level);
        std::cout << "[Logging] Initialized with level: " << levelToString(level) << std::endl;
    } else {
        // Default level based on build type
#ifndef NDEBUG
        setLevel(LEVEL_DEBUG_LOG); // Debug build defaults to DEBUG
        std::cout << "[Logging] Debug build initialized with level: DEBUG" << std::endl;
#else
        setLevel(LEVEL_WARN_LOG);  // Release build defaults to WARN
        // Don't log in release builds to avoid startup noise
#endif
    }

    g_initialized = true;
}

void log(Level level, const std::string& message) {
    // Check if we should log this level
    if (level > getCurrentLevel()) {
        return;
    }

    // Thread-safe logging
    std::lock_guard<std::mutex> lock(g_logMutex);

    // Format: [TIMESTAMP] [LEVEL] message
    std::cout << "[" << getTimestamp() << "] " << getLevelPrefix(level) << " " << message << std::endl;

    // In debug builds, also flush immediately for better debugging
#ifndef NDEBUG
    std::cout.flush();
#endif
}



} // namespace Logging
