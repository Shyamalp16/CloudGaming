#include "WebRTCWrapper.h"
#include <stdexcept>
#include <cstring>
#include <string>
#include <iostream>

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

} // namespace WebRTCWrapper
