// test_log.cpp — unit tests for qplayer::log.
// Verifies:
//   1. Level gate: set_level(Warn) suppresses debug/info, emits warn/error.
//   2. Thread-safety: N threads calling log::warn concurrently produce
//      N complete uninterleaved lines in std::clog.
//
// std::thread waiver: performance-rules forbids direct std::thread usage
// runtime code (OpenMP only). Test bodies are exempt — exercising log's mutex
// from N concurrent native threads is the only way to validate 's
// thread-safety claim, which explicitly requires. The
// waiver lapses if the requirement is ever moved off std::thread.
//
// clog capture strategy: redirect std::clog's stream buffer to a
// std::ostringstream for the duration of each test, then restore.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <atomic>
#include <iostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

#include "../../../include/log.hpp"

using qplayer::log::Level;

// ── clog capture helper ───────────────────────────────────────────────────────

namespace {

class ClogCapture {
public:
    ClogCapture() {
        old_buf_ = std::clog.rdbuf(buf_.rdbuf());
    }
    ~ClogCapture() {
        std::clog.rdbuf(old_buf_);
    }

    // Returns all text written to std::clog since this capture was created.
    std::string captured() const { return buf_.str(); }

    // Count newline-terminated lines in captured output.
    int line_count() const {
        const std::string s = buf_.str();
        int n = 0;
        for (char c : s) {
            if (c == '\n') ++n;
        }
        return n;
    }

private:
    std::ostringstream buf_;
    std::streambuf*    old_buf_;
};

// Restore the log level at scope exit (RAII wrapper so tests don't leak).
class LevelGuard {
public:
    explicit LevelGuard(Level lvl)
        : saved_(qplayer::log::current_level()) {
        qplayer::log::set_level(lvl);
    }
    ~LevelGuard() {
        qplayer::log::set_level(saved_);
    }
private:
    Level saved_;
};

}  // namespace

// ── Level gate tests ──────────────────────────────────────────────────────────

TEST(Log, SetLevel_Warn_DebugIsDropped) {
    // Arrange
    LevelGuard guard(Level::Warn);
    ClogCapture cap;

    // Act
    qplayer::log::debug("should be suppressed");

    // Assert: no output (level gate drops Debug when threshold is Warn).
    EXPECT_EQ(cap.line_count(), 0);
}

TEST(Log, SetLevel_Warn_InfoIsDropped) {
    // Arrange
    LevelGuard guard(Level::Warn);
    ClogCapture cap;

    // Act
    qplayer::log::info("should be suppressed");

    // Assert: no output.
    EXPECT_EQ(cap.line_count(), 0);
}

TEST(Log, SetLevel_Warn_WarnIsEmitted) {
    // Arrange
    LevelGuard guard(Level::Warn);
    ClogCapture cap;

    // Act
    qplayer::log::warn("warn message");

    // Assert: exactly one line.
    EXPECT_EQ(cap.line_count(), 1);
}

TEST(Log, SetLevel_Warn_ErrorIsEmitted) {
    // Arrange
    LevelGuard guard(Level::Warn);
    ClogCapture cap;

    // Act
    qplayer::log::error("error message");

    // Assert: exactly one line.
    EXPECT_EQ(cap.line_count(), 1);
}

TEST(Log, SetLevel_Off_NothingIsEmitted) {
    // Arrange
    LevelGuard guard(Level::Off);
    ClogCapture cap;

    // Act
    qplayer::log::debug("d");
    qplayer::log::info("i");
    qplayer::log::warn("w");
    qplayer::log::error("e");

    // Assert: Level::Off suppresses everything.
    EXPECT_EQ(cap.line_count(), 0);
}

TEST(Log, SetLevel_Debug_AllLevelsEmitted) {
    // Arrange
    LevelGuard guard(Level::Debug);
    ClogCapture cap;

    // Act
    qplayer::log::debug("d");
    qplayer::log::info("i");
    qplayer::log::warn("w");
    qplayer::log::error("e");

    // Assert: 4 lines total.
    EXPECT_EQ(cap.line_count(), 4);
}

TEST(Log, MessageFormat_ContainsLevelTag) {
    // Arrange
    LevelGuard guard(Level::Warn);
    ClogCapture cap;

    // Act
    qplayer::log::warn("hello world");

    // Assert: output contains the expected prefix.
    const std::string out = cap.captured();
    // Format: "[QPlayer/WARN ] hello world\n"
    const bool has_prefix = (out.find("[QPlayer/WARN") != std::string::npos);
    EXPECT_TRUE(has_prefix);
    const bool has_msg = (out.find("hello world") != std::string::npos);
    EXPECT_TRUE(has_msg);
}

TEST(Log, SetLevel_Current_RoundTrip) {
    // Arrange
    LevelGuard guard(Level::Info);

    // Act / Assert: current_level() reflects what set_level() stored.
    // Cast to int because Level has no operator<< (EXPECT_EQ requires it for
    // the failure message; EXPECT_TRUE avoids the stream operator requirement).
    EXPECT_TRUE(qplayer::log::current_level() == Level::Info);
}

// ── Thread-safety test ────────────────────────────────────────────────────────
//
// N threads each write one log::warn message.  After joining, we assert:
//   a) Exactly N newline-terminated lines appeared in clog.
//   b) No line contains content from two different messages (no interleave).
//
// The interleave check: every emitted line must start with "[QPlayer/" and
// end with '\n' as its last character.  If two threads' writes interleave,
// at least one line will violate this property.

TEST(Log, ThreadSafe_NConcurrentWarn_NoLineInterleave) {
    // Arrange
    LevelGuard guard(Level::Warn);
    ClogCapture cap;

    constexpr int kThreads = 8;
    std::atomic<int> barrier{0};

    // Act: all threads start together then each emits one warn message.
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([i, &barrier]() {
            ++barrier;
            // Spin-wait until all threads are ready (maximise concurrency).
            while (barrier.load(std::memory_order_acquire) < kThreads) {}
            qplayer::log::warn("thread " + std::to_string(i) + " reporting");
        });
    }
    for (auto& t : threads) t.join();

    // Assert a: exactly kThreads lines.
    EXPECT_EQ(cap.line_count(), kThreads);

    // Assert b: no interleaved lines — every line starts with "[QPlayer/"
    // and contains exactly one newline (its own terminator).
    const std::string all = cap.captured();
    std::size_t pos = 0;
    int good_lines = 0;
    while (pos < all.size()) {
        std::size_t nl = all.find('\n', pos);
        if (nl == std::string::npos) break;
        const std::string line = all.substr(pos, nl - pos);
        // Each line must begin with the "[QPlayer/" prefix.
        const bool well_formed = (line.rfind("[QPlayer/", 0) == 0);
        EXPECT_TRUE(well_formed);
        ++good_lines;
        pos = nl + 1;
    }
    EXPECT_EQ(good_lines, kThreads);
}
