#pragma once

#include <string>
#include <random>
#include <sstream>

// Generates a simple random alphanumeric string of a given length.
// This is sufficient for creating a unique enough roomId for this use case.
std::string generateRoomId(int length = 8) {
    const std::string chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<> distribution(0, chars.length() - 1);
    
    std::stringstream ss;
    for (int i = 0; i < length; ++i) {
        ss << chars[distribution(generator)];
    }
    return ss.str();
}
