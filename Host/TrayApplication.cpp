#include "pch.h"
#include "TrayApplication.h"

#include <Windows.h>
#include <Shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "HostController.h"
#include "ConfigStore.h"
#include "Diagnostics.h"
#include "ProcessDiscovery.h"
#include "UpdateManager.h"

namespace {
constexpr wchar_t kWindowClass[] = L"CloudGamingHostWindow";
constexpr wchar_t kAppName[] = L"Cloud Gaming Host";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kStatusMessage = WM_APP + 2;
constexpr UINT kFirstRunMessage = WM_APP + 3;
constexpr UINT kUpdateCheckMessage = WM_APP + 4;
constexpr UINT kUpdateInstallMessage = WM_APP + 5;
constexpr UINT_PTR kMetricsTimer = 1;

enum ControlId : int {
    IdStartStop = 100, IdProcess, IdRefresh, IdApply, IdPairing, IdCopy, IdSettings, IdOpenLogs, IdCheckUpdates,
    IdTrayOpen = 200, IdTrayStartStop, IdTrayExit
};

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring MetricText(const nlohmann::json& health, int measuredFps) {
    try {
        const auto& encoder = health.at("encoder");
        const auto& network = health.at("network");
        const auto& audio = health.at("audio");
        const int videoBitrate = encoder.value("initialized", false) ? encoder.value("bitrate", 0) / 1000 : 0;
        return L"FPS: " + std::to_wstring(measuredFps) +
            L"     Video: " + std::to_wstring(videoBitrate) +
            L" kbps     Send: " + std::to_wstring(network.value("sendBitrateKbps", 0)) +
            L" kbps     RTT: " + std::to_wstring(network.value("rttMs", 0)) +
            L" ms     Audio: " + Utf8ToWide(audio.value("state", std::string{"Stopped"}));
    } catch (...) {
        return L"FPS: --     Video: --     Send: --     RTT: --     Audio: --";
    }
}

struct InstallResult { bool success = false; std::string error; };

class TrayApplication final {
public:
    int Run() {
        instance_ = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return EXIT_FAILURE;

        window_ = CreateWindowExW(0, kWindowClass, kAppName,
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT, CW_USEDEFAULT, 680, 410, nullptr, nullptr, instance_, this);
        if (!window_) return EXIT_FAILURE;
        controller_.SetStatusCallback([this] {
            if (const HWND window = window_) PostMessageW(window, kStatusMessage, 0, 0);
        });
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        controller_.SetStatusCallback({});
        controller_.StopAsync();
        controller_.WaitForStop();
        if (updateThread_.joinable()) updateThread_.join();
        return static_cast<int>(message.wParam);
    }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* app = reinterpret_cast<TrayApplication*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = static_cast<TrayApplication*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        return app ? app->HandleMessage(window, message, wParam, lParam)
                   : DefWindowProcW(window, message, wParam, lParam);
    }

    HWND AddStatic(const wchar_t* text, int x, int y, int width, int height) {
        return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
            x, y, width, height, window_, nullptr, instance_, nullptr);
    }

    HWND AddButton(const wchar_t* text, int id, int x, int y, int width) {
        return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            x, y, width, 30, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    }

    void CreateControls() {
        AddStatic(L"Host status", 22, 24, 105, 22);
        status_ = AddStatic(L"Stopped", 140, 24, 490, 22);
        AddStatic(L"Session", 22, 55, 105, 22);
        session_ = AddStatic(L"No active client", 140, 55, 490, 22);
        AddStatic(L"Pairing code", 22, 90, 105, 22);
        pairing_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"Start the host to create a code",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX,
            140, 86, 360, 27, window_, reinterpret_cast<HMENU>(IdPairing), instance_, nullptr);
        copy_ = AddButton(L"Copy", IdCopy, 515, 84, 110);

        AddStatic(L"Game / process", 22, 138, 105, 22);
        process_ = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            140, 134, 360, 260, window_, reinterpret_cast<HMENU>(IdProcess), instance_, nullptr);
        AddButton(L"Refresh", IdRefresh, 515, 132, 110);
        AddButton(L"Use selection", IdApply, 515, 171, 110);

        metrics_ = AddStatic(L"FPS: --     Video: --     Send: --     RTT: --     Audio: --", 22, 230, 610, 24);
        target_ = AddStatic(L"Target: none", 22, 265, 610, 24);
        failure_ = AddStatic(L"", 22, 296, 610, 38);
        startStop_ = AddButton(L"Start host", IdStartStop, 22, 330, 145);
        AddButton(L"Settings", IdSettings, 180, 330, 105);
        AddButton(L"Open logs", IdOpenLogs, 295, 330, 105);
        updateButton_ = AddButton(L"Check updates", IdCheckUpdates, 410, 330, 125);

        const auto font = reinterpret_cast<LPARAM>(GetStockObject(DEFAULT_GUI_FONT));
        EnumChildWindows(window_, [](HWND child, LPARAM value) -> BOOL {
            SendMessageW(child, WM_SETFONT, value, TRUE);
            return TRUE;
        }, font);
        RefreshProcesses();
        UpdateStatus();
    }

    void OpenConfiguration() {
        const auto path = ConfigStore::Path().wstring();
        ShellExecuteW(window_, L"open", L"notepad.exe", (L"\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    }

    void OpenLogs() {
        const auto status = Diagnostics::GetStatus();
        if (!status.activeLog.empty() && std::filesystem::exists(status.activeLog)) {
            const auto argument = L"\"" + status.activeLog.wstring() + L"\"";
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", L"notepad.exe",
                    argument.c_str(), status.logDirectory.c_str(), SW_SHOWNORMAL)) > 32) return;
        }
        if (!status.logDirectory.empty()) {
            ShellExecuteW(window_, L"explore", status.logDirectory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    void RunFirstRunFlow() {
        nlohmann::json config;
        std::string error;
        const auto loaded = ConfigStore::Load(config);
        if (!loaded.success || config.value("setup", nlohmann::json::object()).value("completed", false)) return;
        const int choice = MessageBoxW(window_,
            L"Welcome to Cloud Gaming Host.\n\nChoose Yes for local setup (services on this PC). "
            L"Choose No to edit production signaling and matchmaking endpoints first.",
            L"First-run setup", MB_YESNOCANCEL | MB_ICONINFORMATION);
        if (choice == IDCANCEL) return;
        if (choice == IDNO) {
            OpenConfiguration();
            MessageBoxW(window_, L"Set network.mode to production, enter HTTPS/WSS endpoints, save, then reopen the host.",
                        L"Production setup", MB_OK | MB_ICONINFORMATION);
            return;
        }
        config["network"]["mode"] = "local";
        config["setup"]["completed"] = true;
        if (!ConfigStore::Save(config, error)) {
            MessageBoxW(window_, Utf8ToWide(error).c_str(), L"Setup could not be saved", MB_OK | MB_ICONERROR);
            return;
        }
        MessageBoxW(window_, L"Local setup is ready. Select a game or application, click Use selection, then Start host.",
                    L"Setup complete", MB_OK | MB_ICONINFORMATION);
    }

    void CheckForUpdates() {
        if (updateBusy_) return;
        if (updateThread_.joinable()) updateThread_.join();
        nlohmann::json config;
        const auto loaded = ConfigStore::Load(config);
        if (!loaded.success) {
            MessageBoxW(window_, Utf8ToWide(loaded.error).c_str(), L"Update check", MB_OK | MB_ICONERROR);
            return;
        }
        updateBusy_ = true;
        EnableWindow(updateButton_, FALSE);
        const HWND notifyWindow = window_;
        updateThread_ = std::thread([notifyWindow, config = std::move(config)] {
            auto* result = new UpdateManager::Result(UpdateManager::Check(config));
            if (!IsWindow(notifyWindow) || !PostMessageW(notifyWindow, kUpdateCheckMessage, 0,
                    reinterpret_cast<LPARAM>(result))) delete result;
        });
    }

    void HandleUpdateCheck(UpdateManager::Result* rawResult) {
        std::unique_ptr<UpdateManager::Result> result(rawResult);
        if (updateThread_.joinable()) updateThread_.join();
        updateBusy_ = false;
        EnableWindow(updateButton_, TRUE);
        if (!result) return;
        if (result->status != UpdateManager::Status::Available) {
            const UINT icon = result->status == UpdateManager::Status::Error ? MB_ICONERROR : MB_ICONINFORMATION;
            MessageBoxW(window_, Utf8ToWide(result->message).c_str(), L"Update check", MB_OK | icon);
            return;
        }
        const auto prompt = Utf8ToWide(result->message + ". Download and install it now?");
        if (MessageBoxW(window_, prompt.c_str(), L"Update available", MB_YESNO | MB_ICONINFORMATION) != IDYES) return;
        updateBusy_ = true;
        EnableWindow(updateButton_, FALSE);
        const HWND notifyWindow = window_;
        updateThread_ = std::thread([notifyWindow, update = *result] {
            auto* installed = new InstallResult;
            installed->success = UpdateManager::DownloadVerifyAndLaunch(update, installed->error);
            if (!IsWindow(notifyWindow) || !PostMessageW(notifyWindow, kUpdateInstallMessage, 0,
                    reinterpret_cast<LPARAM>(installed))) delete installed;
        });
    }

    void HandleUpdateInstall(InstallResult* rawResult) {
        std::unique_ptr<InstallResult> result(rawResult);
        if (updateThread_.joinable()) updateThread_.join();
        updateBusy_ = false;
        EnableWindow(updateButton_, TRUE);
        if (!result || !result->success) {
            MessageBoxW(window_, result ? Utf8ToWide(result->error).c_str() : L"Update failed",
                        L"Update", MB_OK | MB_ICONERROR);
            return;
        }
        MessageBoxW(window_, L"The verified installer has started. Cloud Gaming Host will now exit.",
                    L"Update ready", MB_OK | MB_ICONINFORMATION);
        controller_.StopAsync();
        exiting_ = true;
        DestroyWindow(window_);
    }

    void AddTrayIcon() {
        tray_ = {sizeof(tray_)};
        tray_.hWnd = window_;
        tray_.uID = 1;
        tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        tray_.uCallbackMessage = kTrayMessage;
        tray_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(tray_.szTip, kAppName);
        Shell_NotifyIconW(NIM_ADD, &tray_);
        tray_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &tray_);
    }

    void ShowMainWindow() {
        ShowWindow(window_, SW_SHOW);
        SetForegroundWindow(window_);
    }

    void ShowTrayMenu() {
        const auto state = controller_.GetStatus().state;
        const bool active = state != Runtime::HostState::Stopped && state != Runtime::HostState::Failed;
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, IdTrayOpen, L"Open Cloud Gaming Host");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IdTrayStartStop, active ? L"Stop host" : L"Start host");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IdTrayExit, L"Exit");
        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(window_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, point.x, point.y, 0, window_, nullptr);
        DestroyMenu(menu);
    }

    void ToggleHost() {
        const auto state = controller_.GetStatus().state;
        if (state == Runtime::HostState::Stopped || state == Runtime::HostState::Failed) controller_.StartAsync();
        else controller_.StopAsync();
        UpdateStatus();
    }

    void RefreshProcesses() {
        processes_ = ProcessDiscovery::EnumerateTargets();
        SendMessageW(process_, CB_RESETCONTENT, 0, 0);
        for (const auto& process : processes_) {
            std::wstring label = process.processName;
            if (!process.title.empty()) label += L" — " + process.title;
            SendMessageW(process_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        if (!processes_.empty()) SendMessageW(process_, CB_SETCURSEL, 0, 0);
    }

    void ApplyTarget() {
        const LRESULT selected = SendMessageW(process_, CB_GETCURSEL, 0, 0);
        if (selected == CB_ERR || static_cast<size_t>(selected) >= processes_.size()) return;
        const auto& process = processes_[static_cast<size_t>(selected)];
        std::string error;
        if (!controller_.SelectTarget(WideToUtf8(process.processName), process.title, error))
            MessageBoxW(window_, Utf8ToWide(error).c_str(), kAppName, MB_OK | MB_ICONERROR);
    }

    void CopyPairingCode() {
        const auto code = Utf8ToWide(controller_.GetStatus().roomId);
        if (code.empty() || !OpenClipboard(window_)) return;
        EmptyClipboard();
        const size_t bytes = (code.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory) {
            void* target = GlobalLock(memory);
            memcpy(target, code.c_str(), bytes);
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        }
        CloseClipboard();
    }

    void UpdateStatus() {
        const auto status = controller_.GetStatus();
        const auto health = controller_.GetHealthSnapshot();
        const bool active = status.state != Runtime::HostState::Stopped && status.state != Runtime::HostState::Failed;
        const auto state = Utf8ToWide(Runtime::ToString(status.state));
        SetWindowTextW(status_, state.c_str());
        SetWindowTextW(startStop_, active ? L"Stop host" : L"Start host");
        std::string pairingCode = status.roomId;
        try {
            const auto healthCode = health.at("runtime").value("roomId", std::string{});
            if (!healthCode.empty()) pairingCode = healthCode;
        } catch (...) {}
        const auto code = pairingCode.empty() ? std::wstring{L"Start the host to create a code"}
                                              : Utf8ToWide(pairingCode);
        SendMessageW(pairing_, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(code.c_str()));
        const std::wstring process = status.targetProcessName.empty() ? L"none" : Utf8ToWide(status.targetProcessName);
        const std::wstring target = status.targetPid
            ? L"Target: " + process + L" (PID " + std::to_wstring(status.targetPid) + L")"
            : L"Target: " + process;
        SetWindowTextW(target_, target.c_str());
        std::string failureMessage = status.failureReason;
        try {
            const auto& audio = health.at("audio");
            if (failureMessage.empty() && audio.value("state", std::string{}) == "Failed") {
                failureMessage = "Audio: " + audio.value("failureReason", std::string{"capture failed"});
            }
        } catch (...) {}
        const auto failure = Utf8ToWide(failureMessage);
        SetWindowTextW(failure_, failure.c_str());
        std::wstring sessionText = L"No active client";
        try {
            const auto sessionState = Utf8ToWide(health.at("session").value("state", std::string{"Idle"}));
            const auto sessionId = Utf8ToWide(health.at("session").value("sessionId", std::string{}));
            sessionText = sessionState;
            if (!sessionId.empty()) sessionText += L" — " + sessionId;
        } catch (...) {}
        SetWindowTextW(session_, sessionText.c_str());
        const auto now = std::chrono::steady_clock::now();
        try {
            const uint64_t frames = health.at("capture").value("framesArrived", uint64_t{0});
            if (frames < lastFrameCount_) { lastFrameCount_ = frames; lastFpsSample_ = now; measuredFps_ = 0; }
            if (lastFpsSample_ != std::chrono::steady_clock::time_point{}) {
                const auto elapsed = std::chrono::duration<double>(now - lastFpsSample_).count();
                if (elapsed >= 0.5) measuredFps_ = static_cast<int>((frames - lastFrameCount_) / elapsed + 0.5);
            }
            if (lastFpsSample_ == std::chrono::steady_clock::time_point{} ||
                now - lastFpsSample_ >= std::chrono::milliseconds(500)) {
                lastFrameCount_ = frames;
                lastFpsSample_ = now;
            }
        } catch (...) {}
        const auto metrics = MetricText(health, measuredFps_);
        SetWindowTextW(metrics_, metrics.c_str());
        EnableWindow(copy_, !pairingCode.empty());
    }

    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            window_ = window;
            CreateControls();
            AddTrayIcon();
            SetTimer(window_, kMetricsTimer, 1000, nullptr);
            PostMessageW(window_, kFirstRunMessage, 0, 0);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case IdStartStop: case IdTrayStartStop: ToggleHost(); return 0;
            case IdRefresh: RefreshProcesses(); return 0;
            case IdApply: ApplyTarget(); return 0;
            case IdCopy: CopyPairingCode(); return 0;
            case IdSettings: OpenConfiguration(); return 0;
            case IdOpenLogs: OpenLogs(); return 0;
            case IdCheckUpdates: CheckForUpdates(); return 0;
            case IdTrayOpen: ShowMainWindow(); return 0;
            case IdTrayExit: exiting_ = true; DestroyWindow(window_); return 0;
            }
            break;
        case WM_TIMER:
            if (wParam == kMetricsTimer) UpdateStatus();
            return 0;
        case kStatusMessage: UpdateStatus(); return 0;
        case kFirstRunMessage: RunFirstRunFlow(); return 0;
        case kUpdateCheckMessage: HandleUpdateCheck(reinterpret_cast<UpdateManager::Result*>(lParam)); return 0;
        case kUpdateInstallMessage: HandleUpdateInstall(reinterpret_cast<InstallResult*>(lParam)); return 0;
        case kTrayMessage:
            if (LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == NIN_SELECT) ShowMainWindow();
            else if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) ShowTrayMenu();
            return 0;
        case WM_CLOSE:
            if (!exiting_) { ShowWindow(window_, SW_HIDE); return 0; }
            break;
        case WM_DESTROY:
            KillTimer(window_, kMetricsTimer);
            Shell_NotifyIconW(NIM_DELETE, &tray_);
            window_ = nullptr;
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND status_ = nullptr;
    HWND session_ = nullptr;
    HWND pairing_ = nullptr;
    HWND copy_ = nullptr;
    HWND process_ = nullptr;
    HWND metrics_ = nullptr;
    HWND target_ = nullptr;
    HWND failure_ = nullptr;
    HWND startStop_ = nullptr;
    HWND updateButton_ = nullptr;
    NOTIFYICONDATAW tray_{};
    bool exiting_ = false;
    bool updateBusy_ = false;
    std::thread updateThread_;
    uint64_t lastFrameCount_ = 0;
    int measuredFps_ = 0;
    std::chrono::steady_clock::time_point lastFpsSample_{};
    HostController controller_;
    std::vector<ProcessDiscovery::Target> processes_;
};
}

int RunTrayApplication() {
    TrayApplication app;
    return app.Run();
}
