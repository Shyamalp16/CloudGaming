#include "ErrorUtils.h"
#include <iostream>
#include <sstream>
#include <iomanip>

namespace ErrorUtils {

std::string getSystemErrorMessage(DWORD errorCode, const std::string& defaultMessage) {
    if (errorCode == 0) {
        return "No error";
    }

    LPSTR messageBuffer = nullptr;
    size_t size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&messageBuffer), 0, nullptr);

    std::string result;
    if (size > 0 && messageBuffer) {
        // Remove trailing whitespace and newlines
        std::string msg(messageBuffer);
        msg.erase(msg.find_last_not_of(" \t\n\r\f\v") + 1);

        std::stringstream ss;
        ss << "Windows Error " << errorCode << ": " << msg;
        result = ss.str();

        LocalFree(messageBuffer);
    } else {
        std::stringstream ss;
        ss << "Windows Error " << errorCode << ": " << defaultMessage;
        result = ss.str();
    }

    return result;
}

std::string getLastSystemErrorMessage(const std::string& defaultMessage) {
    return getSystemErrorMessage(GetLastError(), defaultMessage);
}

std::string formatErrorCode(const std::error_code& ec, const std::string& context) {
    std::stringstream ss;
    ss << "Error code " << ec.value() << " (" << ec.category().name() << ")";
    if (!ec.message().empty()) {
        ss << ": " << ec.message();
    }
    if (!context.empty()) {
        ss << " [Context: " << context << "]";
    }
    return ss.str();
}

std::string formatHResult(HRESULT hr, const std::string& context) {
    std::stringstream ss;
    ss << "HRESULT 0x" << std::hex << std::uppercase << hr;

    // Try to get a description for common HRESULT values
    if (FAILED(hr)) {
        ss << " (FAILED)";
        if (hr == E_FAIL) ss << " - Unspecified error";
        else if (hr == E_INVALIDARG) ss << " - Invalid argument";
        else if (hr == E_OUTOFMEMORY) ss << " - Out of memory";
        else if (hr == E_ACCESSDENIED) ss << " - Access denied";
        else if (hr == E_NOTIMPL) ss << " - Not implemented";
    } else {
        ss << " (SUCCESS)";
    }

    if (!context.empty()) {
        ss << " [Context: " << context << "]";
    }

    return ss.str();
}

std::string createErrorMessage(ErrorSeverity severity, ErrorCategory category,
                              const std::string& message, const std::string& details) {
    std::stringstream ss;
    ss << "[" << severityToString(severity) << "/" << categoryToString(category) << "] ";
    ss << message;
    if (!details.empty()) {
        ss << " - " << details;
    }
    return ss.str();
}

void logError(ErrorSeverity severity, ErrorCategory category,
              const std::string& message, const std::string& details,
              bool logToStderr) {
    std::string fullMessage = createErrorMessage(severity, category, message, details);

    // Always log to cout with appropriate stream
    if (severity == ErrorSeverity::ERROR || severity == ErrorSeverity::FATAL) {
        std::cerr << fullMessage << std::endl;
    } else if (severity == ErrorSeverity::WARNING) {
        std::cout << fullMessage << std::endl;
    } else {
        std::cout << fullMessage << std::endl;
    }

    // Optionally also log to stderr for critical errors
    if (logToStderr && (severity == ErrorSeverity::ERROR || severity == ErrorSeverity::FATAL)) {
        // Already logged to stderr above via std::cerr
    }
}

std::string severityToString(ErrorSeverity severity) {
    switch (severity) {
        case ErrorSeverity::INFO: return "INFO";
        case ErrorSeverity::WARNING: return "WARN";
        case ErrorSeverity::ERROR: return "ERROR";
        case ErrorSeverity::FATAL: return "FATAL";
        default: return "UNKNOWN";
    }
}

std::string categoryToString(ErrorCategory category) {
    switch (category) {
        case ErrorCategory::SYSTEM: return "SYSTEM";
        case ErrorCategory::NETWORK: return "NETWORK";
        case ErrorCategory::INPUT: return "INPUT";
        case ErrorCategory::VIDEO: return "VIDEO";
        case ErrorCategory::AUDIO: return "AUDIO";
        case ErrorCategory::CONFIG: return "CONFIG";
        case ErrorCategory::MEMORY: return "MEMORY";
        case ErrorCategory::THREAD: return "THREAD";
        case ErrorCategory::IO: return "IO";
        case ErrorCategory::GENERIC: return "GENERIC";
        default: return "UNKNOWN";
    }
}

std::string getStackTrace() {
    // Basic stack trace implementation
    // In a production system, you might want to use a more sophisticated
    // stack trace library like Boost.Stacktrace or similar
    return "[Stack trace not available in this build]";
}

bool isRecoverableError(DWORD errorCode) {
    // Common recoverable Windows error codes
    switch (errorCode) {
        case ERROR_TIMEOUT:
        case ERROR_IO_PENDING:
        case ERROR_BUSY:
        case ERROR_NOT_READY:
        case ERROR_LOCK_VIOLATION:
        case ERROR_SHARING_VIOLATION:
        case ERROR_OPERATION_ABORTED:
        case ERROR_REQUEST_ABORTED:
        case WSAECONNREFUSED:
        case WSAECONNABORTED:
        case WSAECONNRESET:
        case ERROR_HOST_UNREACHABLE:
        case ERROR_NETWORK_UNREACHABLE:
            return true;
        default:
            return false;
    }
}

bool isSuccess(HRESULT hr) {
    return SUCCEEDED(hr);
}

bool isFailure(HRESULT hr) {
    return FAILED(hr);
}

ErrorSeverity getErrorSeverity(DWORD errorCode) {
    if (errorCode == 0) {
        return ErrorSeverity::INFO;
    }

    // Critical system errors
    if (errorCode == ERROR_OUTOFMEMORY ||
        errorCode == ERROR_STACK_OVERFLOW ||
        errorCode == ERROR_INVALID_HANDLE ||
        errorCode == ERROR_NOT_ENOUGH_MEMORY) {
        return ErrorSeverity::FATAL;
    }

    // Recoverable errors
    if (isRecoverableError(errorCode)) {
        return ErrorSeverity::WARNING;
    }

    // Default to error for most system errors
    return ErrorSeverity::ERROR;
}

ErrorCategory getErrorCategory(DWORD errorCode) {
    // Network-related errors
    if (errorCode == WSAECONNREFUSED ||
        errorCode == WSAECONNRESET ||
        errorCode == WSAETIMEDOUT ||
        errorCode == WSAENOTCONN ||
        errorCode == WSAECONNABORTED) {
        return ErrorCategory::NETWORK;
    }

    // Memory-related errors
    if (errorCode == ERROR_OUTOFMEMORY ||
        errorCode == ERROR_NOT_ENOUGH_MEMORY ||
        errorCode == ERROR_STACK_OVERFLOW) {
        return ErrorCategory::MEMORY;
    }

    // I/O related errors
    if (errorCode == ERROR_FILE_NOT_FOUND ||
        errorCode == ERROR_PATH_NOT_FOUND ||
        errorCode == ERROR_ACCESS_DENIED ||
        errorCode == ERROR_SHARING_VIOLATION ||
        errorCode == ERROR_DISK_FULL) {
        return ErrorCategory::IO;
    }

    // Threading errors
    if (errorCode == ERROR_INVALID_HANDLE ||
        errorCode == ERROR_LOCK_VIOLATION) {
        return ErrorCategory::THREAD;
    }

    // Default to system for most Windows errors
    return ErrorCategory::SYSTEM;
}

} // namespace ErrorUtils
