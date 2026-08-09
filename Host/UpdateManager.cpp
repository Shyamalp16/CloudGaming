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
#include <regex>
#include <set>
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
    parts.dwUserNameLength = static_cast<DWORD>(-1);
    parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS ||
        parts.dwUserNameLength != 0 || parts.dwPasswordLength != 0) {
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
    DWORD disabledFeatures = WINHTTP_DISABLE_REDIRECTS;
    if (!request || !WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE,
        &disabledFeatures, sizeof(disabledFeatures)) ||
        !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
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

bool CertificateIsTrusted(PCCERT_CONTEXT certificate) {
    if (!certificate) return false;
    CERT_CHAIN_PARA parameters{sizeof(parameters)};
    PCCERT_CHAIN_CONTEXT chain = nullptr;
    if (!CertGetCertificateChain(nullptr, certificate, nullptr, certificate->hCertStore, &parameters,
        CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT, nullptr, &chain)) return false;
    CERT_CHAIN_POLICY_PARA policyParameters{sizeof(policyParameters)};
    CERT_CHAIN_POLICY_STATUS policyStatus{sizeof(policyStatus)};
    const bool valid = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_BASE, chain,
        &policyParameters, &policyStatus) && policyStatus.dwError == S_OK;
    CertFreeCertificateChain(chain);
    return valid;
}

bool SameHttpsOrigin(const std::string& left, const std::string& right) {
	const auto a = Utf8ToWide(left), b = Utf8ToWide(right);
	URL_COMPONENTSW pa{sizeof(pa)}, pb{sizeof(pb)};
	pa.dwSchemeLength = pa.dwHostNameLength = static_cast<DWORD>(-1);
	pb.dwSchemeLength = pb.dwHostNameLength = static_cast<DWORD>(-1);
	if (!WinHttpCrackUrl(a.c_str(), 0, 0, &pa) || !WinHttpCrackUrl(b.c_str(), 0, 0, &pb) ||
		pa.nScheme != INTERNET_SCHEME_HTTPS || pb.nScheme != INTERNET_SCHEME_HTTPS) return false;
	const std::wstring ah(pa.lpszHostName, pa.dwHostNameLength), bh(pb.lpszHostName, pb.dwHostNameLength);
	return _wcsicmp(ah.c_str(), bh.c_str()) == 0 && pa.nPort == pb.nPort;
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
					const std::set<std::string>& publisherPins, std::string& error) {
	if (publisherPins.empty()) { error = "A compiled SHA-256 publisher certificate pin is required"; return false; }
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
    const bool trusted = CertificateIsTrusted(signer);
    CertFreeCertificateContext(signer);
	if (!read || !trusted || !publisherPins.contains(ToHex(hash.data(), hashSize))) {
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

bool CurrentWindowsAtLeast(const std::array<int, 3>& minimum) {
	struct VersionInfo {
		ULONG size;
		ULONG major;
		ULONG minor;
		ULONG build;
		ULONG platform;
		WCHAR servicePack[128];
	};
	using RtlGetVersionFn = LONG(WINAPI*)(VersionInfo*);
	const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	if (!ntdll) return false;
	const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
	if (!rtlGetVersion) return false;
	VersionInfo current{};
	current.size = sizeof(current);
	if (rtlGetVersion(&current) < 0) return false;
	const std::array<int, 3> actual{static_cast<int>(current.major), static_cast<int>(current.minor),
		static_cast<int>(current.build)};
	return actual >= minimum;
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

std::string AuthenticodeSignerSha256(const std::filesystem::path& path) {
	HCERTSTORE store = nullptr;
	HCRYPTMSG message = nullptr;
	DWORD encoding = 0, contentType = 0, formatType = 0;
	if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(),
		CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED, CERT_QUERY_FORMAT_FLAG_BINARY, 0,
		&encoding, &contentType, &formatType, &store, &message, nullptr)) return {};
	DWORD signerSize = 0;
	if (!CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &signerSize)) {
		CryptMsgClose(message); CertCloseStore(store, 0); return {};
	}
	Bytes signerBytes(signerSize);
	if (!CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, signerBytes.data(), &signerSize)) {
		CryptMsgClose(message); CertCloseStore(store, 0); return {};
	}
	auto* signer = reinterpret_cast<PCMSG_SIGNER_INFO>(signerBytes.data());
	CERT_INFO search{};
	search.Issuer = signer->Issuer;
	search.SerialNumber = signer->SerialNumber;
	PCCERT_CONTEXT certificate = CertFindCertificateInStore(store, encoding, 0,
		CERT_FIND_SUBJECT_CERT, &search, nullptr);
	std::string result;
	if (certificate) {
		DWORD hashSize = 0;
		if (CertGetCertificateContextProperty(certificate, CERT_SHA256_HASH_PROP_ID, nullptr, &hashSize)) {
			Bytes hash(hashSize);
			if (CertGetCertificateContextProperty(certificate, CERT_SHA256_HASH_PROP_ID, hash.data(), &hashSize))
				result = ToHex(hash.data(), hashSize);
		}
		CertFreeCertificateContext(certificate);
	}
	CryptMsgClose(message);
	CertCloseStore(store, 0);
	return result;
}

bool VerifyAuthenticode(const std::filesystem::path& path, const std::set<std::string>& publisherPins) {
    WINTRUST_FILE_INFO file{sizeof(file)};
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA data{sizeof(data)};
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(nullptr, &policy, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &data);
	if (status != ERROR_SUCCESS) return false;
	const auto signer = AuthenticodeSignerSha256(path);
	return !signer.empty() && publisherPins.contains(signer);
}
}

Result Check(const nlohmann::json& config) {
    Result result;
	const auto update = config.value("update", nlohmann::json::object());
	const std::string feed = CLOUD_GAMING_UPDATE_FEED_URL;
	std::set<std::string> publisherPins;
	for (const char* candidate : {CLOUD_GAMING_UPDATE_CERT_SHA256, CLOUD_GAMING_UPDATE_CERT_SHA256_NEXT}) {
		const auto normalized = NormalizeHex(candidate);
		if (normalized.size() == 64) publisherPins.insert(normalized);
	}
    if (feed.empty()) {
        result.status = Status::Disabled;
        result.message = "Update checking is not configured";
        return result;
    }
    Bytes manifest, signature;
    std::string error;
    if (!DownloadHttps(feed, 256 * 1024, manifest, error) ||
        !DownloadHttps(feed + ".p7s", 256 * 1024, signature, error) ||
		!VerifyManifest(manifest, signature, publisherPins, error)) {
        result.message = error;
        return result;
    }
    try {
        const auto document = nlohmann::json::parse(manifest.begin(), manifest.end());
		static const std::set<std::string> expectedFields = {"schemaVersion", "product", "version", "channel",
			"publishedUtc", "downloadUrl", "size", "sha256", "minimumOs"};
		if (!document.is_object() || document.size() != expectedFields.size()) throw std::runtime_error("invalid shape");
		for (auto iterator = document.begin(); iterator != document.end(); ++iterator)
			if (!expectedFields.contains(iterator.key())) throw std::runtime_error("unknown field");
		result.version = document.at("version").get<std::string>();
		result.downloadUrl = document.at("downloadUrl").get<std::string>();
		result.sha256 = NormalizeHex(document.at("sha256").get<std::string>());
		result.size = document.at("size").get<std::uint64_t>();
		const auto channel = update.value("channel", std::string{"stable"});
		const auto published = document.at("publishedUtc").get<std::string>();
		const auto minimumOs = document.at("minimumOs").get<std::string>();
		const auto minimumVersion = ParseVersion(minimumOs);
		if (document.value("schemaVersion", 0) != 1 || document.value("product", std::string{}) != "Cloud Gaming Host" ||
			document.value("channel", std::string{}) != channel || !SameHttpsOrigin(feed, result.downloadUrl) ||
			result.sha256.size() != 64 || !std::all_of(result.sha256.begin(), result.sha256.end(), ::isxdigit) ||
			ParseVersion(result.version)[0] < 0 || result.size == 0 || result.size > 750ull * 1024ull * 1024ull ||
			!std::regex_match(published, std::regex(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?Z$)")) ||
			minimumVersion[0] < 0) throw std::runtime_error("invalid fields");
		if (!CurrentWindowsAtLeast(minimumVersion)) {
			result.status = Status::Error;
			result.message = "This update requires a newer Windows version";
			return result;
		}
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
	if (update.size == 0 || update.size > 750ull * 1024ull * 1024ull ||
		!DownloadHttps(update.downloadUrl, static_cast<size_t>(update.size), installer, error)) return false;
	if (installer.size() != update.size) { error = "The downloaded update size does not match"; return false; }
    if (Sha256(installer) != NormalizeHex(update.sha256)) { error = "The downloaded update hash does not match"; return false; }
	std::array<unsigned char, 16> random{};
	if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
		error = "Could not create a protected update path"; return false;
	}
	const auto directory = AppPaths::UserDataDirectory() / L"updates" /
		Utf8ToWide(ToHex(random.data(), random.size()));
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
	std::set<std::string> publisherPins;
	for (const char* candidate : {CLOUD_GAMING_UPDATE_CERT_SHA256, CLOUD_GAMING_UPDATE_CERT_SHA256_NEXT}) {
		const auto normalized = NormalizeHex(candidate);
		if (normalized.size() == 64) publisherPins.insert(normalized);
	}
	if (!VerifyAuthenticode(path, publisherPins)) {
		error = "The installer is not signed by the pinned Cloud Gaming publisher"; return false;
	}
	const std::wstring parameters = L"/i \"" + path.wstring() + L"\"";
	wchar_t systemDirectory[MAX_PATH]{};
	if (!GetSystemDirectoryW(systemDirectory, MAX_PATH)) { error = "Could not locate Windows Installer"; return false; }
	const std::filesystem::path msiexec = std::filesystem::path(systemDirectory) / L"msiexec.exe";
	if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", msiexec.c_str(),
        parameters.c_str(), directory.c_str(), SW_SHOWNORMAL)) <= 32) {
        error = "Could not start Windows Installer";
        return false;
    }
    return true;
}
}
