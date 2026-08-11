#include "GameLauncher.h"

#include <shellapi.h>

#include <algorithm>
#include <cwctype>

#include "Diagnostics.h"

namespace {
std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

std::filesystem::path SteamExecutable() {
    wchar_t value[32768]{};
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                     RRF_RT_REG_SZ, nullptr, value, &bytes) != ERROR_SUCCESS) return {};
    return std::filesystem::path(value) / L"Steam.exe";
}
}

GameLauncher::~GameLauncher() { Stop(); }

bool GameLauncher::Start(const GameInventory::Game& game, std::string& error) {
    Stop();
    game_ = game;
    for (const auto& target : ProcessDiscovery::EnumerateTargets()) baseline_.insert(target.processId);
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job_) SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    if (game.source == "steam") {
        const auto steam = SteamExecutable();
        if (!std::filesystem::is_regular_file(steam)) {
            error = "Steam.exe was not found"; Stop(); return false;
        }
        const auto parameters = L"-applaunch " + game.launchTarget.wstring();
        SHELLEXECUTEINFOW launch{sizeof(launch)};
        launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        launch.lpVerb = L"open";
        launch.lpFile = steam.c_str();
        launch.lpParameters = parameters.c_str();
        launch.lpDirectory = steam.parent_path().c_str();
        launch.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&launch)) {
            error = "Steam rejected the launch request: " + std::to_string(GetLastError());
            Stop(); return false;
        }
        if (launch.hProcess) CloseHandle(launch.hProcess);
        Diagnostics::Log("INFO", "GAME", "Launching Steam offering through Steam client", game.id);
        return true;
    }

    Diagnostics::Log("INFO", "GAME", "Launching manual executable directly",
                     game.launchTarget.string());
    std::wstring command = L"\"" + game.launchTarget.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(game.launchTarget.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        game.launchTarget.parent_path().c_str(), &startup, &process)) {
        error = "CreateProcess failed: " + std::to_string(GetLastError()); Stop(); return false;
    }
    CloseHandle(process.hThread);
    process_ = process.hProcess;
    targetPid_ = process.dwProcessId;
    if (job_) jobOwnsProcess_ = AssignProcessToJobObject(job_, process_) != FALSE;
    return true;
}

std::optional<ProcessDiscovery::Target> GameLauncher::PollTarget() {
    const auto expected = game_.source == "manual" ? Lower(game_.launchTarget.filename().wstring()) : L"";
    std::optional<ProcessDiscovery::Target> best;
    for (const auto& target : ProcessDiscovery::EnumerateTargets()) {
        const auto process = Lower(target.processName);
        if (!expected.empty() && process != expected) continue;
        if (expected.empty() && (baseline_.count(target.processId) || process == L"steam.exe" ||
                                 process == L"explorer.exe")) continue;
        const auto area = static_cast<long long>(target.clientWidth) * target.clientHeight;
        const auto bestArea = best ? static_cast<long long>(best->clientWidth) * best->clientHeight : -1;
        if (area > bestArea) best = target;
    }
    if (best) {
        targetWindow_ = best->window;
        targetPid_ = best->processId;
        TrackProcess(targetPid_);
    }
    return best;
}

void GameLauncher::TrackProcess(DWORD pid) noexcept {
    if (process_ && GetProcessId(process_) == pid) return;
    if (process_) CloseHandle(process_);
    process_ = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_SET_QUOTA, FALSE, pid);
    jobOwnsProcess_ = process_ && job_ && AssignProcessToJobObject(job_, process_) != FALSE;
}

void GameLauncher::Stop() noexcept {
    if (targetWindow_ && IsWindow(targetWindow_)) PostMessageW(targetWindow_, WM_CLOSE, 0, 0);
    if (process_ && WaitForSingleObject(process_, 5000) == WAIT_TIMEOUT) {
        if (jobOwnsProcess_) TerminateJobObject(job_, 1);
        else TerminateProcess(process_, 1);
    }
    if (process_) CloseHandle(process_);
    if (job_) CloseHandle(job_);
    process_ = job_ = nullptr;
    jobOwnsProcess_ = false;
    targetWindow_ = nullptr;
    targetPid_ = 0;
    baseline_.clear();
    game_ = {};
}

bool GameLauncher::Running() const noexcept {
    return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}
