#include <Windows.h>
#include <atomic>
#include <exception>
#include <iostream>

#include "AppInit.h"
#include "AgentServer.h"
#include "ConfigUtils.h"
#include "ConfigStore.h"
#include "Diagnostics.h"
#include "ErrorUtils.h"
#include "Runtime.h"
#include "SelfTests.h"
#include "SecretStore.h"
#include "TrayApplication.h"
#include "UpdateManager.h"
#include "Version.h"
#include "IdGenerator.h"

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
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << CLOUD_GAMING_VERSION << std::endl;
        return EXIT_SUCCESS;
    }
	if (argc == 2 && std::string(argv[1]) == "--device-id") {
		try { std::cout << loadOrCreateHostId() << std::endl; return EXIT_SUCCESS; }
		catch (const std::exception& ex) { std::cerr << ex.what() << std::endl; return EXIT_FAILURE; }
	}
    Diagnostics::Initialize();
    Diagnostics::InstallCrashHandler();
    if ((argc == 2 && std::string(argv[1]) == "--agent") ||
        (argc == 4 && std::string(argv[1]) == "--agent" && std::string(argv[2]) == "--pipe-name")) {
        const std::string pipeName = argc == 4 ? argv[3] : "ReflexGaming.HostAgent.v1";
        try {
            AppInit::InitializeProcess();
            AgentServer server(pipeName);
            const int result = server.Run();
            Diagnostics::Shutdown();
            return result;
        } catch (const std::exception& ex) {
            LOG_FATAL(ErrorUtils::ErrorCategory::SYSTEM, "Native agent failed", ex.what());
            Diagnostics::Shutdown();
            return EXIT_FAILURE;
        }
    }
    if (argc == 3 && std::string(argv[1]) == "--set-secret") {
        const std::string name = argv[2];
        if (name != "hostSecret" && name != "turnUrls" && name != "turnUsername" && name != "turnCredential") {
            std::cerr << "Unsupported secret name" << std::endl;
            Diagnostics::Shutdown();
            return EXIT_FAILURE;
        }
        std::string value, error;
        std::getline(std::cin, value);
        const bool saved = !value.empty() && SecretStore::Set(name, value, error);
        if (!saved) std::cerr << (error.empty() ? "Secret cannot be empty" : error) << std::endl;
        Diagnostics::Shutdown();
        return saved ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 4 && std::string(argv[1]) == "--configure-production") {
        nlohmann::json config;
        std::string error;
        const auto loaded = ConfigStore::Load(config);
        bool saved = loaded.success;
        if (saved) {
            config["network"]["mode"] = "production";
            config["network"]["production"]["signalingUrl"] = argv[2];
            config["network"]["production"]["matchmakerUrl"] = argv[3];
            config["setup"]["completed"] = true;
            saved = ConfigStore::Save(config, error);
        } else error = loaded.error;
        if (!saved) std::cerr << error << std::endl;
        Diagnostics::Shutdown();
        return saved ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string(argv[1]) == "--check-updates") {
        nlohmann::json config;
        const auto loaded = ConfigStore::Load(config);
        if (!loaded.success) {
            std::cerr << loaded.error << std::endl;
            Diagnostics::Shutdown();
            return EXIT_FAILURE;
        }
        const auto update = UpdateManager::Check(config);
        std::cout << update.message << std::endl;
        Diagnostics::Shutdown();
        return update.status == UpdateManager::Status::Error ? EXIT_FAILURE : EXIT_SUCCESS;
    }
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
    if (argc == 1) {
        try {
            AppInit::InitializeProcess();
            if (HWND console = GetConsoleWindow()) ShowWindow(console, SW_HIDE);
            const int result = RunTrayApplication();
            Diagnostics::Shutdown();
            return result;
        } catch (const std::exception& ex) {
            LOG_FATAL(ErrorUtils::ErrorCategory::SYSTEM, "Host UI failed", ex.what());
            Diagnostics::Shutdown();
            return EXIT_FAILURE;
        }
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
