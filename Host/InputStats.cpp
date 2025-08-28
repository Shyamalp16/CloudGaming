#include "InputStats.h"
#include <sstream>
#include <iomanip>

namespace InputStats {

// Global statistics instance
InputHealthStats globalInputStats;

// StatsLogger implementation
StatsLogger::StatsLogger()
    : start_time_(std::chrono::steady_clock::now()) {
}

StatsLogger::~StatsLogger() {
    stop();
}

void StatsLogger::start() {
    if (running_.load()) return;

    running_.store(true);
    logger_thread_ = std::thread(&StatsLogger::loggerLoop, this);
}

void StatsLogger::stop() {
    if (!running_.load()) return;

    running_.store(false);
    if (logger_thread_.joinable()) {
        logger_thread_.join();
    }
}

std::string StatsLogger::getStatsString() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::stringstream ss;
    ss << std::fixed << std::setprecision(3);

    // Update computed metrics
    globalInputStats.updateComputedMetrics();

    auto uptime = globalInputStats.uptime_seconds.load();

    ss << "[InputStats] Uptime: " << uptime << "s | ";

    // Keyboard stats
    auto& kb = globalInputStats.keyboard;
    auto blockedKeys = InputMetrics::load(InputMetrics::keysBlocked());
    ss << "KB: " << kb.events_received.load() << " in, "
       << kb.events_injected.load() << " inj, "
       << kb.events_dropped_invalid.load() << " drop, "
       << kb.events_skipped_foreground.load() << " skip, "
       << blockedKeys << " blocked, "
       << kb.modifier_timeout_released.load() << " mod_timeout, "
       << kb.regular_timeout_released.load() << " reg_timeout, "
       << kb.events_stale_ignored.load() << " stale, "
       << kb.recovery_success_count.load() << " recovered, "
       << kb.release_all_count.load() << " emergency | ";

    // Mouse stats
    auto& mouse = globalInputStats.mouse;
    ss << "Mouse: " << mouse.events_received.load() << " in, "
       << mouse.events_injected.load() << " inj, "
       << mouse.events_skipped_foreground.load() << " skip, "
       << mouse.moves_coalesced.load() << " coal, "
       << mouse.clicks_processed.load() << " click, "
       << mouse.wheel_events.load() << " wheel | ";

    // Coordinate transformation stats
    auto coordSuccess = InputMetrics::load(InputMetrics::mouseCoordTransformSuccess());
    auto coordErrors = InputMetrics::load(InputMetrics::mouseCoordTransformErrors());
    auto coordClipped = InputMetrics::load(InputMetrics::mouseCoordClipped());

    if (coordSuccess + coordErrors > 0) {
        ss << "Coord: " << coordSuccess << " success, " << coordErrors << " err, " << coordClipped << " clip | ";
    }

    // Rates
    ss << "Rates: KB "
       << (kb.injection_success_rate * 100.0) << "% inj, "
       << (kb.drop_rate * 100.0) << "% drop, "
       << (kb.skip_rate * 100.0) << "% skip | Mouse "
       << (mouse.injection_success_rate * 100.0) << "% inj, "
       << (mouse.skip_rate * 100.0) << "% skip, "
       << (mouse.coalesce_rate * 100.0) << "% coal";

    // Processing time
    auto avg_time = globalInputStats.avg_processing_time_us.load();
    if (avg_time > 0) {
        ss << " | Avg Process: " << avg_time << "us";
    }

    return ss.str();
}

void StatsLogger::logStats() {
    std::cout << getStatsString() << std::endl;
}

void StatsLogger::loggerLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1)); // Log every 1 second

        updateUptime();
        logStats();
    }
}

void StatsLogger::updateUptime() {
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_);
    globalInputStats.uptime_seconds.store(duration.count());
}

// Global stats logger instance (defined in cpp, declared extern in header)
StatsLogger globalStatsLogger;

} // namespace InputStats

// Initialize stats logging when the module is loaded
struct StatsInitializer {
    StatsInitializer() {
        InputStats::globalStatsLogger.start();
    }
    ~StatsInitializer() {
        InputStats::globalStatsLogger.stop();
    }
};

// Static initializer to start stats logging
static StatsInitializer statsInitializer;
