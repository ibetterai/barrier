/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2014-2016 Symless Ltd.
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "platform/OSXScreen.h"
#include "barrier/DisplayGeometry.h"

#include "test/global/gtest.h"

TEST(OSXScreenTests, selectsDisplayContainingEnterCoordinate)
{
    const std::vector<ScreenRect> displays = {
        {0, 0, 1920, 1080},
        {1920, -840, 1080, 1920}
    };
    EXPECT_EQ(OSXScreen::displayIndexAt(displays, 1920, -400), 1);
}

TEST(OSXScreenTests, resolvesSharedEdgeToSingleDisplay)
{
    // half-open bounds: the x=1920 edge belongs to the right display only
    const std::vector<ScreenRect> displays = {
        {0, 0, 1920, 1080},
        {1920, -840, 1080, 1920}
    };
    EXPECT_EQ(OSXScreen::displayIndexAt(displays, 1919, 0), 0);
    EXPECT_EQ(OSXScreen::displayIndexAt(displays, 1920, 0), 1);
    EXPECT_EQ(OSXScreen::displayIndexAt(displays, 1920, -400), 1);
}

TEST(OSXScreenTests, returnsNoDisplayForCoordinateOutsideAllRects)
{
    const std::vector<ScreenRect> displays = {
        {0, 0, 1920, 1080},
        {1920, -840, 1080, 1920}
    };
    EXPECT_EQ(OSXScreen::displayIndexAt(displays, -1, 0), -1);
    EXPECT_EQ(OSXScreen::displayIndexAt(displays, 5000, 5000), -1);
}

TEST(OSXScreenTests, doesNotRequestWakeForAwakeDisplay)
{
    EXPECT_FALSE(OSXScreen::shouldRequestWake(false, 1000, 1010));
    // even when a hold would otherwise be expired
    EXPECT_FALSE(OSXScreen::shouldRequestWake(false, 1000, 0));
}

TEST(OSXScreenTests, requestsWakeForAsleepDisplayWhenNoHoldActive)
{
    // no hold running (expiry in the past or never set)
    EXPECT_TRUE(OSXScreen::shouldRequestWake(true, 1000, 0));
    EXPECT_TRUE(OSXScreen::shouldRequestWake(true, 1000, 999));
}

TEST(OSXScreenTests, refreshesActiveHoldInsteadOfStacking)
{
    // an active hold (expiry in the future) absorbs the entry as a refresh
    EXPECT_FALSE(OSXScreen::shouldRequestWake(true, 1000, 1010));
    EXPECT_FALSE(OSXScreen::shouldRequestWake(true, 1000, 1001));
    // at the expiry boundary the hold has ended, so a new one starts
    EXPECT_TRUE(OSXScreen::shouldRequestWake(true, 1000, 1000));
}

TEST(OSXScreenTests, naturalScrollDirectionSignMatchesPreference)
{
    EXPECT_EQ(-1, OSXScreen::naturalScrollDirectionSign(true));
    EXPECT_EQ(1, OSXScreen::naturalScrollDirectionSign(false));
}

TEST(OSXScreenTests, normalizesDeltasForSamePreferenceToIdentity)
{
    // both hosts natural: capture and injection conversions cancel
    SInt32 x = 120;
    SInt32 y = 360;
    OSXScreen::normalizeScrollDeltas(true, x, y);
    OSXScreen::normalizeScrollDeltas(true, x, y);
    EXPECT_EQ(120, x);
    EXPECT_EQ(360, y);

    // both hosts classic: nothing to convert on either side
    x = 120;
    y = 360;
    OSXScreen::normalizeScrollDeltas(false, x, y);
    OSXScreen::normalizeScrollDeltas(false, x, y);
    EXPECT_EQ(120, x);
    EXPECT_EQ(360, y);
}

TEST(OSXScreenTests, invertsBothAxesForDifferingPreferences)
{
    // natural source, classic target: every axis flips exactly once
    SInt32 x = 120;
    SInt32 y = 360;
    OSXScreen::normalizeScrollDeltas(true, x, y);
    OSXScreen::normalizeScrollDeltas(false, x, y);
    EXPECT_EQ(-120, x);
    EXPECT_EQ(-360, y);

    // classic source, natural target: the same inversion
    x = 120;
    y = 360;
    OSXScreen::normalizeScrollDeltas(false, x, y);
    OSXScreen::normalizeScrollDeltas(true, x, y);
    EXPECT_EQ(-120, x);
    EXPECT_EQ(-360, y);
}

TEST(OSXScreenTests, invertsHorizontalOnlyDeltaWhenPreferencesDiffer)
{
    SInt32 x = 120;
    SInt32 y = 0;
    OSXScreen::normalizeScrollDeltas(true, x, y);
    OSXScreen::normalizeScrollDeltas(false, x, y);
    EXPECT_EQ(-120, x);
    EXPECT_EQ(0, y);
}

TEST(OSXScreenTests, invertsVerticalOnlyDeltaWhenPreferencesDiffer)
{
    SInt32 x = 0;
    SInt32 y = 360;
    OSXScreen::normalizeScrollDeltas(true, x, y);
    OSXScreen::normalizeScrollDeltas(false, x, y);
    EXPECT_EQ(0, x);
    EXPECT_EQ(-360, y);
}

TEST(OSXScreenTests, doesNotForwardWheelWithoutRemoteOwnership)
{
    // cursor on the primary itself: wheel events stay local
    EXPECT_FALSE(OSXScreen::shouldForwardRemoteWheel(true, true));
    // a secondary screen never captures input
    EXPECT_FALSE(OSXScreen::shouldForwardRemoteWheel(false, false));
}

TEST(OSXScreenTests, forwardsWheelWhileOwningRemoteTarget)
{
    // cursor off the primary and on a client screen
    EXPECT_TRUE(OSXScreen::shouldForwardRemoteWheel(true, false));
}

TEST(OSXScreenTests, disablesScrollDiagnosticsForEmptyAndExplicitFalseValues)
{
    EXPECT_FALSE(OSXScreen::shouldEnableScrollDiagnostics(NULL));
    EXPECT_FALSE(OSXScreen::shouldEnableScrollDiagnostics(""));
    EXPECT_FALSE(OSXScreen::shouldEnableScrollDiagnostics("0"));
    EXPECT_FALSE(OSXScreen::shouldEnableScrollDiagnostics("false"));
    EXPECT_FALSE(OSXScreen::shouldEnableScrollDiagnostics("FALSE"));
    EXPECT_FALSE(OSXScreen::shouldEnableScrollDiagnostics("off"));
    EXPECT_FALSE(OSXScreen::shouldEnableScrollDiagnostics("OFF"));
    EXPECT_FALSE(OSXScreen::shouldEnableScrollDiagnostics("no"));
    EXPECT_FALSE(OSXScreen::shouldEnableScrollDiagnostics("NO"));
}

TEST(OSXScreenTests, enablesScrollDiagnosticsForTruthyValues)
{
    EXPECT_TRUE(OSXScreen::shouldEnableScrollDiagnostics("1"));
    EXPECT_TRUE(OSXScreen::shouldEnableScrollDiagnostics("true"));
    EXPECT_TRUE(OSXScreen::shouldEnableScrollDiagnostics("yes"));
    EXPECT_TRUE(OSXScreen::shouldEnableScrollDiagnostics("anything"));
}

TEST(OSXScreenTests, spacesSwipeFallbackUsesSameOptInParsing)
{
    EXPECT_FALSE(OSXScreen::shouldEnableSpacesSwipeFallback(NULL));
    EXPECT_FALSE(OSXScreen::shouldEnableSpacesSwipeFallback(""));
    EXPECT_FALSE(OSXScreen::shouldEnableSpacesSwipeFallback("0"));
    EXPECT_FALSE(OSXScreen::shouldEnableSpacesSwipeFallback("false"));
    EXPECT_TRUE(OSXScreen::shouldEnableSpacesSwipeFallback("1"));
    EXPECT_TRUE(OSXScreen::shouldEnableSpacesSwipeFallback("true"));
}

TEST(OSXScreenTests, syntheticSpacesSwipeWheelRequiresSentinel)
{
    EXPECT_TRUE(OSXScreen::isSyntheticSpacesSwipeWheel(30000, -31415));
    EXPECT_TRUE(OSXScreen::isSyntheticSpacesSwipeWheel(-30000, -31415));
    EXPECT_FALSE(OSXScreen::isSyntheticSpacesSwipeWheel(30000, 0));
    EXPECT_FALSE(OSXScreen::isSyntheticSpacesSwipeWheel(240, -31415));
}

TEST(OSXScreenTests, spacesSwipeSourceAccumulatesRawMagicMouseSignal)
{
    OSXScreen::SpacesSwipeSourceState state;
    SInt32 direction = 0;

    EXPECT_FALSE(OSXScreen::updateSpacesSwipeSourceState(
        state, 0.20, 1000, 0.55, 180, 180, direction));
    EXPECT_EQ(0, direction);

    EXPECT_TRUE(OSXScreen::updateSpacesSwipeSourceState(
        state, 0.40, 1030, 0.55, 180, 180, direction));
    EXPECT_EQ(1, direction);
}

TEST(OSXScreenTests, spacesSwipeSourceResetsAcrossWindow)
{
    OSXScreen::SpacesSwipeSourceState state;
    SInt32 direction = 0;

    EXPECT_FALSE(OSXScreen::updateSpacesSwipeSourceState(
        state, 0.30, 1000, 0.55, 180, 180, direction));
    EXPECT_FALSE(OSXScreen::updateSpacesSwipeSourceState(
        state, 0.30, 1201, 0.55, 180, 180, direction));
    EXPECT_EQ(0, direction);
}

TEST(OSXScreenTests, spacesSwipeSourceAllowsFastRepeatedSwipes)
{
    OSXScreen::SpacesSwipeSourceState state;
    SInt32 direction = 0;

    EXPECT_TRUE(OSXScreen::updateSpacesSwipeSourceState(
        state, 0.60, 1000, 0.55, 180, 180, direction));
    EXPECT_EQ(1, direction);

    EXPECT_FALSE(OSXScreen::updateSpacesSwipeSourceState(
        state, 0.60, 1100, 0.55, 180, 180, direction));
    EXPECT_TRUE(OSXScreen::updateSpacesSwipeSourceState(
        state, 0.60, 1181, 0.55, 180, 180, direction));
    EXPECT_EQ(1, direction);
}

TEST(OSXScreenTests, spacesSwipeSourceReportsNegativeDirection)
{
    OSXScreen::SpacesSwipeSourceState state;
    SInt32 direction = 0;

    EXPECT_TRUE(OSXScreen::updateSpacesSwipeSourceState(
        state, -0.60, 1000, 0.55, 180, 180, direction));
    EXPECT_EQ(-1, direction);
}

TEST(OSXScreenTests, displayLabelMatchesIndexResolution)
{
    // the log formatter must resolve the display under a point with the
    // same half-open bounds as the platform resolver, so a transition
    // log never names a different display than the one routing used
    const std::vector<ScreenRect> displays = {
        {0, 0, 1920, 1080},
        {1920, -840, 1080, 1920}
    };
    const std::vector<std::string> names = {"LU", "VG"};

    EXPECT_EQ(0, OSXScreen::displayIndexAt(displays, 1919, 0));
    EXPECT_EQ("LU", barrier::displayLabelAt(displays, names, 1919, 0));

    EXPECT_EQ(1, OSXScreen::displayIndexAt(displays, 1920, -400));
    EXPECT_EQ("VG", barrier::displayLabelAt(displays, names, 1920, -400));

    EXPECT_EQ(-1, OSXScreen::displayIndexAt(displays, 5000, 5000));
    EXPECT_EQ("", barrier::displayLabelAt(displays, names, 5000, 5000));
}
