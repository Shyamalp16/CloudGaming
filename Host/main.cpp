#include <Windows.h>
#include <atomic>
#include <exception>
#include <iostream>

#include "AppInit.h"
#include "Runtime.h"

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

int main() {
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
            std::cerr << "[main] Host failed to start: " << status.failureReason << std::endl;
        }
    } catch (const std::exception& ex) {
        std::cerr << "[main] Unhandled exception: " << ex.what() << std::endl;
        runtime.RequestStop();
        runtime.Stop();
    } catch (...) {
        std::cerr << "[main] Unhandled non-standard exception" << std::endl;
        runtime.RequestStop();
        runtime.Stop();
    }

    SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
    g_runtime.store(nullptr, std::memory_order_release);
    return exitCode;
}
