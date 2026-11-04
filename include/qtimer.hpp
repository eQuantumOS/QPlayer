/// @file qtimer.hpp
/// @brief Lightweight monotonic stopwatch for examples and benchmarks.
/// @ingroup core
///
/// Header-only utility (like types.hpp / error.hpp): no .cpp counterpart.
/// start()/end() bracket a single region; lapStart()/lapStop() accumulate
/// several regions into one total, finalised by lapFinish().
///
/// CLOCK_MONOTONIC is used because it is immune to wall-clock adjustments (NTP,
/// manual changes), so it measures true elapsed time. show() writes program
/// output to stdout — it is not a diagnostic logger, so it is outside the
/// qplayer::log policy (which governs std::cerr / std::clog).

#pragma once

#include <cstdarg>
#include <cstdio>
#include <time.h>   // POSIX clock_gettime / CLOCK_MONOTONIC (Linux only)

namespace qplayer {

/// Monotonic stopwatch. Bracket a region with start()/end(), or accumulate
/// several with lapStart()/lapStop() and finalise via lapFinish().
class QTimer {
public:
    QTimer() { reset(); }

    /// Begin a single timed region.
    void start() { clock_gettime(CLOCK_MONOTONIC, &start_); }

    /// End the region opened by start(): elapsed total = end - start.
    void end() {
        clock_gettime(CLOCK_MONOTONIC, &end_);
        elapsed_ns_ = diff_ns(start_, end_);
        calculateTime();
    }

    /// Zero the accumulated total. Call before the first lapStart() when reusing
    /// a timer for a fresh accumulating session.
    void reset() {
        elapsed_ns_  = 0.0;
        elapsed_us_  = 0.0;
        elapsed_ms_  = 0.0;
        elapsed_sec_ = 0.0;
        time_str_[0] = '\0';
    }

    /// Accumulating laps: each lapStart()/lapStop() pair adds its region to the
    /// running total; lapFinish() stops the final region and formats the total.
    void lapStart() { clock_gettime(CLOCK_MONOTONIC, &start_); }
    void lapStop() {
        clock_gettime(CLOCK_MONOTONIC, &end_);
        elapsed_ns_ += diff_ns(start_, end_);
    }
    void lapFinish() {
        lapStop();
        calculateTime();
    }

    /// Derive the sec/ms/us scales and the compact display string from the
    /// nanosecond total. hh:mm:ss for >= 1 s, else the largest unit that is >= 1.
    void calculateTime() {
        elapsed_us_  = elapsed_ns_ / 1.0e3;
        elapsed_ms_  = elapsed_ns_ / 1.0e6;
        elapsed_sec_ = elapsed_ns_ / 1.0e9;

        if (elapsed_sec_ >= 1.0) {
            const long total = static_cast<long>(elapsed_sec_);
            const int  h = static_cast<int>(total / 3600);
            const int  m = static_cast<int>((total / 60) % 60);
            const int  s = static_cast<int>(total % 60);
            std::snprintf(time_str_, sizeof(time_str_), "%02d:%02d:%02d", h, m, s);
        } else if (elapsed_ms_ >= 1.0) {
            std::snprintf(time_str_, sizeof(time_str_), "%.1f ms", elapsed_ms_);
        } else if (elapsed_us_ >= 1.0) {
            std::snprintf(time_str_, sizeof(time_str_), "%.1f us", elapsed_us_);
        } else {
            std::snprintf(time_str_, sizeof(time_str_), "%.1f ns", elapsed_ns_);
        }
    }

    /// Print the formatted elapsed time to stdout.
    void show() const { std::printf("%s\n", time_str_); }

    /// Print a printf-style label followed by the elapsed time. The format
    /// attribute lets the compiler type-check the variadic arguments.
    __attribute__((format(printf, 2, 3)))
    void show(const char* fmt, ...) const {
        char    log[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(log, sizeof(log), fmt, ap);
        va_end(ap);
        std::printf("%s %s\n", log, time_str_);
    }

    /// Accessors (valid after end() / lapFinish()).
    const char* getTime()       const { return time_str_; }
    double      getElapsedSec()  const { return elapsed_sec_; }
    double      getElapsedMSec() const { return elapsed_ms_; }
    double      getElapsedUSec() const { return elapsed_us_; }
    double      getElapsedNSec() const { return elapsed_ns_; }

private:
    /// Nanoseconds between two monotonic samples (a -> b). Computed in double so
    /// a negative tv_nsec delta is correctly offset by the tv_sec delta.
    static double diff_ns(const timespec& a, const timespec& b) {
        return static_cast<double>(b.tv_sec - a.tv_sec) * 1.0e9 +
               static_cast<double>(b.tv_nsec - a.tv_nsec);
    }

    timespec start_{};
    timespec end_{};
    double   elapsed_ns_  = 0.0;
    double   elapsed_us_  = 0.0;
    double   elapsed_ms_  = 0.0;
    double   elapsed_sec_ = 0.0;
    char     time_str_[32] = {};
};

}  // namespace qplayer
