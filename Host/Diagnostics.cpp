#include "Diagnostics.h"

#include <Windows.h>
#include <DbgHelp.h>
#include <sddl.h>

#include <chrono>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <regex>
#include <sstream>
#include <functional>
#include <vector>

#pragma comment(lib, "Dbghelp.lib")
#pragma comment(lib, "Advapi32.lib")

namespace Diagnostics {
namespace {
std::mutex g_mutex;
Status g_status;
constexpr std::uintmax_t kMaxLogBytes = 5 * 1024 * 1024;
constexpr int kLogFiles = 5;

template <typename CharT>
class ConsoleTeeBuffer final : public std::basic_streambuf<CharT> {
public:
    using Base = std::basic_streambuf<CharT>;
    using typename Base::int_type;
    using typename Base::traits_type;
    using Callback = std::function<void(std::basic_string<CharT>)>;

    ConsoleTeeBuffer(Base* original, Callback callback)
        : original_(original), callback_(std::move(callback)) {}

protected:
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
        const CharT character = traits_type::to_char_type(value);
        if (traits_type::eq_int_type(original_->sputc(character), traits_type::eof())) return traits_type::eof();
        Process(&character, 1);
        return value;
    }

    std::streamsize xsputn(const CharT* data, std::streamsize count) override {
        const auto written = original_->sputn(data, count);
        if (written > 0) Process(data, static_cast<size_t>(written));
        return written;
    }

    int sync() override { return original_->pubsync(); }

private:
    void Process(const CharT* data, size_t count) {
        std::vector<std::basic_string<CharT>> completed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (size_t index = 0; index < count; ++index) {
                if (data[index] == static_cast<CharT>('\n')) {
                    if (!line_.empty()) completed.push_back(std::move(line_));
                    line_.clear();
                } else if (data[index] != static_cast<CharT>('\r')) {
                    line_.push_back(data[index]);
                }
            }
        }
        for (auto& line : completed) callback_(std::move(line));
    }

    Base* original_;
    Callback callback_;
    std::mutex mutex_;
    std::basic_string<CharT> line_;
};

std::unique_ptr<ConsoleTeeBuffer<char>> g_coutTee;
std::unique_ptr<ConsoleTeeBuffer<char>> g_cerrTee;
std::unique_ptr<ConsoleTeeBuffer<wchar_t>> g_wcoutTee;
std::unique_ptr<ConsoleTeeBuffer<wchar_t>> g_wcerrTee;
std::streambuf* g_originalCout = nullptr;
std::streambuf* g_originalCerr = nullptr;
std::wstreambuf* g_originalWcout = nullptr;
std::wstreambuf* g_originalWcerr = nullptr;

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "[wide console conversion failed]";
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

void InstallConsoleCaptureLocked() {
    if (g_coutTee) return;
    g_originalCout = std::cout.rdbuf();
    g_originalCerr = std::cerr.rdbuf();
    g_originalWcout = std::wcout.rdbuf();
    g_originalWcerr = std::wcerr.rdbuf();
    g_coutTee = std::make_unique<ConsoleTeeBuffer<char>>(g_originalCout,
        [](std::string line) { Log("INFO", "CONSOLE", line); });
    g_cerrTee = std::make_unique<ConsoleTeeBuffer<char>>(g_originalCerr,
        [](std::string line) { Log("ERROR", "CONSOLE", line); });
    g_wcoutTee = std::make_unique<ConsoleTeeBuffer<wchar_t>>(g_originalWcout,
        [](std::wstring line) { Log("INFO", "CONSOLE", Utf8(line)); });
    g_wcerrTee = std::make_unique<ConsoleTeeBuffer<wchar_t>>(g_originalWcerr,
        [](std::wstring line) { Log("ERROR", "CONSOLE", Utf8(line)); });
    std::cout.rdbuf(g_coutTee.get());
    std::cerr.rdbuf(g_cerrTee.get());
    std::wcout.rdbuf(g_wcoutTee.get());
    std::wcerr.rdbuf(g_wcerrTee.get());
}

bool HardenPath(const std::filesystem::path& path) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> storage(bytes);
    if (!GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes)) {
        CloseHandle(token); return false;
    }
    CloseHandle(token);
    LPWSTR sidString = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sidString)) return false;
    const std::wstring sddl = L"D:P(A;;FA;;;SY)(A;;FA;;;" + std::wstring(sidString) + L")";
    LocalFree(sidString);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                               &descriptor, nullptr)) return false;
    const BOOL applied = SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION |
        PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
    LocalFree(descriptor);
    return applied == TRUE;
}

std::filesystem::path AppDataRoot() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    std::filesystem::path base = length > 0 && length < MAX_PATH
        ? std::filesystem::path(buffer) : std::filesystem::temp_directory_path();
    return base / L"CloudGamingHost";
}

std::string Timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &time);
    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%dT%H:%M:%S");
    return output.str();
}

std::string FileTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &time);
    std::ostringstream output;
    output << std::put_time(&local, "%Y%m%d-%H%M%S");
    return output.str();
}

void RotateLocked() {
    std::error_code ec;
    if (!std::filesystem::exists(g_status.activeLog, ec) ||
        std::filesystem::file_size(g_status.activeLog, ec) < kMaxLogBytes) return;
    for (int index = kLogFiles - 1; index >= 1; --index) {
        auto source = g_status.logDirectory / ("host." + std::to_string(index) + ".jsonl");
        auto target = g_status.logDirectory / ("host." + std::to_string(index + 1) + ".jsonl");
        std::filesystem::remove(target, ec);
        if (std::filesystem::exists(source, ec)) std::filesystem::rename(source, target, ec);
    }
    auto first = g_status.logDirectory / "host.1.jsonl";
    std::filesystem::remove(first, ec);
    std::filesystem::rename(g_status.activeLog, first, ec);
}

LONG WINAPI WriteCrashDump(EXCEPTION_POINTERS* exceptionPointers) {
    try {
        const auto directory = AppDataRoot() / L"dumps";
        std::filesystem::create_directories(directory);
        HardenPath(directory);
        const auto path = directory / ("host-" + FileTimestamp() + ".dmp");
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION info{};
            info.ThreadId = GetCurrentThreadId();
            info.ExceptionPointers = exceptionPointers;
            info.ClientPointers = FALSE;
			// Do not include broad process memory: it may contain session tokens,
			// TURN credentials, captured frame data, or user input.
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal,
                              exceptionPointers ? &info : nullptr, nullptr, nullptr);
            CloseHandle(file);
            HardenPath(path);
        }
    } catch (...) {}
    return EXCEPTION_EXECUTE_HANDLER;
}

nlohmann::json SanitizeJson(nlohmann::json value) {
    if (value.is_object()) {
        for (auto& [key, child] : value.items()) {
            std::string lowered = key;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (lowered == "roomid" || lowered == "pairingcode" || lowered == "sessionid" || lowered == "hostid" ||
				lowered.find("secret") != std::string::npos || lowered.find("token") != std::string::npos ||
                lowered.find("password") != std::string::npos || lowered.find("credential") != std::string::npos) {
                child = "[REDACTED]";
            } else child = SanitizeJson(std::move(child));
        }
    } else if (value.is_array()) {
        for (auto& child : value) child = SanitizeJson(std::move(child));
    } else if (value.is_string()) {
        value = Redact(value.get<std::string>());
    }
    return value;
}
}

bool Initialize() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_status.initialized) return true;
    std::error_code ec;
    g_status.logDirectory = AppDataRoot() / L"logs";
    std::filesystem::create_directories(g_status.logDirectory, ec);
    if (ec) return false;
    g_status.activeLog = g_status.logDirectory / L"host.jsonl";
    HardenPath(AppDataRoot());
    HardenPath(g_status.logDirectory);
    g_status.initialized = true;
    InstallConsoleCaptureLocked();
    return true;
}

void Shutdown() noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_originalCout) std::cout.rdbuf(g_originalCout);
    if (g_originalCerr) std::cerr.rdbuf(g_originalCerr);
    if (g_originalWcout) std::wcout.rdbuf(g_originalWcout);
    if (g_originalWcerr) std::wcerr.rdbuf(g_originalWcerr);
    g_coutTee.reset();
    g_cerrTee.reset();
    g_wcoutTee.reset();
    g_wcerrTee.reset();
    g_originalCout = nullptr;
    g_originalCerr = nullptr;
    g_originalWcout = nullptr;
    g_originalWcerr = nullptr;
    g_status.initialized = false;
}

void InstallCrashHandler() { SetUnhandledExceptionFilter(WriteCrashDump); }

std::string Redact(std::string value) {
    static const std::regex authorization(
        R"((authorization\s*[:=]\s*(?:bearer\s+)?)[^\s&,;\"}]+)",
        std::regex_constants::icase);
    static const std::regex sensitive(
        R"(((?:\"?[A-Za-z0-9_-]*(?:secret|token|password|credential)[A-Za-z0-9_-]*\"?\s*[:=]\s*\"?)|(?:[A-Za-z0-9_-]*(?:secret|token|password|credential)[A-Za-z0-9_-]*%3[dD]))[^\s&,;\"}]+)",
        std::regex_constants::icase);
	static const std::regex queryCredential(R"(([?&](?:token|accessToken)=)[^&#\s]+)", std::regex_constants::icase);
	static const std::regex uuid(R"(\b[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\b)",
		std::regex_constants::icase);
	static const std::regex roomCode(R"(\b[0-9a-f]{32}\b)", std::regex_constants::icase);
	static const std::regex ipv4(R"(\b(?:\d{1,3}\.){3}\d{1,3}\b)");
    value = std::regex_replace(value, authorization, "$1[REDACTED]");
	value = std::regex_replace(value, sensitive, "$1[REDACTED]");
	value = std::regex_replace(value, queryCredential, "$1[REDACTED]");
	value = std::regex_replace(value, uuid, "[SESSION_ID]");
	value = std::regex_replace(value, roomCode, "[PAIRING_CODE]");
	return std::regex_replace(value, ipv4, "[IP_ADDRESS]");
}

void Log(const std::string& severity, const std::string& category,
         const std::string& message, const std::string& details) {
    if (!Initialize()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    try {
        RotateLocked();
        std::error_code existsError;
        const bool newLogFile = !std::filesystem::exists(g_status.activeLog, existsError);
        std::ofstream stream(g_status.activeLog, std::ios::app | std::ios::binary);
        if (!stream) { ++g_status.writeFailures; return; }
        nlohmann::json record{{"timestamp", Timestamp()}, {"severity", severity}, {"category", category},
                              {"pid", GetCurrentProcessId()}, {"thread", GetCurrentThreadId()},
                              {"message", Redact(message)}};
        if (!details.empty()) record["details"] = Redact(details);
        stream << record.dump() << '\n';
        if (newLogFile) HardenPath(g_status.activeLog);
        ++g_status.recordsWritten;
    } catch (...) { ++g_status.writeFailures; }
}

Status GetStatus() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}

bool CreateSupportBundle(const nlohmann::json& health, const nlohmann::json& config,
                         std::filesystem::path& outputDirectory, std::string& error) {
    try {
        outputDirectory = AppDataRoot() / L"support" / std::filesystem::path(FileTimestamp());
        std::filesystem::create_directories(outputDirectory);
        HardenPath(AppDataRoot() / L"support");
        HardenPath(outputDirectory);
		std::ofstream(outputDirectory / L"health.json") << SanitizeJson(health).dump(2);
        std::ofstream(outputDirectory / L"config.sanitized.json") << SanitizeJson(config).dump(2);
        const auto status = GetStatus();
        std::error_code ec;
        if (!status.activeLog.empty() && std::filesystem::exists(status.activeLog, ec)) {
			std::ifstream input(status.activeLog, std::ios::binary);
			std::ofstream output(outputDirectory / L"host.jsonl", std::ios::binary | std::ios::trunc);
			std::string line;
			while (std::getline(input, line)) output << Redact(line) << '\n';
        }
        HardenPath(outputDirectory / L"health.json");
        HardenPath(outputDirectory / L"config.sanitized.json");
        if (std::filesystem::exists(outputDirectory / L"host.jsonl", ec)) HardenPath(outputDirectory / L"host.jsonl");
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}
} // namespace Diagnostics
