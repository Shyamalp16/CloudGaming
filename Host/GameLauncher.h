#pragma once

#include <Windows.h>

#include <optional>
#include <string>
#include <unordered_set>

#include "GameInventory.h"
#include "ProcessDiscovery.h"

class GameLauncher final {
public:
    ~GameLauncher();
    bool Start(const GameInventory::Game& game, std::string& error);
    std::optional<ProcessDiscovery::Target> PollTarget();
    void Stop() noexcept;
    bool Running() const noexcept;

private:
    void TrackProcess(DWORD pid) noexcept;

    GameInventory::Game game_;
    std::unordered_set<DWORD> baseline_;
    HANDLE process_ = nullptr;
    HANDLE job_ = nullptr;
    bool jobOwnsProcess_ = false;
    HWND targetWindow_ = nullptr;
    DWORD targetPid_ = 0;
};
