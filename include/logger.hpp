/**
 * logger.hpp
 * ProtoSync — real-time measurement CSV logger.
 *
 * Logs one row per second for both MIDI (24 PPQN) and GPIO (4 PPQN) clocks.
 * File is created on start with a timestamp in the filename:
 *   protosync_YYYYMMDD_HHMMSS.csv
 *
 * A companion Python script (to_excel.py) converts the CSV to a formatted
 * .xlsx workbook with summary statistics and jitter charts.
 */

#pragma once

#include "ui.hpp"   /* MeasureState */

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

/* ─────────────────────────────────────────────────────────────────────────
 * CSV column layout (matches to_excel.py expectations — do not reorder)
 *
 *  0  elapsed_s          seconds since logging started
 *  1  source             "MIDI" or "GPIO"
 *  2  bpm                Link BPM at sample time
 *  3  interval_mean_us   rolling mean tick interval
 *  4  interval_min_us    rolling min
 *  5  interval_max_us    rolling max
 *  6  interval_stddev_us 1-sigma jitter
 *  7  jitter_last_us     signed jitter of most recent tick
 *  8  jitter_peak_us     max |jitter| since last reset
 *  9  sync_error_us      MIDI vs Link phase error (0 for GPIO)
 * 10  sample_count       total ticks since last reset
 * ───────────────────────────────────────────────────────────────────────── */

class MeasureLogger {
public:
    /* Opens the log file.  Returns true on success. */
    bool open(const std::string &dir = ".");

    /* Writes one row for each source.  Thread-safe. */
    void log(double              bpm,
             const MeasureState &midi,
             const MeasureState &gpio);

    /* Flushes and closes the file.  Safe to call multiple times. */
    void close();

    /* Returns the path of the currently-open log file. */
    const std::string &path() const { return path_; }

    bool is_open() const { return file_.is_open(); }

private:
    std::ofstream file_;
    std::mutex    mtx_;
    std::string   path_;

    /* Monotonic start time for elapsed_s column */
    uint64_t start_ns_ = 0;

    void write_row(double elapsed_s,
                   const char        *source,
                   double             bpm,
                   const MeasureState &ms);
};