/*
 * progress_bar.hpp - A lightweight terminal progress bar for stderr
 *
 * Usage:
 *   ProgressBar bar(total, "label");   // total=0 → count-only mode
 *   for (...) { do_work(); bar.update(++done); }
 *   bar.finish(done);
 *
 * The bar is a no-op when stderr is not a TTY (CI, log files, pipes to file).
 * It uses ANSI escape "\r\033[K" so it never scrolls the terminal.
 * Redraws are throttled to ≤20 fps to keep CPU overhead negligible.
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <unistd.h>

class ProgressBar {
public:
    /// @param total  Expected item count; 0 means unknown (count-only mode).
    /// @param label  Short prefix printed before the bar (may be "").
    ProgressBar(std::size_t total, const char* label = "")
        : total_(total)
        , label_(label)
        , active_(isatty(STDERR_FILENO) != 0)
        , t0_(std::chrono::steady_clock::now()) {}

    ~ProgressBar() {
        // Clear the bar line so subsequent stderr output is not garbled.
        if (active_) std::cerr << "\r\033[K" << std::flush;
    }

    /// Call after every completed item to refresh the display.
    void update(std::size_t n) {
        if (!active_) return;
        auto now = std::chrono::steady_clock::now();
        // Throttle to ~20 redraws/s so the callback is essentially free.
        if (n != total_ &&
            std::chrono::duration<double>(now - lastDraw_).count() < 0.05)
            return;
        lastDraw_ = now;
        redraw(n, now);
    }

    /// Call once after all items are processed; prints a final newline.
    void finish(std::size_t n) {
        if (!active_) return;
        redraw(n, std::chrono::steady_clock::now());
        std::cerr << '\n' << std::flush;
        active_ = false;
    }

private:
    static constexpr int BAR_W = 28;   // visual bar width in characters

    void redraw(std::size_t n,
                std::chrono::steady_clock::time_point now) {
        double elapsed = std::chrono::duration<double>(now - t0_).count();
        double frac    = (total_ > 0)
                         ? std::min(1.0, static_cast<double>(n) / total_)
                         : 0.0;
        int    filled  = static_cast<int>(frac * BAR_W);
        double rate    = (elapsed > 0.0) ? n / elapsed : 0.0;
        double eta     = (rate > 0.0 && total_ > 0 && n < total_)
                         ? static_cast<double>(total_ - n) / rate : 0.0;

        std::cerr << "\r\033[K";   // cursor to column 0, erase rest of line

        if (label_[0]) std::cerr << label_ << ' ';

        // Bar + percentage (only when total is known)
        if (total_ > 0) {
            std::cerr << '[';
            for (int i = 0; i < filled;      ++i) std::cerr << '#';
            for (int i = filled; i < BAR_W;  ++i) std::cerr << '-';
            std::cerr << "] "
                      << std::setw(3) << static_cast<int>(frac * 100) << "% | ";
        }

        // Item count
        std::cerr << fmtCount(n);
        if (total_ > 0) std::cerr << '/' << fmtCount(total_);

        // Throughput
        if (rate > 0.0) {
            std::cerr << " | ";
            if      (rate >= 1e6) std::cerr << std::fixed << std::setprecision(1) << (rate / 1e6) << "M/s";
            else if (rate >= 1e3) std::cerr << std::fixed << std::setprecision(1) << (rate / 1e3) << "k/s";
            else                  std::cerr << std::fixed << std::setprecision(0) << rate << "/s";
        }

        // ETA
        if (eta > 0.0) {
            auto e = static_cast<int>(eta);
            std::cerr << " | ETA ";
            if (e >= 3600)
                std::cerr << (e / 3600) << 'h'
                          << std::setw(2) << std::setfill('0')
                          << ((e % 3600) / 60) << 'm';
            else if (e >= 60)
                std::cerr << (e / 60) << 'm'
                          << std::setw(2) << std::setfill('0')
                          << (e % 60) << 's';
            else
                std::cerr << e << 's';
            std::cerr << std::setfill(' ');
        }

        std::cerr << std::flush;
    }

    // Print numbers with SI suffixes so long counts fit on one line.
    // Thresholds: ≥ 1 M → "x.xM", ≥ 100 k → "xxxk", else plain decimal.
    static constexpr std::size_t THRESHOLD_MEGA = 1'000'000;
    static constexpr std::size_t THRESHOLD_KILO = 100'000;

    static std::string fmtCount(std::size_t n) {
        char buf[32];
        if      (n >= THRESHOLD_MEGA) std::snprintf(buf, sizeof(buf), "%.1fM", n / 1e6);
        else if (n >= THRESHOLD_KILO) std::snprintf(buf, sizeof(buf), "%.0fk", n / 1e3);
        else                          std::snprintf(buf, sizeof(buf), "%zu",   n);
        return buf;
    }

    std::size_t total_;
    const char* label_;
    bool        active_;
    std::chrono::steady_clock::time_point t0_;
    std::chrono::steady_clock::time_point lastDraw_{};
};
