#pragma once
#include <string>
#include <system_error>
// Winsock error codes are referenced; ensure correct include order
#include <winsock2.h>
#include <windows.h>

// Avoid Windows macro collisions with enum names
#ifdef ERROR
#undef ERROR
#endif

/**
 * @brief Centralized error reporting utilities for consistent error handling
 *
 * This module provides standardized error reporting functions that replace
 * inconsistent usage of std::system_category().message and FormatMessageA.
 * It ensures uniform error formatting and logging across the application.
 */
namespace ErrorUtils {

/**
 * @brief Error severity levels for consistent categorization
 */
enum class ErrorSeverity {
    INFO,       // Informational messages
    WARNING,    // Non-critical issues
    ERROR,      // Critical errors requiring attention
    FATAL       // System-critical errors that may cause termination
};

/**
 * @brief Error categories for better organization
 */
enum class ErrorCategory {
    SYSTEM,         // Windows system errors
    NETWORK,        // Network-related errors
    INPUT,          // Input handling errors
    VIDEO,          // Video processing errors
    AUDIO,          // Audio processing errors
    CONFIG,         // Configuration errors
    MEMORY,         // Memory management errors
    THREAD,         // Threading/synchronization errors
    IO,             // File/disk I/O errors
    GENERIC         // General/other errors
};

/**
 * @brief Get a human-readable description of a Windows system error code
 *
 * Replaces inconsistent usage of FormatMessageA and std::system_category().message
 *
 * @param errorCode Windows error code (GetLastError() or specific error)
 * @param defaultMessage Default message if error code cannot be resolved
 * @return Formatted error message string
 */
std::string getSystemErrorMessage(DWORD errorCode, const std::string& defaultMessage = "Unknown system error");

/**
 * @brief Get a human-readable description of the last Windows system error
 *
 * Convenience function that calls GetLastError() internally
 *
 * @param defaultMessage Default message if error code cannot be resolved
 * @return Formatted error message string
 */
std::string getLastSystemErrorMessage(const std::string& defaultMessage = "Unknown system error");

/**
 * @brief Format a std::error_code into a consistent error message
 *
 * @param ec The error code to format
 * @param context Optional context information about where the error occurred
 * @return Formatted error message string
 */
std::string formatErrorCode(const std::error_code& ec, const std::string& context = "");

/**
 * @brief Format an HRESULT into a consistent error message
 *
 * @param hr The HRESULT to format
 * @param context Optional context information about where the error occurred
 * @return Formatted error message string
 */
std::string formatHResult(HRESULT hr, const std::string& context = "");

/**
 * @brief Create a standardized error message with category and severity
 *
 * @param severity Error severity level
 * @param category Error category
 * @param message Primary error message
 * @param details Optional detailed information
 * @return Formatted error message with consistent prefix
 */
std::string createErrorMessage(ErrorSeverity severity, ErrorCategory category,
                              const std::string& message, const std::string& details = "");

/**
 * @brief Log an error message with consistent formatting and optional output
 *
 * @param severity Error severity level
 * @param category Error category
 * @param message Primary error message
 * @param details Optional detailed information
 * @param logToStderr Whether to also output to stderr
 */
void logError(ErrorSeverity severity, ErrorCategory category,
              const std::string& message, const std::string& details = "",
              bool logToStderr = true);

/**
 * @brief Convert ErrorSeverity enum to string
 */
std::string severityToString(ErrorSeverity severity);

/**
 * @brief Convert ErrorCategory enum to string
 */
std::string categoryToString(ErrorCategory category);

/**
 * @brief Get a stack trace if available (platform-dependent)
 *
 * @return Stack trace string or empty string if not available
 */
std::string getStackTrace();

/**
 * @brief Check if a Windows error code indicates a recoverable condition
 *
 * @param errorCode Windows error code to check
 * @return true if the error is recoverable, false if it's fatal
 */
bool isRecoverableError(DWORD errorCode);

/**
 * @brief Check if an HRESULT indicates success
 *
 * @param hr HRESULT to check
 * @return true if the HRESULT indicates success
 */
bool isSuccess(HRESULT hr);

/**
 * @brief Check if an HRESULT indicates failure
 *
 * @param hr HRESULT to check
 * @return true if the HRESULT indicates failure
 */
bool isFailure(HRESULT hr);

/**
 * @brief Get the severity level for a Windows error code
 *
 * @param errorCode Windows error code
 * @return Appropriate severity level for the error
 */
ErrorSeverity getErrorSeverity(DWORD errorCode);

/**
 * @brief Get the category for a Windows error code
 *
 * @param errorCode Windows error code
 * @return Appropriate category for the error
 */
ErrorCategory getErrorCategory(DWORD errorCode);

// Convenience macros for common error reporting patterns

/**
 * @brief Log a system error with the last error code
 */
#define LOG_SYSTEM_ERROR(message) \
    ErrorUtils::logError(ErrorUtils::ErrorSeverity::ERROR, ErrorUtils::ErrorCategory::SYSTEM, \
                        message, ErrorUtils::getLastSystemErrorMessage())

/**
 * @brief Log an input error with custom details
 */
#define LOG_INPUT_ERROR(message, details) \
    ErrorUtils::logError(ErrorUtils::ErrorSeverity::ERROR, ErrorUtils::ErrorCategory::INPUT, \
                        message, details)

/**
 * @brief Log a network error with error code
 */
#define LOG_NETWORK_ERROR(message, errorCode) \
    ErrorUtils::logError(ErrorUtils::ErrorSeverity::ERROR, ErrorUtils::ErrorCategory::NETWORK, \
                        message, ErrorUtils::getSystemErrorMessage(errorCode))

/**
 * @brief Log a warning with custom category
 */
#define LOG_WARNING(category, message) \
    ErrorUtils::logError(ErrorUtils::ErrorSeverity::WARNING, category, message)

/**
 * @brief Log an informational message
 */
#define LOG_INFO(category, message) \
    ErrorUtils::logError(ErrorUtils::ErrorSeverity::INFO, category, message)

/**
 * @brief Log a fatal error (may cause application termination)
 */
#define LOG_FATAL(category, message, details) \
    ErrorUtils::logError(ErrorUtils::ErrorSeverity::FATAL, category, message, details)

} // namespace ErrorUtils
