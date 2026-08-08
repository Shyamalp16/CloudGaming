#pragma once

#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#pragma comment(lib, "bcrypt.lib")

inline std::array<unsigned char, 16> generateSecureIdBytes() {
    std::array<unsigned char, 16> bytes{};
    const NTSTATUS status = BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                                            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) throw std::runtime_error("BCryptGenRandom failed");
    return bytes;
}

inline std::string generateRoomId() {
    const auto bytes = generateSecureIdBytes();
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned char value : bytes) out << std::setw(2) << static_cast<unsigned>(value);
    return out.str();
}

inline std::string generateHostId() {
    auto bytes = generateSecureIdBytes();
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out << '-';
        out << std::setw(2) << static_cast<unsigned>(bytes[i]);
    }
    return out.str();
}
