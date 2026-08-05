#include "WebRTCWrapper.h"
#include "pion_webrtc.h"
#include <stdexcept>
#include <cstring>
#include <string>
#include <iostream>

#define LOG_ERROR(msg) std::cerr << "[ERROR] " << msg << std::endl
#define LOG_INFO(msg) std::cout << "[INFO] " << msg << std::endl

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
    char* cMsg = ::getDataChannelMessage();
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

    return result;
}

std::string getMouseChannelMessageString() {
    char* cMsg = ::getMouseChannelMessage();
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

    return result;
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

        ::SetWebRTCStatsCallback(&webrtcStatsCallbackImpl);
    } else {
        LOG_INFO("Enhanced WebRTC stats callback disabled");
        ::SetWebRTCStatsCallback(nullptr);
    }
}

} // namespace WebRTCWrapper
