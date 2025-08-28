#pragma once

#include <string>

namespace InputSchema {
    // Common JSON field names
    static constexpr const char* kType = "type";
    static constexpr const char* kCode = "code";
    static constexpr const char* kClientSendTime = "client_send_time";

    // Mouse fields
    static constexpr const char* kX = "x";
    static constexpr const char* kY = "y";
    static constexpr const char* kButton = "button";

    // Event type values
    static constexpr const char* kKeyDown = "keydown";
    static constexpr const char* kKeyUp   = "keyup";
    static constexpr const char* kMouseMove = "mousemove";
    static constexpr const char* kMouseDown = "mousedown";
    static constexpr const char* kMouseUp   = "mouseup";
}


