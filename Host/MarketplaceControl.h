#pragma once

#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

class MarketplaceControl final {
public:
    using CommandHandler = std::function<void(const nlohmann::json&)>;
    MarketplaceControl();
    ~MarketplaceControl();
    MarketplaceControl(const MarketplaceControl&) = delete;
    MarketplaceControl& operator=(const MarketplaceControl&) = delete;

    bool Start(const std::string& matchmakerUrl, const std::string& hostId,
               const std::string& hostSecret, CommandHandler handler, std::string& error);
    void Stop() noexcept;
    bool Send(const std::string& type, const std::string& sessionId,
              const nlohmann::json& payload = {}, const std::string& commandId = {});
    bool Connected() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
