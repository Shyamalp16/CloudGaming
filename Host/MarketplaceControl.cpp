#include "MarketplaceControl.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <boost/asio/ssl.hpp>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>

using PlainClient = websocketpp::client<websocketpp::config::asio_client>;
using TlsClient = websocketpp::client<websocketpp::config::asio_tls_client>;

struct MarketplaceControl::Impl {
    PlainClient plain;
    TlsClient tls;
    std::thread worker;
    std::atomic<bool> stop{false};
    std::atomic<bool> connected{false};
    std::mutex mutex;
    std::condition_variable wake;
    websocketpp::connection_hdl connection;
    CommandHandler handler;
    std::string url;
    std::string secret;
    bool useTls = false;

    template <typename Client>
    void Configure(Client& client) {
        client.clear_access_channels(websocketpp::log::alevel::all);
        client.clear_error_channels(websocketpp::log::elevel::all);
        client.init_asio();
        client.set_open_handler([this](websocketpp::connection_hdl hdl) {
            std::lock_guard lock(mutex); connection = hdl; connected = true;
        });
        client.set_close_handler([this](websocketpp::connection_hdl) { connected = false; });
        client.set_fail_handler([this](websocketpp::connection_hdl) { connected = false; });
        client.set_message_handler([this](websocketpp::connection_hdl, auto message) {
            if (message->get_payload().size() > 16 * 1024) return;
            auto value = nlohmann::json::parse(message->get_payload(), nullptr, false);
            if (value.is_object() && handler) handler(value);
        });
    }

    template <typename Client>
    void Run(Client& client) {
        while (!stop) {
            websocketpp::lib::error_code error;
            auto candidate = client.get_connection(url, error);
            if (!error) {
                candidate->append_header("Authorization", "Bearer " + secret);
                client.connect(candidate);
                client.run();
                client.reset();
            }
            connected = false;
            std::unique_lock lock(mutex);
            wake.wait_for(lock, std::chrono::seconds(2), [this] { return stop.load(); });
        }
    }
};

MarketplaceControl::MarketplaceControl() : impl_(std::make_unique<Impl>()) {}
MarketplaceControl::~MarketplaceControl() { Stop(); }

bool MarketplaceControl::Start(const std::string& matchmakerUrl, const std::string& hostId,
                               const std::string& hostSecret, CommandHandler handler,
                               std::string& error) {
    if (impl_->worker.joinable()) return true;
    if (matchmakerUrl.rfind("https://", 0) == 0)
        impl_->url = "wss://" + matchmakerUrl.substr(8);
    else if (matchmakerUrl.rfind("http://", 0) == 0)
        impl_->url = "ws://" + matchmakerUrl.substr(7);
    else { error = "Matchmaker URL must use HTTP or HTTPS"; return false; }
    while (!impl_->url.empty() && impl_->url.back() == '/') impl_->url.pop_back();
    impl_->url += "/api/v1/host/control?hostId=" + hostId;
    impl_->secret = hostSecret;
    impl_->handler = std::move(handler);
    impl_->useTls = impl_->url.rfind("wss://", 0) == 0;
    impl_->stop = false;

    if (impl_->useTls) {
        impl_->Configure(impl_->tls);
        impl_->tls.set_tls_init_handler([this](websocketpp::connection_hdl hdl) {
            auto context = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_client);
            context->set_default_verify_paths();
            context->set_verify_mode(boost::asio::ssl::verify_peer);
            const auto connection = impl_->tls.get_con_from_hdl(hdl);
            context->set_verify_callback(boost::asio::ssl::host_name_verification(
                connection->get_uri()->get_host()));
            return context;
        });
        impl_->worker = std::thread([this] { impl_->Run(impl_->tls); });
    } else {
        impl_->Configure(impl_->plain);
        impl_->worker = std::thread([this] { impl_->Run(impl_->plain); });
    }
    return true;
}

void MarketplaceControl::Stop() noexcept {
    if (!impl_ || !impl_->worker.joinable()) return;
    impl_->stop = true;
    impl_->wake.notify_all();
    websocketpp::lib::error_code error;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->connected) {
            if (impl_->useTls) impl_->tls.close(impl_->connection, websocketpp::close::status::normal, "Stopping", error);
            else impl_->plain.close(impl_->connection, websocketpp::close::status::normal, "Stopping", error);
        }
    }
    if (impl_->useTls) impl_->tls.stop(); else impl_->plain.stop();
    impl_->worker.join();
    impl_->connected = false;
    if (!impl_->secret.empty()) SecureZeroMemory(impl_->secret.data(), impl_->secret.size());
    impl_->secret.clear();
}

bool MarketplaceControl::Send(const std::string& type, const std::string& sessionId,
                              const nlohmann::json& payload, const std::string& commandId) {
    if (!impl_->connected) return false;
    nlohmann::json event{{"type", type}, {"payload", payload}};
    if (!sessionId.empty()) event["sessionId"] = sessionId;
    if (!commandId.empty()) event["commandId"] = commandId;
    const auto body = event.dump();
    websocketpp::lib::error_code error;
    std::lock_guard lock(impl_->mutex);
    if (impl_->useTls) impl_->tls.send(impl_->connection, body, websocketpp::frame::opcode::text, error);
    else impl_->plain.send(impl_->connection, body, websocketpp::frame::opcode::text, error);
    return !error;
}

bool MarketplaceControl::Connected() const noexcept { return impl_->connected; }
