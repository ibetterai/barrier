/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "../src/ProximitySignalFilter.h"

#include <gtest/gtest.h>

#include <limits>

TEST(ProximitySignalFilterTests, entryRequiresThreeConsecutiveFilteredSamples)
{
    barrier::ProximitySignalFilter filter;
    filter.addSample(-75, 0);
    filter.addSample(-75, 500);
    EXPECT_FALSE(filter.isNear(500));

    filter.addSample(-75, 1000);
    EXPECT_TRUE(filter.isNear(1000));

    filter.reset();
    filter.addSample(-76, 1500);
    filter.addSample(-76, 2000);
    filter.addSample(-76, 2500);
    EXPECT_FALSE(filter.isNear(2500));
}

TEST(ProximitySignalFilterTests, exactExitBoundaryStartsDepartureGrace)
{
    barrier::ProximitySignalFilter filter;
    filter.addSample(-75, 0);
    filter.addSample(-75, 500);
    filter.addSample(-75, 1000);
    ASSERT_TRUE(filter.isNear(1000));

    // EWMA: 0.25 * -135 + 0.75 * -75 == -90 exactly.
    filter.addSample(-135, 1500);
    double filtered = 0.0;
    ASSERT_TRUE(filter.filteredDbm(filtered));
    EXPECT_DOUBLE_EQ(-90.0, filtered);
    EXPECT_TRUE(filter.isDepartureGrace(1500));
    EXPECT_TRUE(filter.isNear(10999));
    EXPECT_FALSE(filter.isNear(11000));
}

TEST(ProximitySignalFilterTests, departureRequiresContinuousTenSecondAbsence)
{
    barrier::ProximitySignalFilter filter;
    filter.addSample(-75, 0);
    filter.addSample(-75, 500);
    filter.addSample(-75, 1000);
    EXPECT_TRUE(filter.isNear(1000));
    EXPECT_TRUE(filter.isNear(10999));
    EXPECT_FALSE(filter.isNear(11000));
}

TEST(ProximitySignalFilterTests, exposesDepartureGraceWithoutExtendingIt)
{
    barrier::ProximitySignalFilter filter;
    filter.addSample(-75, 0);
    filter.addSample(-75, 500);
    filter.addSample(-75, 1000);
    ASSERT_TRUE(filter.isNear(1000));

    EXPECT_FALSE(filter.isDepartureGrace(2499));
    EXPECT_TRUE(filter.isDepartureGrace(2500));
    EXPECT_FALSE(filter.isDepartureWarning(3999));
    EXPECT_TRUE(filter.isDepartureWarning(4000));
    EXPECT_TRUE(filter.isDepartureGrace(10999));
    EXPECT_FALSE(filter.isNear(11000));
    EXPECT_FALSE(filter.isDepartureGrace(11000));
    EXPECT_FALSE(filter.isDepartureWarning(11000));
}

TEST(ProximitySignalFilterTests, transientWeakSampleDoesNotExposeWarning)
{
    barrier::ProximitySignalFilter filter;
    filter.addSample(-75, 0);
    filter.addSample(-75, 500);
    filter.addSample(-75, 1000);
    ASSERT_TRUE(filter.isNear(1000));

    filter.addSample(-127, 1500);
    filter.addSample(-127, 2000);
    EXPECT_TRUE(filter.isDepartureGrace(2000));
    EXPECT_FALSE(filter.isDepartureWarning(4499));
    filter.addSample(-20, 2500);
    EXPECT_FALSE(filter.isDepartureGrace(2500));
    EXPECT_FALSE(filter.isDepartureWarning(5499));
    EXPECT_TRUE(filter.isNear(12499));
}

TEST(ProximitySignalFilterTests, sustainedWeakSignalExposesWarning)
{
    barrier::ProximitySignalFilter filter;
    filter.addSample(-75, 0);
    filter.addSample(-75, 500);
    filter.addSample(-75, 1000);
    ASSERT_TRUE(filter.isNear(1000));

    filter.addSample(-127, 1500);
    filter.addSample(-127, 2000);
    EXPECT_TRUE(filter.isDepartureGrace(2500));
    EXPECT_FALSE(filter.isDepartureWarning(4499));
    EXPECT_TRUE(filter.isDepartureWarning(4500));
    EXPECT_TRUE(filter.isNear(11499));
}

TEST(ProximitySignalFilterTests, exitThresholdRefreshesPresenceWithoutReentry)
{
    barrier::ProximitySignalFilter filter;
    filter.addSample(-75, 0);
    filter.addSample(-75, 500);
    filter.addSample(-75, 1000);
    ASSERT_TRUE(filter.isNear(1000));

    // EWMA: 0.25 * -131 + 0.75 * -75 == -89, which still refreshes.
    filter.addSample(-131, 9000);
    double filtered = 0.0;
    ASSERT_TRUE(filter.filteredDbm(filtered));
    EXPECT_DOUBLE_EQ(-89.0, filtered);
    EXPECT_TRUE(filter.isNear(10999));
    filter.addSample(-80, 11000);
    EXPECT_TRUE(filter.isNear(20999));
    EXPECT_FALSE(filter.isNear(21000));
}

TEST(ProximitySignalFilterTests, hysteresisBandCannotColdEnter)
{
    barrier::ProximitySignalFilter filter;
    filter.addSample(-80, 0);
    filter.addSample(-80, 500);
    filter.addSample(-80, 1000);
    EXPECT_FALSE(filter.isNear(1000));

    filter.reset();
    filter.addSample(-75, 1500);
    filter.addSample(-75, 2000);
    filter.addSample(-75, 2500);
    ASSERT_TRUE(filter.isNear(2500));
    filter.addSample(-95, 3000);
    EXPECT_FALSE(filter.isDepartureGrace(3000));
    EXPECT_TRUE(filter.isNear(3000));
}

TEST(ProximitySignalFilterTests, departureClearsStrongSignalMemory)
{
    barrier::ProximitySignalFilter filter;
    filter.addSample(-50, 0);
    filter.addSample(-50, 500);
    filter.addSample(-50, 1000);
    ASSERT_TRUE(filter.isNear(1000));
    ASSERT_FALSE(filter.isNear(11000));

    for (qint64 time = 11000; time <= 16000; time += 500) {
        filter.addSample(-90, time);
        EXPECT_FALSE(filter.isNear(time));
    }
    double filtered = 0.0;
    ASSERT_TRUE(filter.filteredDbm(filtered));
    EXPECT_DOUBLE_EQ(-90.0, filtered);
}

TEST(ProximitySignalFilterTests, elapsedTimeComparisonCannotOverflow)
{
    barrier::ProximitySignalFilter filter;
    filter.addSample(-50, std::numeric_limits<qint64>::min());
    filter.addSample(-50, std::numeric_limits<qint64>::min() + 1);
    filter.addSample(-50, -1);
    ASSERT_TRUE(filter.isNear(-1));

    EXPECT_FALSE(filter.isNear(std::numeric_limits<qint64>::max()));
}

TEST(ProximitySignalFilterTests, invalidSamplesAndResetDoNotRetainPresence)
{
    barrier::ProximitySignalFilter filter;
    filter.addSample(-50, 0);
    filter.addSample(-50, 500);
    filter.addSample(127, 750);
    filter.addSample(-50, 1000);
    EXPECT_TRUE(filter.isNear(1000));

    filter.reset();
    EXPECT_FALSE(filter.isNear(1000));
    filter.addSample(127, 2000);
    EXPECT_FALSE(filter.isNear(2000));
}

TEST(ProximitySignalFilterTests, exposesFilteredSignalOnlyAfterValidSample)
{
    barrier::ProximitySignalFilter filter;
    double filtered = 0.0;
    EXPECT_FALSE(filter.filteredDbm(filtered));
    filter.addSample(127, 0);
    EXPECT_FALSE(filter.filteredDbm(filtered));
    filter.addSample(-60, 1);
    ASSERT_TRUE(filter.filteredDbm(filtered));
    EXPECT_DOUBLE_EQ(-60.0, filtered);
    filter.addSample(-40, 2);
    ASSERT_TRUE(filter.filteredDbm(filtered));
    EXPECT_DOUBLE_EQ(-55.0, filtered);
}

TEST(ProximitySignalFilterTests, customPolicyControlsEntryAndDeparture)
{
    const barrier::ProximityThresholdPolicy policy{-60, -80};
    ASSERT_TRUE(policy.isValid());
    barrier::ProximitySignalFilter filter(policy);
    filter.addSample(-61, 0);
    filter.addSample(-61, 500);
    filter.addSample(-61, 1000);
    EXPECT_FALSE(filter.isNear(1000));

    filter.reset();
    filter.addSample(-60, 1500);
    filter.addSample(-60, 2000);
    filter.addSample(-60, 2500);
    ASSERT_TRUE(filter.isNear(2500));

    // EWMA: 0.25 * -140 + 0.75 * -60 == -80 exactly.
    filter.addSample(-140, 3000);
    EXPECT_TRUE(filter.isDepartureGrace(3000));
    EXPECT_FALSE(filter.isNear(12500));
}

TEST(ProximitySignalFilterTests,
     reconfigureClearsSamplesButBoundsAnActiveConnection)
{
    barrier::ProximitySignalFilter filter;
    filter.addSample(-60, 0);
    filter.addSample(-60, 500);
    filter.addSample(-60, 1000);
    ASSERT_TRUE(filter.isNear(1000));

    const barrier::ProximityThresholdPolicy policy{-55, -75};
    ASSERT_TRUE(filter.reconfigure(policy, 1500));
    double filtered = 0.0;
    EXPECT_FALSE(filter.filteredDbm(filtered));
    EXPECT_FALSE(filter.isNear(1500));
    EXPECT_TRUE(filter.isReconfigurationGrace(10999));
    EXPECT_FALSE(filter.isReconfigurationGrace(11000));

    filter.addSample(-90, 2000);
    EXPECT_FALSE(filter.isNear(2000));
    EXPECT_TRUE(filter.isReconfigurationGrace(2000));
    ASSERT_TRUE(filter.filteredDbm(filtered));
    EXPECT_DOUBLE_EQ(-90.0, filtered);
}

TEST(ProximitySignalFilterTests,
     reconfigureKeepsOnlyTheRemainingOldDepartureWindow)
{
    barrier::ProximitySignalFilter filter;
    filter.addSample(-60, 0);
    filter.addSample(-60, 500);
    filter.addSample(-60, 1000);
    ASSERT_TRUE(filter.isNear(1000));

    ASSERT_TRUE(filter.reconfigure(
        barrier::ProximityThresholdPolicy{-55, -75}, 9500));
    EXPECT_TRUE(filter.isReconfigurationGrace(10999));
    EXPECT_FALSE(filter.isReconfigurationGrace(11000));

    // Samples in the new hysteresis band never extend the old deadline.
    filter.addSample(-65, 10000);
    filter.addSample(-65, 10500);
    filter.addSample(-65, 10900);
    EXPECT_FALSE(filter.isNear(10900));
    EXPECT_TRUE(filter.isReconfigurationGrace(10999));
    EXPECT_FALSE(filter.isReconfigurationGrace(11000));
}

TEST(ProximitySignalFilterTests,
     threeFreshConnectSamplesCompleteReconfiguration)
{
    barrier::ProximitySignalFilter filter;
    filter.addSample(-60, 0);
    filter.addSample(-60, 500);
    filter.addSample(-60, 1000);
    ASSERT_TRUE(filter.reconfigure(
        barrier::ProximityThresholdPolicy{-55, -75}, 1500));

    filter.addSample(-55, 2000);
    filter.addSample(-55, 2500);
    EXPECT_TRUE(filter.isReconfigurationGrace(2500));
    EXPECT_FALSE(filter.isNear(2500));
    filter.addSample(-55, 3000);
    EXPECT_TRUE(filter.isNear(3000));
    EXPECT_FALSE(filter.isReconfigurationGrace(3000));
}

TEST(ProximitySignalFilterTests, farReconfigurationCreatesNoGrace)
{
    barrier::ProximitySignalFilter filter;
    filter.addSample(-90, 0);
    ASSERT_TRUE(filter.reconfigure(
        barrier::ProximityThresholdPolicy{-55, -75}, 500));
    EXPECT_FALSE(filter.isNear(500));
    EXPECT_FALSE(filter.isReconfigurationGrace(500));
}

TEST(ProximitySignalFilterTests, policyEnforcesBoundsAndFifteenDbmHysteresis)
{
    EXPECT_TRUE((barrier::ProximityThresholdPolicy{-30, -45}).isValid());
    EXPECT_TRUE((barrier::ProximityThresholdPolicy{-85, -100}).isValid());
    EXPECT_FALSE((barrier::ProximityThresholdPolicy{-30, -44}).isValid());
    EXPECT_FALSE((barrier::ProximityThresholdPolicy{-101, -120}).isValid());
    EXPECT_FALSE((barrier::ProximityThresholdPolicy{-20, -40}).isValid());
    EXPECT_FALSE((barrier::ProximityThresholdPolicy{-80, -75}).isValid());
}
