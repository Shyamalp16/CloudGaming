#include "ShutdownManager.h"

std::atomic<bool> ShutdownManager::g_shutdown_flag(false);

void ShutdownManager::SetShutdown(bool value) {
    g_shutdown_flag.store(value);
}

bool ShutdownManager::IsShutdown() {
    return g_shutdown_flag.load();
}