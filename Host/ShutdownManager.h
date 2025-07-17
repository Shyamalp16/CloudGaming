#pragma once

#include <atomic>

class ShutdownManager {
public:
    static void SetShutdown(bool value);
    static bool IsShutdown();

private:
    static std::atomic<bool> g_shutdown_flag;
};