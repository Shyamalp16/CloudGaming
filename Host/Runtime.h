#pragma once

#include <string>

namespace Runtime {
    // Print banner and room id
    void PrintBanner(const std::string& roomId);

    // Monitor WebRTC connection state (logs only)
    void MonitorConnection();
}


