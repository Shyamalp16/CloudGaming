#pragma once
#ifndef LOGGING_H
#define LOGGING_H

#include <string>
#include <iostream>
#include <atomic>
#include <mutex>
#include <chrono>

/**
 * @brief Lightweight logging facility with compile-time and runtime level control
 *
 * Supports:
 * - Compile-time log levels via NDEBUG macros
 * - Runtime configurable levels via INPUT_LOG_LEVEL environment variable
 * - Thread-safe logging with minimal overhead
 * - Production defaults to WARN/ERROR only
 *
 * Usage:
 *   LOG_ERROR("Something went wrong");
 *   LOG_WARN("Warning message");
 *   LOG_INFO("Info message");
 *   LOG_DEBUG("Debug info");
 *   LOG_TRACE("Trace level");
 */

namespace Logging {

// Log levels in order of increasing verbosity
enum Level {
    LEVEL_NONE = 0,       //!< No logging
    LEVEL_ERROR_LOG = 1,  //!< Critical errors only
    LEVEL_WARN_LOG = 2,   //!< Warnings and errors
    LEVEL_INFO_LOG = 3,   //!< General information
    LEVEL_DEBUG_LOG = 4,  //!< Debug information
    LEVEL_TRACE_LOG = 5   //!< Detailed trace information
};

// Convert string to log level
Level stringToLevel(const std::string& level);

// Convert log level to string
std::string levelToString(Level level);

// Get current log level (runtime configurable)
Level getCurrentLevel();

// Set log level at runtime
void setLevel(Level level);

// Initialize logging system (call once at startup)
void initialize();

// Core logging function
void log(Level level, const std::string& message);

} // namespace Logging

// Convenience macros that respect both compile-time and runtime levels
#define LOG_ERROR(message) Logging::log(Logging::LEVEL_ERROR_LOG, message)
#define LOG_WARN(message)  Logging::log(Logging::LEVEL_WARN_LOG, message)
#define LOG_INFO(message)  Logging::log(Logging::LEVEL_INFO_LOG, message)
#define LOG_DEBUG(message) Logging::log(Logging::LEVEL_DEBUG_LOG, message)
#define LOG_TRACE(message) Logging::log(Logging::LEVEL_TRACE_LOG, message)

// Legacy compatibility macros (deprecated - use LOG_* macros above)
#define LOG(level, message) Logging::log(level, message)

#endif // LOGGING_H
