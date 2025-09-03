#include "ThreadPriorityManager.h"
#include <cstdlib>
#include <sstream>

namespace ThreadPriorityManager {

// Global priority configuration
ThreadPriorityConfig globalPriorityConfig;

// Global MMCSS handle for input injection threads
MMCSSHandle globalInputInjectionHandle;

// Convert MMCSSClass to string for logging
std::string mmcssClassToString(MMCSSClass mmcssClass) {
    switch (mmcssClass) {
        case MMCSSClass::Games: return "Games";
        case MMCSSClass::Display: return "Display";
        case MMCSSClass::Audio: return "Audio";
        case MMCSSClass::Playback: return "Playback";
        case MMCSSClass::Capture: return "Capture";
        case MMCSSClass::None: return "None";
        default: return "Unknown";
    }
}

// Parse MMCSS class from string
MMCSSClass stringToMMCSSClass(const std::string& str) {
    if (str == "Games" || str == "games") return MMCSSClass::Games;
    if (str == "Display" || str == "display") return MMCSSClass::Display;
    if (str == "Audio" || str == "audio") return MMCSSClass::Audio;
    if (str == "Playback" || str == "playback") return MMCSSClass::Playback;
    if (str == "Capture" || str == "capture") return MMCSSClass::Capture;
    if (str == "None" || str == "none") return MMCSSClass::None;
    return MMCSSClass::Games; // Default fallback
}

// Initialize global priority configuration from environment/config
void initializeGlobalConfig() {
    // Read from environment variables
    const char* mmcssEnv = std::getenv("INPUT_THREAD_MMCSS_CLASS");
    if (mmcssEnv) {
        globalPriorityConfig.mmcssClass = stringToMMCSSClass(mmcssEnv);
        std::cout << "[ThreadPriorityManager] Set MMCSS class to '" << mmcssClassToString(globalPriorityConfig.mmcssClass)
                  << "' from environment" << std::endl;
    }

    const char* priorityEnv = std::getenv("INPUT_THREAD_PRIORITY");
    if (priorityEnv) {
        try {
            int priority = std::stoi(priorityEnv);
            if (priority >= THREAD_PRIORITY_IDLE && priority <= THREAD_PRIORITY_TIME_CRITICAL) {
                globalPriorityConfig.threadPriority = priority;
                std::cout << "[ThreadPriorityManager] Set thread priority to " << priority << " from environment" << std::endl;
            }
        } catch (const std::exception&) {
            std::cerr << "[ThreadPriorityManager] Invalid INPUT_THREAD_PRIORITY value: " << priorityEnv << std::endl;
        }
    }

    const char* enableMmcssEnv = std::getenv("INPUT_THREAD_ENABLE_MMCSS");
    if (enableMmcssEnv) {
        globalPriorityConfig.enableMMCSS = (std::string(enableMmcssEnv) == "1" ||
                                           std::string(enableMmcssEnv) == "true" ||
                                           std::string(enableMmcssEnv) == "TRUE");
        std::cout << "[ThreadPriorityManager] MMCSS " << (globalPriorityConfig.enableMMCSS ? "enabled" : "disabled")
                  << " from environment" << std::endl;
    }

    const char* enableTimeCriticalEnv = std::getenv("INPUT_THREAD_ENABLE_TIME_CRITICAL");
    if (enableTimeCriticalEnv) {
        globalPriorityConfig.enableTimeCritical = (std::string(enableTimeCriticalEnv) == "1" ||
                                                  std::string(enableTimeCriticalEnv) == "true" ||
                                                  std::string(enableTimeCriticalEnv) == "TRUE");
        std::cout << "[ThreadPriorityManager] TIME_CRITICAL priority "
                  << (globalPriorityConfig.enableTimeCritical ? "enabled" : "disabled")
                  << " from environment" << std::endl;
    }

    const char* taskNameEnv = std::getenv("INPUT_THREAD_TASK_NAME");
    if (taskNameEnv) {
        globalPriorityConfig.taskName = taskNameEnv;
        std::cout << "[ThreadPriorityManager] Set MMCSS task name to '" << globalPriorityConfig.taskName
                  << "' from environment" << std::endl;
    }

    std::cout << "[ThreadPriorityManager] Initialized with config:" << std::endl;
    std::cout << "  MMCSS Class: " << mmcssClassToString(globalPriorityConfig.mmcssClass) << std::endl;
    std::cout << "  Thread Priority: " << globalPriorityConfig.threadPriority << std::endl;
    std::cout << "  MMCSS Enabled: " << (globalPriorityConfig.enableMMCSS ? "Yes" : "No") << std::endl;
    std::cout << "  TIME_CRITICAL Enabled: " << (globalPriorityConfig.enableTimeCritical ? "Yes" : "No") << std::endl;
    std::cout << "  Task Name: " << globalPriorityConfig.taskName << std::endl;
}

// Configuration API
void setMMCSSClass(MMCSSClass mmcssClass) {
    globalPriorityConfig.mmcssClass = mmcssClass;
}

void setThreadPriority(int priority) {
    if (priority >= THREAD_PRIORITY_IDLE && priority <= THREAD_PRIORITY_TIME_CRITICAL) {
        globalPriorityConfig.threadPriority = priority;
    }
}

void enableMMCSS(bool enable) {
    globalPriorityConfig.enableMMCSS = enable;
}

void enableTimeCritical(bool enable) {
    globalPriorityConfig.enableTimeCritical = enable;
}

void setTaskName(const std::string& taskName) {
    globalPriorityConfig.taskName = taskName;
}

} // namespace ThreadPriorityManager
