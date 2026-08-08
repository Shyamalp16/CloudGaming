#include "SelfTests.h"

#include <iostream>
#include <string>

#include "InputSchema.h"
#include "Diagnostics.h"
#include "SessionManager.h"
#include "StreamProfileManager.h"
#include "Websocket.h"

namespace {
bool Check(bool condition, const char* name) {
    std::cout << "[self-test] " << (condition ? "PASS " : "FAIL ") << name << std::endl;
    return condition;
}
}

bool RunHostSelfTests() {
    bool passed = true;
    passed &= Check(InputSchema::Validate(R"({"type":"input_reset","reason":"test"})").valid,
                    "input reset schema");
    passed &= Check(!InputSchema::Validate(R"({"type":"stream_config","width":1920,"height":1080,"fps":60})").valid,
                    "profile rejected on input channel");
    passed &= Check(!InputSchema::Validate(std::string(5000, 'x')).valid, "oversized input rejected");
    const auto redacted = Diagnostics::Redact(
        R"(Authorization: Bearer topsecret pairingToken=abc123 "credential":"def456")");
    passed &= Check(redacted.find("topsecret") == std::string::npos &&
                    redacted.find("abc123") == std::string::npos && redacted.find("def456") == std::string::npos,
                    "diagnostic secret redaction");

    SessionManager sessions;
    const std::string sessionId = "01234567-89ab-4cde-8fab-0123456789ab";
    passed &= Check(sessions.Authorize(sessionId) && sessions.Accepts(sessionId), "session authorization");
    sessions.MarkConnected(sessionId);
    passed &= Check(sessions.GetStatus().state == SessionManager::State::Connected, "session connected transition");
    sessions.MarkReconnecting();
    passed &= Check(sessions.GetStatus().state == SessionManager::State::Reconnecting, "session reconnect transition");
    sessions.Terminate("test");
    passed &= Check(!sessions.Accepts(sessionId) && sessions.GetStatus().state == SessionManager::State::Idle,
                    "session termination reset");

    StreamProfileManager profiles;
    std::string error;
    nlohmann::json video{{"bitrateMin", 3000000}, {"bitrateMax", 12000000},
        {"defaultProfile", {{"width", 1920}, {"height", 1080}, {"fps", 60}, {"bitrate", 8000000}}}};
    passed &= Check(profiles.Configure(video, error) && profiles.TakePending().has_value(),
                    "default profile configuration");
    StreamProfileManager::ClientCapabilities caps{2560, 1440, 60, 12000000, true};
    passed &= Check(profiles.Request(sessionId, {1280, 720, 30, 4000000}, caps, error),
                    "supported negotiated profile");
    passed &= Check(!profiles.Request(sessionId, {3840, 2160, 60, 12000000}, caps, error),
                    "unsupported profile rejection");

    passed &= Check(WebsocketPolicy::ComputeReconnectDelayMs(0, 0.0) == 500,
                    "reconnect initial backoff");
    passed &= Check(WebsocketPolicy::ComputeReconnectDelayMs(20, 0.25) == 37500,
                    "reconnect bounded exponential backoff");
    return passed;
}
