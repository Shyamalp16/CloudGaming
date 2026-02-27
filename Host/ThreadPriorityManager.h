#pragma once

#include <windows.h>
#include <avrt.h>
#include <string>
#include <atomic>
#include <mutex>
#include <iostream>
#include <sddl.h>  // For SID functions

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
    bool fallbackToWin32Priority = true;        // Fallback to Win32 priority if MMCSS fails
    bool showDiagnosticsOnFailure = true;       // Show detailed diagnostics on MMCSS failure
    bool retryMMCSSOnFailure = false;           // Retry MMCSS after initial failure
    int mmcssRetryDelayMs = 1000;               // Delay before retrying MMCSS
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

    // Check if current process has administrator privileges
    static bool isRunningAsAdministrator() {
        BOOL isAdmin = FALSE;
        PSID adminGroup = nullptr;
        SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;

        if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                   DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
            if (!CheckTokenMembership(nullptr, adminGroup, &isAdmin)) {
                isAdmin = FALSE;
            }
            FreeSid(adminGroup);
        }
        return isAdmin == TRUE;
    }

    // Diagnose MMCSS issues and provide detailed information
    static void diagnoseMMCSSIssues() {
        std::cout << "[ThreadPriorityManager] MMCSS Diagnostics:" << std::endl;

        // Check administrator privileges
        bool isAdmin = isRunningAsAdministrator();
        std::cout << "  - Running as Administrator: " << (isAdmin ? "YES" : "NO") << std::endl;
        if (!isAdmin) {
            std::cout << "  - WARNING: MMCSS typically requires administrator privileges" << std::endl;
        }

        // Check MMCSS service status
        SC_HANDLE scManager = OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (scManager) {
            SC_HANDLE service = OpenService(scManager, TEXT("MMCSS"), SERVICE_QUERY_STATUS);
            if (service) {
                SERVICE_STATUS status;
                if (QueryServiceStatus(service, &status)) {
                    std::cout << "  - MMCSS Service Status: "
                              << (status.dwCurrentState == SERVICE_RUNNING ? "RUNNING" : "NOT RUNNING")
                              << std::endl;
                }
                CloseServiceHandle(service);
            } else {
                std::cout << "  - MMCSS Service: Could not query status" << std::endl;
            }
            CloseServiceHandle(scManager);
        }

        // Check avrt.dll availability
        HMODULE avrtModule = LoadLibraryA("avrt.dll");
        if (avrtModule) {
            std::cout << "  - avrt.dll: Available" << std::endl;
            FreeLibrary(avrtModule);
        } else {
            std::cout << "  - avrt.dll: NOT FOUND" << std::endl;
        }

        // Get system memory info
        MEMORYSTATUSEX memStatus;
        memStatus.dwLength = sizeof(memStatus);
        if (GlobalMemoryStatusEx(&memStatus)) {
            std::cout << "  - System Memory: " << (memStatus.ullTotalPhys / (1024 * 1024)) << " MB total, "
                      << (memStatus.ullAvailPhys / (1024 * 1024)) << " MB available" << std::endl;
            std::cout << "  - Page File: " << (memStatus.ullTotalPageFile / (1024 * 1024)) << " MB total, "
                      << (memStatus.ullAvailPageFile / (1024 * 1024)) << " MB available" << std::endl;
        }
    }

    // Elevate thread priority using MMCSS
    bool elevate(const ThreadPriorityConfig& config = globalPriorityConfig) {
        std::lock_guard<std::mutex> lock(mutex);
        static std::atomic<bool> s_mmcssUnavailable{ false };

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
        bool mmcssSuccess = false;

        // Set MMCSS priority if enabled and runtime indicates availability.
        if (config.enableMMCSS && !s_mmcssUnavailable.load(std::memory_order_relaxed)) {
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
                bool showDiagnostics = false;

                switch (error) {
                    case 1552:
                        errorMsg = "MMCSS service not available or paging file too small";
                        showDiagnostics = true;
                        s_mmcssUnavailable.store(true, std::memory_order_relaxed);
                        break;
                    case 5:
                        errorMsg = "Access denied - MMCSS requires admin privileges";
                        showDiagnostics = true;
                        s_mmcssUnavailable.store(true, std::memory_order_relaxed);
                        break;
                    case 2:
                        errorMsg = "MMCSS service not running";
                        showDiagnostics = true;
                        s_mmcssUnavailable.store(true, std::memory_order_relaxed);
                        break;
                    case 87:
                        errorMsg = "Invalid parameter - MMCSS task class may not be supported";
                        std::cerr << "[ThreadPriorityManager] MMCSS task class '" << taskClass
                                  << "' may not be supported. This can happen with certain Windows configurations." << std::endl;
                        break;
                    case 50:
                        errorMsg = "MMCSS not supported on this platform";
                        s_mmcssUnavailable.store(true, std::memory_order_relaxed);
                        break;
                    default:
                        errorMsg = "Unknown MMCSS error";
                        break;
                }

                std::cerr << "[ThreadPriorityManager] Failed to set MMCSS priority '" << taskClass
                          << "' (Error: " << error << " - " << errorMsg << ")" << std::endl;

                if (showDiagnostics) {
                    diagnoseMMCSSIssues();
                }

                // Try fallback MMCSS task classes for error 87 (invalid parameter)
                if (error == 87 && taskClass != "Games") {
                    std::cerr << "[ThreadPriorityManager] Trying fallback MMCSS task class 'Games'..." << std::endl;
                    mmcssHandle = AvSetMmThreadCharacteristicsA("Games", &taskIndex);
                    if (mmcssHandle != nullptr) {
                        std::cout << "[ThreadPriorityManager] Successfully elevated thread with MMCSS class 'Games' (fallback)" << std::endl;
                        mmcssSuccess = true;

                        // Set MMCSS priority level
                        if (!AvSetMmThreadPriority(mmcssHandle, AVRT_PRIORITY_HIGH)) {
                            DWORD priorityError = GetLastError();
                            std::cerr << "[ThreadPriorityManager] Failed to set MMCSS priority level (Error: " << priorityError << ")" << std::endl;
                        }
                    } else {
                        std::cerr << "[ThreadPriorityManager] Fallback MMCSS task class also failed" << std::endl;
                        mmcssSuccess = false;
                    }
                } else {
                    // Don't mark as complete failure - we can still try Win32 priority
                    mmcssSuccess = false;
                }
            } else {
                mmcssSuccess = true;

                // Set MMCSS priority level
                if (!AvSetMmThreadPriority(mmcssHandle, AVRT_PRIORITY_HIGH)) {
                    DWORD error = GetLastError();
                    std::cerr << "[ThreadPriorityManager] Failed to set MMCSS priority level (Error: " << error << ")" << std::endl;
                    // Continue anyway as basic MMCSS is already set
                }

                std::cout << "[ThreadPriorityManager] Successfully elevated thread with MMCSS class '" << taskClass << "'" << std::endl;
            }
        } else if (config.enableMMCSS && s_mmcssUnavailable.load(std::memory_order_relaxed)) {
            // MMCSS has already failed with non-recoverable runtime conditions.
            // Avoid repeated attempts and rely on Win32 thread priority fallback.
            mmcssSuccess = false;
        }

        // Set Win32 thread priority (always try this as fallback)
        if (!SetThreadPriority(threadHandle, config.threadPriority)) {
            DWORD error = GetLastError();
            std::cerr << "[ThreadPriorityManager] Failed to set thread priority (Error: " << error << ")" << std::endl;

            // If both MMCSS and Win32 priority failed, this is a complete failure
            if (!mmcssSuccess) {
                success = false;
            }
        } else {
            if (config.enableTimeCritical) {
                std::cout << "[ThreadPriorityManager] Successfully set thread priority to TIME_CRITICAL" << std::endl;
            } else {
                std::cout << "[ThreadPriorityManager] Successfully set thread priority to HIGH" << std::endl;
            }
            // At least Win32 priority succeeded
            success = true;
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
