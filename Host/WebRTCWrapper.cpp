#include "WebRTCWrapper.h"
#include <stdexcept>
#include <cstring>
#include <string>
#include <iostream>
#include <cassert>

// Temporary simple logging to fix compilation
#define LOG_ERROR(msg) std::cout << "[ERROR] " << msg << std::endl
#define LOG_WARN(msg) std::cout << "[WARN] " << msg << std::endl
#define LOG_INFO(msg) std::cout << "[INFO] " << msg << std::endl
#define LOG_DEBUG(msg) std::cout << "[DEBUG] " << msg << std::endl
#define LOG_TRACE(msg) std::cout << "[TRACE] " << msg << std::endl

namespace WebRTCWrapper {

namespace {
// RAII helper for C string ownership
class CStrOwner {
private:
    char* ptr_;
public:
    explicit CStrOwner(char* p) noexcept : ptr_(p) {}
    ~CStrOwner() noexcept {
        if (ptr_) {
            freeCString(ptr_);
        }
    }

    // Disable copy operations
    CStrOwner(const CStrOwner&) = delete;
    CStrOwner& operator=(const CStrOwner&) = delete;

    // Allow move operations
    CStrOwner(CStrOwner&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }
    CStrOwner& operator=(CStrOwner&& other) noexcept {
        if (this != &other) {
            if (ptr_) {
                freeCString(ptr_);
            }
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    const char* get() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
};

} // anonymous namespace

std::string getDataChannelMessageString() {
    char* cMsg = getDataChannelMessage();
    if (!cMsg) {
        return std::string{};
    }

    CStrOwner owner(cMsg);
    std::string result;

    try {
        result = std::string(owner.get());
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to copy data channel message to std::string: " + std::string(e.what()));
        throw std::runtime_error("Memory allocation failed during message copy");
    }

    LOG_TRACE("Retrieved data channel message: " + std::to_string(result.size()) + " characters");
    return result;
}

std::string getMouseChannelMessageString() {
    char* cMsg = getMouseChannelMessage();
    if (!cMsg) {
        return std::string{};
    }

    CStrOwner owner(cMsg);
    std::string result;

    try {
        result = std::string(owner.get());
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to copy mouse channel message to std::string: " + std::string(e.what()));
        throw std::runtime_error("Memory allocation failed during message copy");
    }

    LOG_TRACE("Retrieved mouse channel message: " + std::to_string(result.size()) + " characters");
    return result;
}

WebRTCWrapper::CStringWrapper WebRTCWrapper::getMouseChannelMessageSafe() {
    return WebRTCWrapper::CStringWrapper(getMouseChannelMessage());
}

// Test function to verify RAII wrapper behavior
void testCStringWrapper() {
    std::cout << "[WebRTCWrapper] Testing CStringWrapper RAII behavior..." << std::endl;

    // Test 1: Empty wrapper
    {
        WebRTCWrapper::CStringWrapper emptyWrapper;
        assert(!emptyWrapper.valid());
        assert(emptyWrapper.toString().empty());
        std::cout << "[PASS] Empty wrapper test" << std::endl;
    }

    // Test 2: Wrapper with content (would require actual message)
    // This test would be more comprehensive with a test harness
    std::cout << "[WebRTCWrapper] CStringWrapper tests completed" << std::endl;
}

// Go-exported C functions (fallback stubs provided if not linked)
extern "C" {
#ifndef GO_ENQUEUE_LINKED
    // Fallback stubs to satisfy the linker when the Go DLL is not linked.
    // Define GO_ENQUEUE_LINKED and link the Go import library to use real exports.
    void enqueueDataChannelMessage(char* msg) {
        (void)msg;
        LOG_WARN("Go enqueueDataChannelMessage not linked; dropping message");
    }
    void enqueueMouseChannelMessage(char* msg) {
        (void)msg;
        LOG_WARN("Go enqueueMouseChannelMessage not linked; dropping message");
    }
#else
    void enqueueDataChannelMessage(char* msg);
    void enqueueMouseChannelMessage(char* msg);
#endif
}

void enqueueDataChannelMessage(const std::string& message) {
    if (message.empty()) {
        LOG_WARN("Attempted to enqueue empty data channel message");
        return;
    }

    // Call the Go-side enqueue function
    char* cMsg = const_cast<char*>(message.c_str());
    enqueueDataChannelMessage(cMsg);
    LOG_TRACE("Enqueued data channel message: " + std::to_string(message.size()) + " characters");
}

void enqueueMouseChannelMessage(const std::string& message) {
    if (message.empty()) {
        LOG_WARN("Attempted to enqueue empty mouse channel message");
        return;
    }

    // Call the Go-side enqueue function
    char* cMsg = const_cast<char*>(message.c_str());
    enqueueMouseChannelMessage(cMsg);
    LOG_TRACE("Enqueued mouse channel message: " + std::to_string(message.size()) + " characters");
}

// Static storage for the enhanced stats callback
static WebRTCStatsCallback g_webrtcStatsCallback = nullptr;

// Enhanced WebRTC stats callback implementation
static void webrtcStatsCallbackImpl(double packetLoss, double rtt, double jitter,
                                   uint32_t nackCount, uint32_t pliCount, uint32_t twccCount,
                                   uint32_t pacerQueueLength, uint32_t sendBitrateKbps) {
    if (g_webrtcStatsCallback) {
        try {
            g_webrtcStatsCallback(packetLoss, rtt, jitter, nackCount, pliCount, twccCount,
                                pacerQueueLength, sendBitrateKbps);
        } catch (const std::exception& e) {
            LOG_ERROR("Exception in WebRTC stats callback: " + std::string(e.what()));
        }
    }
}

void setWebRTCStatsCallback(WebRTCStatsCallback callback) {
    g_webrtcStatsCallback = callback;

    if (callback) {
        LOG_INFO("Enhanced WebRTC stats callback registered");

        // Register the callback with the Go side
        // Cast to void* to match the C function signature
        SetWebRTCStatsCallback(reinterpret_cast<void*>(&webrtcStatsCallbackImpl));
    } else {
        LOG_INFO("Enhanced WebRTC stats callback disabled");
        SetWebRTCStatsCallback(nullptr);
    }
}

} // namespace WebRTCWrapper
