#include "WindowsSecurity.h"

#include <Windows.h>
#include <sddl.h>

#include <vector>

#pragma comment(lib, "Advapi32.lib")

namespace WindowsSecurity {
bool ProtectForCurrentUserAndSystem(const std::filesystem::path& path, std::string& error) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        error = "OpenProcessToken failed: " + std::to_string(GetLastError());
        return false;
    }
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> storage(bytes);
    if (bytes == 0 || !GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes)) {
        error = "GetTokenInformation failed: " + std::to_string(GetLastError());
        CloseHandle(token);
        return false;
    }
    CloseHandle(token);

    LPWSTR sidString = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sidString)) {
        error = "ConvertSidToStringSid failed: " + std::to_string(GetLastError());
        return false;
    }
    const std::wstring sddl = L"D:P(A;;FA;;;SY)(A;;FA;;;" + std::wstring(sidString) + L")";
    LocalFree(sidString);

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        error = "Security descriptor conversion failed: " + std::to_string(GetLastError());
        return false;
    }
    const BOOL applied = SetFileSecurityW(path.c_str(),
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
    const DWORD lastError = applied ? ERROR_SUCCESS : GetLastError();
    LocalFree(descriptor);
    if (!applied) {
        error = "SetFileSecurity failed: " + std::to_string(lastError);
        return false;
    }
    return true;
}
}
