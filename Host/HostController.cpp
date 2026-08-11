#include "pch.h"
#include "HostController.h"

#include <exception>

#include "Diagnostics.h"

HostController::HostController() {
    runtime_.SetStatusCallback([this](const Runtime::HostStatus&) { Notify(); });
}

HostController::~HostController() {
    runtime_.SetStatusCallback({});
    StopAsync();
    WaitForStop();
}

bool HostController::StartAsync() {
    std::thread completed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (workerActive_) return false;
        if (worker_.joinable()) completed = std::move(worker_);
    }
    if (completed.joinable()) completed.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        workerActive_ = true;
        worker_ = std::thread([this] {
            try {
                if (runtime_.Start()) runtime_.Run();
            } catch (const std::exception& ex) {
                Diagnostics::Log("ERROR", "LIFECYCLE", "Host worker failed", ex.what());
                runtime_.Stop();
            } catch (...) {
                Diagnostics::Log("ERROR", "LIFECYCLE", "Host worker failed", "unknown exception");
                runtime_.Stop();
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                workerActive_ = false;
            }
            Notify();
        });
    }
    Notify();
    return true;
}

void HostController::StopAsync() noexcept {
    runtime_.RequestStop();
}

void HostController::WaitForStop() noexcept {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (worker_.joinable()) worker = std::move(worker_);
    }
    if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) worker.join();
}

bool HostController::SelectTarget(const std::string& processName,
                                  const std::wstring& preferredTitle,
                                  std::string& error) {
    return runtime_.QueueTargetSelection(processName, preferredTitle, error);
}

void HostController::RequestPresenceRefresh() noexcept {
    runtime_.RequestPresenceRefresh();
}

Runtime::HostStatus HostController::GetStatus() const {
    return runtime_.GetStatus();
}

nlohmann::json HostController::GetHealthSnapshot() const {
    return runtime_.GetHealthSnapshot();
}

void HostController::SetStatusCallback(StatusCallback callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(callback);
    }
    Notify();
}

void HostController::Notify() {
    StatusCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = callback_;
    }
    if (callback) callback();
}
