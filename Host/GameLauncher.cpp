#include "GameLauncher.h"

#include <shellapi.h>

#include <algorithm>
#include <cwctype>

namespace {
std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
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
        const auto uri = L"steam://rungameid/" + game.launchTarget.wstring();
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr,
                                                    SW_SHOWNORMAL)) <= 32) {
            error = "Steam rejected the launch request"; Stop(); return false;
        }
        return true;
    }

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
