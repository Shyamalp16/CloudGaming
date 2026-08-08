#include <Windows.h>
#include <atomic>
#include <exception>
#include <iostream>

#include "AppInit.h"
#include "ConfigUtils.h"
#include "Diagnostics.h"
#include "ErrorUtils.h"
#include "Runtime.h"
#include "SelfTests.h"

namespace {
std::atomic<Runtime::HostRuntime*> g_runtime{nullptr};

BOOL WINAPI ConsoleControlHandler(DWORD controlType) {
    switch (controlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (auto* runtime = g_runtime.load(std::memory_order_acquire)) {
            runtime->RequestStop();
            return TRUE;
        }
        return FALSE;
    default:
        return FALSE;
    }
}
}

int main(int argc, char** argv) {
    Diagnostics::Initialize();
    Diagnostics::InstallCrashHandler();
    if (argc == 2 && std::string(argv[1]) == "--self-test") {
        const bool passed = RunHostSelfTests();
        Diagnostics::Shutdown();
        return passed ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string(argv[1]) == "--support-bundle") {
        Diagnostics::Log("INFO", "DIAGNOSTICS", "offline support bundle requested");
        nlohmann::json config;
        ConfigUtils::LoadConfig(config);
        std::filesystem::path output;
        std::string error;
        const nlohmann::json health{{"runtime", {{"state", "Offline"},
            {"note", "Bundle generated without starting capture services"}}}};
        const bool created = Diagnostics::CreateSupportBundle(health, config, output, error);
        if (created) std::cout << "Support bundle: " << output.string() << std::endl;
        else std::cerr << "Support bundle failed: " << error << std::endl;
        Diagnostics::Shutdown();
        return created ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    Runtime::HostRuntime runtime;
    g_runtime.store(&runtime, std::memory_order_release);
    SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);

    int exitCode = EXIT_FAILURE;
    try {
        AppInit::InitializeProcess();
        if (runtime.Start()) {
            runtime.Run();
            exitCode = EXIT_SUCCESS;
        } else {
            const auto status = runtime.GetStatus();
            LOG_FATAL(ErrorUtils::ErrorCategory::SYSTEM, "Host failed to start", status.failureReason);
        }
    } catch (const std::exception& ex) {
        LOG_FATAL(ErrorUtils::ErrorCategory::SYSTEM, "Unhandled exception", ex.what());
        runtime.RequestStop();
        runtime.Stop();
    } catch (...) {
        LOG_FATAL(ErrorUtils::ErrorCategory::SYSTEM, "Unhandled non-standard exception", "unknown");
        runtime.RequestStop();
        runtime.Stop();
    }

    SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
    g_runtime.store(nullptr, std::memory_order_release);
    Diagnostics::Shutdown();
    return exitCode;
}
