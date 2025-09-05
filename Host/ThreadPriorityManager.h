#pragma once

#include <windows.h>
#include <avrt.h>
#include <string>
#include <atomic>
#include <mutex>
#include <iostream>

namespace ThreadPriorityManager {

// MMCSS task classes for different priorities
enum class MMCSSClass {
    Games,      // Highest priority for gaming input
    Display,    // High priority for display operations
    Audio,      // Audio processing priority
    Playback,   // Media playback priority
    Capture,    // Media capture priority
    None        // No MMCSS priority
};

// Thread priority configuration
struct ThreadPriorityConfig {
    MMCSSClass mmcssClass = MMCSSClass::Games;  // Default to Games for input threads
    int threadPriority = THREAD_PRIORITY_TIME_CRITICAL;  // Win32 thread priority
    bool enableMMCSS = true;                    // Whether to use MMCSS
    bool enableTimeCritical = true;             // Whether to use TIME_CRITICAL priority
    std::string taskName = "InputInjection";    // MMCSS task name
};

// Global priority configuration
extern ThreadPriorityConfig globalPriorityConfig;

// MMCSS handle management
class MMCSSHandle {
private:
    HANDLE mmcssHandle = nullptr;
    bool isElevated = false;
    mutable std::mutex mutex;

public:
    MMCSSHandle() = default;
    ~MMCSSHandle() {
        demote();
    }

    // Delete copy operations
    MMCSSHandle(const MMCSSHandle&) = delete;
    MMCSSHandle& operator=(const MMCSSHandle&) = delete;

    // Move operations
    MMCSSHandle(MMCSSHandle&& other) noexcept {
        // No need to lock in move constructor - other is being moved from
        mmcssHandle = other.mmcssHandle;
        isElevated = other.isElevated;
        other.mmcssHandle = nullptr;
        other.isElevated = false;
    }

    MMCSSHandle& operator=(MMCSSHandle&& other) noexcept {
        if (this != &other) {
            demote();
            // No need to lock other in move assignment - it's being moved from
            mmcssHandle = other.mmcssHandle;
            isElevated = other.isElevated;
            other.mmcssHandle = nullptr;
            other.isElevated = false;
        }
        return *this;
    }

    // Elevate thread priority using MMCSS
    bool elevate(const ThreadPriorityConfig& config = globalPriorityConfig) {
        std::lock_guard<std::mutex> lock(mutex);

        if (isElevated) {
            return true; // Already elevated
        }

        // Get current thread handle
        HANDLE threadHandle = GetCurrentThread();
        if (threadHandle == nullptr) {
            std::cerr << "[ThreadPriorityManager] Failed to get current thread handle" << std::endl;
            return false;
        }

        bool success = true;

        // Set MMCSS priority if enabled
        if (config.enableMMCSS) {
            std::string taskClass;
            switch (config.mmcssClass) {
                case MMCSSClass::Games:
                    taskClass = "Games";
                    break;
                case MMCSSClass::Display:
                    taskClass = "Display";
                    break;
                case MMCSSClass::Audio:
                    taskClass = "Audio";
                    break;
                case MMCSSClass::Playback:
                    taskClass = "Playback";
                    break;
                case MMCSSClass::Capture:
                    taskClass = "Capture";
                    break;
                default:
                    taskClass = "Games"; // Default fallback
                    break;
            }

            // Join MMCSS task
            DWORD taskIndex = 0;
            mmcssHandle = AvSetMmThreadCharacteristicsA(taskClass.c_str(), &taskIndex);
            if (mmcssHandle == nullptr) {
                DWORD error = GetLastError();
                std::string errorMsg;
                switch (error) {
                    case 1552:
                        errorMsg = "MMCSS service not available or paging file too small";
                        break;
                    case 5:
                        errorMsg = "Access denied - MMCSS requires admin privileges";
                        break;
                    case 2:
                        errorMsg = "MMCSS service not running";
                        break;
                    default:
                        errorMsg = "Unknown MMCSS error";
                        break;
                }
                std::cerr << "[ThreadPriorityManager] Failed to set MMCSS priority '" << taskClass
                          << "' (Error: " << error << " - " << errorMsg << ")" << std::endl;
                success = false;
            } else {
                // Set MMCSS priority level
                if (!AvSetMmThreadPriority(mmcssHandle, AVRT_PRIORITY_HIGH)) {
                    DWORD error = GetLastError();
                    std::cerr << "[ThreadPriorityManager] Failed to set MMCSS priority level (Error: " << error << ")" << std::endl;
                    // Continue anyway as basic MMCSS is already set
                }

                std::cout << "[ThreadPriorityManager] Successfully elevated thread with MMCSS class '" << taskClass << "'" << std::endl;
            }
        }

        // Set Win32 thread priority if enabled
        if (config.enableTimeCritical) {
            if (!SetThreadPriority(threadHandle, config.threadPriority)) {
                DWORD error = GetLastError();
                std::cerr << "[ThreadPriorityManager] Failed to set thread priority (Error: " << error << ")" << std::endl;
                success = false;
            } else {
                std::cout << "[ThreadPriorityManager] Successfully set thread priority to TIME_CRITICAL" << std::endl;
            }
        }

        if (success) {
            isElevated = true;
        }

        return success;
    }

    // Demote thread priority back to normal
    void demote() {
        std::lock_guard<std::mutex> lock(mutex);

        if (!isElevated) {
            return;
        }

        // Leave MMCSS task
        if (mmcssHandle != nullptr) {
            if (!AvRevertMmThreadCharacteristics(mmcssHandle)) {
                DWORD error = GetLastError();
                std::cerr << "[ThreadPriorityManager] Failed to revert MMCSS characteristics (Error: " << error << ")" << std::endl;
            } else {
                std::cout << "[ThreadPriorityManager] Successfully reverted MMCSS characteristics" << std::endl;
            }
            mmcssHandle = nullptr;
        }

        // Reset thread priority to normal
        HANDLE threadHandle = GetCurrentThread();
        if (threadHandle != nullptr) {
            if (!SetThreadPriority(threadHandle, THREAD_PRIORITY_NORMAL)) {
                DWORD error = GetLastError();
                std::cerr << "[ThreadPriorityManager] Failed to reset thread priority (Error: " << error << ")" << std::endl;
            } else {
                std::cout << "[ThreadPriorityManager] Successfully reset thread priority to NORMAL" << std::endl;
            }
        }

        isElevated = false;
    }

    // Check if thread is currently elevated
    bool isElevatedPriority() const {
        std::lock_guard<std::mutex> lock(mutex);
        return isElevated;
    }
};

// RAII wrapper for automatic priority management
class ScopedPriorityElevation {
private:
    MMCSSHandle handle;
    bool elevated = false;

public:
    ScopedPriorityElevation(const ThreadPriorityConfig& config = globalPriorityConfig) {
        elevated = handle.elevate(config);
    }

    ~ScopedPriorityElevation() {
        if (elevated) {
            handle.demote();
        }
    }

    bool isElevated() const {
        return elevated;
    }
};

// Global MMCSS handle for input injection threads
extern MMCSSHandle globalInputInjectionHandle;

// Initialize global priority configuration from environment/config
void initializeGlobalConfig();

// Apply priority elevation to current thread (for input injection)
inline bool elevateCurrentThreadForInput() {
    return globalInputInjectionHandle.elevate(globalPriorityConfig);
}

// Check if current thread has elevated priority
inline bool isCurrentThreadElevated() {
    return globalInputInjectionHandle.isElevatedPriority();
}

// Demote current thread priority
inline void demoteCurrentThread() {
    globalInputInjectionHandle.demote();
}

// Configuration API
void setMMCSSClass(MMCSSClass mmcssClass);
void setThreadPriority(int priority);
void enableMMCSS(bool enable);
void enableTimeCritical(bool enable);
void setTaskName(const std::string& taskName);

} // namespace ThreadPriorityManager
