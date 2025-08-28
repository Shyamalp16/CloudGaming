#include "pch.h"
#include "Runtime.h"
#include "pion_webrtc.h"
#include <iostream>

namespace Runtime {

void PrintBanner(const std::string& roomId)
{
    std::wcout << L"\n----------------------------------------\n";
    std::wcout << L"  Cloud Gaming Host Initialized\n";
    std::wcout << L"  Your Room ID is: " << winrt::to_hstring(roomId).c_str() << L"\n";
    std::wcout << L"  Please copy this ID into the web client.\n";
    std::wcout << L"----------------------------------------\n\n";
}

void MonitorConnection()
{
    int state = getPeerConnectionState();
    if (state == 4 || state == 5 || state == 6) {
        std::wcout << L"[runtime] Peer disconnected (state=" << state << L"). Keeping host alive for reconnection." << std::endl;
    }
}

}


