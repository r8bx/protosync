/**
 * logger.cpp
 * ProtoSync — MeasureLogger implementation.
 */

#include "logger.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

#include <spdlog/spdlog.h>

/* ─────────────────────────────────────────────────────────────────────────
 * MeasureLogger::open
 * ───────────────────────────────────────────────────────────────────────── */
bool MeasureLogger::open(const std::string &dir)
{
    /* Build filename: protosync_YYYYMMDD_HHMMSS.csv */
    std::time_t now = std::time(nullptr);
    std::tm    *tm  = std::localtime(&now);
    char        ts[32];
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm);

    path_ = dir + "/protosync_" + ts + ".csv";

    std::lock_guard<std::mutex> lk(mtx_);
    file_.open(path_, std::ios::out | std::ios::trunc);
    if (!file_.is_open()) {
        spdlog::error("Logger: cannot open {}", path_);
        return false;
    }

    /* Record start time for elapsed_s */
    start_ns_ = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count());

    /* Write CSV header */
    file_ << "elapsed_s,"
          << "source,"
          << "bpm,"
          << "interval_mean_us,"
          << "interval_min_us,"
          << "interval_max_us,"
          << "interval_stddev_us,"
          << "jitter_last_us,"
          << "jitter_peak_us,"
          << "sync_error_us,"
          << "sample_count\n";
    file_.flush();

    spdlog::info("Logger: writing to {}", path_);
    return true;
}

/* ─────────────────────────────────────────────────────────────────────────
 * MeasureLogger::write_row  (private)
 * ───────────────────────────────────────────────────────────────────────── */
void MeasureLogger::write_row(double             elapsed_s,
                               const char        *source,
                               double             bpm,
                               const MeasureState &ms)
{
    /* mtx_ must already be held by the caller */
    file_ << std::fixed << std::setprecision(3) << elapsed_s  << ','
          << source                                            << ','
          << std::setprecision(3) << bpm                      << ','
          << std::setprecision(2) << ms.interval_mean_us      << ','
          << std::setprecision(2) << ms.interval_min_us       << ','
          << std::setprecision(2) << ms.interval_max_us       << ','
          << std::setprecision(2) << ms.interval_stddev_us    << ','
          << std::setprecision(2) << ms.jitter_last_us        << ','
          << std::setprecision(2) << ms.jitter_peak_us        << ','
          << std::setprecision(2) << ms.sync_error_us         << ','
          << ms.sample_count                                  << '\n';
}

/* ─────────────────────────────────────────────────────────────────────────
 * MeasureLogger::log
 * Called once per second from logger_thread_fn in main.cpp.
 * ───────────────────────────────────────────────────────────────────────── */
void MeasureLogger::log(double              bpm,
                         const MeasureState &midi,
                         const MeasureState &gpio)
{
    if (!file_.is_open()) return;

    uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count());

    double elapsed_s = static_cast<double>(now_ns - start_ns_) / 1e9;

    std::lock_guard<std::mutex> lk(mtx_);
    write_row(elapsed_s, "MIDI", bpm, midi);
    write_row(elapsed_s, "GPIO", bpm, gpio);
    file_.flush();   /* ensure data survives a crash */
}

/* ─────────────────────────────────────────────────────────────────────────
 * MeasureLogger::close
 * ───────────────────────────────────────────────────────────────────────── */
void MeasureLogger::close()
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (file_.is_open()) {
        file_.flush();
        file_.close();
        spdlog::info("Logger: closed {}", path_);
    }
}