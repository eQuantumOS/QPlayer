// test_qtimer.cpp — Unit tests for qplayer::QTimer (include/qtimer.hpp).
// Timing tests assert only direction / sign (elapsed > 0, accumulation grows,
// reset zeroes) with loose bounds so they stay stable under load. Absolute
// durations are deliberately not asserted..

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <time.h>

#include "../../../include/qtimer.hpp"

using qplayer::QTimer;

namespace {

// Sleep for the given milliseconds via CLOCK_MONOTONIC-comparable nanosleep.
void sleep_ms(long ms) {
    timespec req{ms / 1000, (ms % 1000) * 1'000'000};
    nanosleep(&req, nullptr);
}

}  // namespace

// start()/end() around a real sleep yields a positive elapsed and a non-empty
// formatted string.
TEST(QTimer, StartEnd_MeasuresPositiveElapsed) {
    // Arrange
    QTimer t;

    // Act
    t.start();
    sleep_ms(2);
    t.end();

    // Assert
    EXPECT_TRUE(t.getElapsedNSec() > 0.0);
    EXPECT_TRUE(t.getElapsedMSec() > 0.0);
    EXPECT_TRUE(t.getTime()[0] != '\0');  // string was formatted
}

// reset() returns the accumulated total and the display string to zero/empty.
TEST(QTimer, Reset_ZeroesElapsedAndString) {
    // Arrange
    QTimer t;
    t.start();
    sleep_ms(1);
    t.end();

    // Act
    t.reset();

    // Assert
    EXPECT_NEAR(t.getElapsedNSec(), 0.0, 1e-12);  // exact 0 after reset
    EXPECT_TRUE(t.getTime()[0] == '\0');
}

// lapStart()/lapStop() accumulate across regions: the running total after two
// laps exceeds the total after one.
TEST(QTimer, Laps_AccumulateAcrossRegions) {
    // Arrange
    QTimer t;
    t.reset();

    // Act
    t.lapStart();
    sleep_ms(2);
    t.lapStop();
    const double after_one = t.getElapsedNSec();

    t.lapStart();
    sleep_ms(2);
    t.lapStop();
    const double after_two = t.getElapsedNSec();

    t.lapFinish();

    // Assert
    EXPECT_TRUE(after_one > 0.0);
    EXPECT_TRUE(after_two > after_one);   // second region added to the total
    EXPECT_TRUE(t.getTime()[0] != '\0');  // finalised string
}
