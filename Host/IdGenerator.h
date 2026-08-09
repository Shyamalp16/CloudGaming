#pragma once

#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <cctype>
#include "SecretStore.h"
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

inline bool isValidHostId(const std::string& value) {
	if (value.size() != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' || value[23] != '-')
		return false;
	for (size_t index = 0; index < value.size(); ++index) {
		if (index == 8 || index == 13 || index == 18 || index == 23) continue;
		if (!std::isxdigit(static_cast<unsigned char>(value[index]))) return false;
	}
	return true;
}

inline std::string loadOrCreateHostId() {
	std::string error;
	const auto stored = SecretStore::Get("deviceId", error);
	if (stored && isValidHostId(*stored)) return *stored;
	if (!error.empty()) throw std::runtime_error("Could not read the host device identity: " + error);
	if (stored) throw std::runtime_error("The stored host device identity is invalid; re-enrollment is required");
	const auto generated = generateHostId();
	if (!SecretStore::Set("deviceId", generated, error))
		throw std::runtime_error("Could not persist the host device identity: " + error);
	return generated;
}
