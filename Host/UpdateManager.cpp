#include "pch.h"
#include "UpdateManager.h"

#include <Windows.h>
#include <Winhttp.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <Softpub.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include "AppPaths.h"
#include "Version.h"
#include "WindowsSecurity.h"

#pragma comment(lib, "Winhttp.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Wintrust.lib")
#pragma comment(lib, "Bcrypt.lib")

namespace UpdateManager {
namespace {
using Bytes = std::vector<unsigned char>;

struct InternetHandleDeleter { void operator()(HINTERNET value) const { if (value) WinHttpCloseHandle(value); } };
using InternetHandle = std::unique_ptr<void, InternetHandleDeleter>;

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), count);
    return result;
}

bool DownloadHttps(const std::string& url, size_t maximumBytes, Bytes& body, std::string& error) {
    const auto wideUrl = Utf8ToWide(url);
    if (wideUrl.empty()) { error = "Invalid update URL"; return false; }
    URL_COMPONENTSW parts{sizeof(parts)};
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        error = "The update feed must use HTTPS";
        return false;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    InternetHandle session(WinHttpOpen(L"CloudGamingHost/" CLOUD_GAMING_VERSION_W,
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) { error = "Could not initialize the update network client"; return false; }
    WinHttpSetTimeouts(session.get(), 5000, 5000, 10000, 15000);
    InternetHandle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection) { error = "Could not connect to the update server"; return false; }
    InternetHandle request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request || !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.get(), nullptr)) {
        error = "The update download failed";
        return false;
    }
    DWORD status = 0, statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             nullptr, &status, &statusSize, nullptr) || status != 200) {
        error = "The update server returned HTTP " + std::to_string(status);
        return false;
    }
    body.clear();
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) { error = "Could not read update data"; return false; }
        if (available == 0) break;
        if (body.size() + available > maximumBytes) { error = "Update response exceeds its size limit"; return false; }
        const size_t offset = body.size();
        body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), body.data() + offset, available, &read)) {
            error = "Could not read update data";
            return false;
        }
        body.resize(offset + read);
    }
    return true;
}

std::string NormalizeHex(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
        return c == ':' || c == ' ' || c == '-';
    }), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string ToHex(const unsigned char* bytes, size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(size * 2, '0');
    for (size_t i = 0; i < size; ++i) {
        result[i * 2] = digits[bytes[i] >> 4];
        result[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    return result;
}

bool VerifyManifest(const Bytes& manifest, const Bytes& signature,
                    const std::string& pinnedCertificate, std::string& error) {
    const auto pin = NormalizeHex(pinnedCertificate);
    if (pin.size() != 64) { error = "A SHA-256 publisher certificate pin is required"; return false; }
    CRYPT_VERIFY_MESSAGE_PARA parameters{sizeof(parameters)};
    parameters.dwMsgAndCertEncodingType = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;
    parameters.hCryptProv = 0;
    parameters.pfnGetSignerCertificate = nullptr;
    const BYTE* content[] = {manifest.data()};
    DWORD contentSizes[] = {static_cast<DWORD>(manifest.size())};
    PCCERT_CONTEXT signer = nullptr;
    if (!CryptVerifyDetachedMessageSignature(&parameters, 0, signature.data(),
            static_cast<DWORD>(signature.size()), 1, content, contentSizes, &signer)) {
        error = "The update manifest signature is invalid";
        return false;
    }
    DWORD hashSize = 0;
    CertGetCertificateContextProperty(signer, CERT_SHA256_HASH_PROP_ID, nullptr, &hashSize);
    Bytes hash(hashSize);
    const bool read = hashSize > 0 && CertGetCertificateContextProperty(
        signer, CERT_SHA256_HASH_PROP_ID, hash.data(), &hashSize);
    CertFreeCertificateContext(signer);
    if (!read || ToHex(hash.data(), hashSize) != pin) {
        error = "The update manifest signer does not match the configured publisher";
        return false;
    }
    return true;
}

std::array<int, 3> ParseVersion(const std::string& version) {
    std::array<int, 3> result{-1, -1, -1};
    size_t start = 0;
    for (size_t i = 0; i < result.size(); ++i) {
        const auto end = version.find('.', start);
        const auto token = version.substr(start, end == std::string::npos ? end : end - start);
        if (token.empty() || token.size() > 6 || !std::all_of(token.begin(), token.end(), ::isdigit)) return {-1, -1, -1};
        result[i] = std::stoi(token);
        if (i < 2 && end == std::string::npos) return {-1, -1, -1};
        if (i == 2 && end != std::string::npos) return {-1, -1, -1};
        start = end + 1;
    }
    return result;
}

std::string Sha256(const Bytes& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, resultSize = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &resultSize, 0);
    std::vector<unsigned char> object(objectSize), digest(32);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) < 0 ||
        BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) < 0 ||
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) digest.clear();
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return digest.empty() ? std::string{} : ToHex(digest.data(), digest.size());
}

bool VerifyAuthenticode(const std::filesystem::path& path) {
    WINTRUST_FILE_INFO file{sizeof(file)};
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA data{sizeof(data)};
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(nullptr, &policy, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &data);
    return status == ERROR_SUCCESS;
}
}

Result Check(const nlohmann::json& config) {
    Result result;
    const auto update = config.value("update", nlohmann::json::object());
    const auto feed = update.value("feedUrl", std::string{});
    if (feed.empty()) {
        result.status = Status::Disabled;
        result.message = "Update checking is not configured";
        return result;
    }
    Bytes manifest, signature;
    std::string error;
    if (!DownloadHttps(feed, 256 * 1024, manifest, error) ||
        !DownloadHttps(feed + ".p7s", 256 * 1024, signature, error) ||
        !VerifyManifest(manifest, signature, update.value("publisherCertificateSha256", std::string{}), error)) {
        result.message = error;
        return result;
    }
    try {
        const auto document = nlohmann::json::parse(manifest.begin(), manifest.end());
        result.version = document.at("version").get<std::string>();
        result.downloadUrl = document.at("downloadUrl").get<std::string>();
        result.sha256 = NormalizeHex(document.at("sha256").get<std::string>());
        if (document.value("schemaVersion", 0) != 1 || result.downloadUrl.rfind("https://", 0) != 0 ||
            result.sha256.size() != 64 || ParseVersion(result.version)[0] < 0) throw std::runtime_error("invalid fields");
    } catch (...) {
        result.message = "The signed update manifest is malformed";
        return result;
    }
    if (ParseVersion(result.version) <= ParseVersion(CLOUD_GAMING_VERSION)) {
        result.status = Status::UpToDate;
        result.message = "Cloud Gaming Host is up to date";
    } else {
        result.status = Status::Available;
        result.message = "Cloud Gaming Host " + result.version + " is available";
    }
    return result;
}

bool DownloadVerifyAndLaunch(const Result& update, std::string& error) {
    if (update.status != Status::Available) { error = "No verified update is available"; return false; }
    Bytes installer;
    if (!DownloadHttps(update.downloadUrl, 750ull * 1024ull * 1024ull, installer, error)) return false;
    if (Sha256(installer) != NormalizeHex(update.sha256)) { error = "The downloaded update hash does not match"; return false; }
    const auto directory = AppPaths::UserDataDirectory() / L"updates";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) { error = "Could not create the update directory"; return false; }
    std::string aclError;
    if (!WindowsSecurity::ProtectForCurrentUserAndSystem(directory, aclError)) {
        error = "Could not protect the update directory: " + aclError;
        return false;
    }
    const auto path = directory / (L"CloudGamingHost-" + Utf8ToWide(update.version) + L"-x64.msi");
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(installer.data()), static_cast<std::streamsize>(installer.size()));
        if (!output) { error = "Could not save the update installer"; return false; }
    }
    if (!VerifyAuthenticode(path)) { error = "Windows could not verify the update publisher signature"; return false; }
    const std::wstring parameters = L"/i \"" + path.wstring() + L"\"";
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"runas", L"msiexec.exe",
        parameters.c_str(), directory.c_str(), SW_SHOWNORMAL)) <= 32) {
        error = "Could not start Windows Installer";
        return false;
    }
    return true;
}
}
