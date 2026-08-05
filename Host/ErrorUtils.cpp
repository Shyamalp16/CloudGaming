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


} // namespace ErrorUtils
