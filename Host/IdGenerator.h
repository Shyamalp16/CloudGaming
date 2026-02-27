#pragma once

#include <string>
#include <random>
#include <sstream>
#include <iomanip>

// Generates a simple random alphanumeric string of a given length.
// This is sufficient for creating a unique enough roomId for this use case.
inline std::string generateRoomId(int length = 8) {
    const std::string chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<> distribution(0, static_cast<int>(chars.length() - 1));
    
    std::stringstream ss;
    for (int i = 0; i < length; ++i) {
        ss << chars[distribution(generator)];
    }
    return ss.str();
}

// Generates a UUID-like host identifier (format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)
// This provides a unique identifier for each host instance across sessions.
inline std::string generateHostId() {
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);
    
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    
    // UUID format: 8-4-4-4-12
    ss << std::setw(8) << dist(generator) << "-";
    ss << std::setw(4) << (dist(generator) & 0xFFFF) << "-";
    ss << std::setw(4) << ((dist(generator) & 0x0FFF) | 0x4000) << "-";  // Version 4
    ss << std::setw(4) << ((dist(generator) & 0x3FFF) | 0x8000) << "-";  // Variant
    ss << std::setw(8) << dist(generator);
    ss << std::setw(4) << (dist(generator) & 0xFFFF);
    
    return ss.str();
}
