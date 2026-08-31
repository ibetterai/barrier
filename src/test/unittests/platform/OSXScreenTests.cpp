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
#include <stdexcept>
#include <thread>


TEST(OSXScreenTests, clientBootstrapsGeometryFromOnlineDisplaysWhenActiveSetIsEmpty)
{
    const OSXScreen::DisplayRefreshDecision decision =
        OSXScreen::decideDisplayRefresh(
            OSXScreen::DisplayRefreshRole::SecondaryClient,
            false, true, 0, true, 1);

    EXPECT_EQ(OSXScreen::DisplayRefreshSource::Online, decision.source);
    EXPECT_FALSE(decision.preserveCurrentSnapshot);
    EXPECT_FALSE(decision.retryRequired);
}

TEST(OSXScreenTests, clientBootstrapsGeometryFromOnlineDisplaysWhenActiveQueryFails)
{
    const OSXScreen::DisplayRefreshDecision decision =
        OSXScreen::decideDisplayRefresh(
            OSXScreen::DisplayRefreshRole::SecondaryClient,
            false, false, 0, true, 1);

    EXPECT_EQ(OSXScreen::DisplayRefreshSource::Online, decision.source);
    EXPECT_FALSE(decision.preserveCurrentSnapshot);
    EXPECT_FALSE(decision.retryRequired);
}

TEST(OSXScreenTests, clientPreservesValidGeometryAcrossTransientEmptyDisplaySet)
{
    const OSXScreen::DisplayRefreshDecision decision =
		OSXScreen::decideDisplayRefresh(
            OSXScreen::DisplayRefreshRole::SecondaryClient,
            true, true, 0, false, 0);

    EXPECT_EQ(OSXScreen::DisplayRefreshSource::None, decision.source);
    EXPECT_TRUE(decision.preserveCurrentSnapshot);
    EXPECT_FALSE(decision.retryRequired);
}

TEST(OSXScreenTests, clientPreservesValidGeometryWhenActiveQueryFails)
{
    const OSXScreen::DisplayRefreshDecision decision =
		OSXScreen::decideDisplayRefresh(
            OSXScreen::DisplayRefreshRole::SecondaryClient,
            true, false, 0, false, 0);

    EXPECT_EQ(OSXScreen::DisplayRefreshSource::None, decision.source);
    EXPECT_TRUE(decision.preserveCurrentSnapshot);
    EXPECT_FALSE(decision.retryRequired);
}

TEST(OSXScreenTests, clientPreservesValidGeometryInsteadOfUsingOnlineFallback)
{
    const OSXScreen::DisplayRefreshDecision decision =
		OSXScreen::decideDisplayRefresh(
            OSXScreen::DisplayRefreshRole::SecondaryClient,
            true, true, 0, true, 1);

    EXPECT_EQ(OSXScreen::DisplayRefreshSource::None, decision.source);
    EXPECT_TRUE(decision.preserveCurrentSnapshot);
    EXPECT_FALSE(decision.retryRequired);
}

TEST(OSXScreenTests, publishesActiveDisplaysWhenAvailable)
{
    const OSXScreen::DisplayRefreshDecision decision =
        OSXScreen::decideDisplayRefresh(
            OSXScreen::DisplayRefreshRole::PrimaryServer,
            true, true, 2, true, 3);

    EXPECT_EQ(OSXScreen::DisplayRefreshSource::Active, decision.source);
    EXPECT_FALSE(decision.preserveCurrentSnapshot);
    EXPECT_FALSE(decision.retryRequired);
}

TEST(OSXScreenTests, primaryPublishesEmptyInsteadOfUsingOnlineDisplay)
{
    const OSXScreen::DisplayRefreshDecision decision =
        OSXScreen::decideDisplayRefresh(
            OSXScreen::DisplayRefreshRole::PrimaryServer,
            true, true, 0, true, 1);

    EXPECT_EQ(OSXScreen::DisplayRefreshSource::None, decision.source);
    EXPECT_FALSE(decision.preserveCurrentSnapshot);
    EXPECT_FALSE(decision.retryRequired);
}

TEST(OSXScreenTests, primaryRetriesQueryFailureWithoutPublishingOnlineDisplay)
{
    const OSXScreen::DisplayRefreshDecision decision =
        OSXScreen::decideDisplayRefresh(
            OSXScreen::DisplayRefreshRole::PrimaryServer,
            true, false, 0, true, 1);

    EXPECT_EQ(OSXScreen::DisplayRefreshSource::None, decision.source);
    EXPECT_TRUE(decision.preserveCurrentSnapshot);
    EXPECT_TRUE(decision.retryRequired);
}

TEST(OSXScreenTests, clientLeavesColdStartEmptyWhenNoDisplaySetIsAvailable)
{
    const OSXScreen::DisplayRefreshDecision decision =
        OSXScreen::decideDisplayRefresh(
            OSXScreen::DisplayRefreshRole::SecondaryClient,
            false, true, 0, false, 0);

    EXPECT_EQ(OSXScreen::DisplayRefreshSource::None, decision.source);
    EXPECT_FALSE(decision.preserveCurrentSnapshot);
    EXPECT_FALSE(decision.retryRequired);
}

TEST(OSXScreenTests, beginConfigurationCallbackDoesNotCaptureOldGeometry)
{
    EXPECT_FALSE(OSXScreen::displayReconfigurationCaptureReady(
        kCGDisplayBeginConfigurationFlag | kCGDisplaySetModeFlag));
}

TEST(OSXScreenTests, postConfigurationCallbackCapturesFreshGeometry)
{
    EXPECT_TRUE(OSXScreen::displayReconfigurationCaptureReady(
        kCGDisplaySetModeFlag));
}

TEST(OSXScreenTests, staleRetryGenerationCannotPublishAfterNewCallback)
{
    EXPECT_TRUE(OSXScreen::displayRefreshGenerationIsCurrent(7, 7));
    EXPECT_FALSE(OSXScreen::displayRefreshGenerationIsCurrent(7, 8));
}

TEST(OSXScreenTests, reEnablesEventTapAfterEveryRecoverableDisable)
{
    EXPECT_TRUE(OSXScreen::eventTapDisableRequiresReenable(
        kCGEventTapDisabledByTimeout));
    EXPECT_TRUE(OSXScreen::eventTapDisableRequiresReenable(
        kCGEventTapDisabledByUserInput));
    EXPECT_FALSE(OSXScreen::eventTapDisableRequiresReenable(
        kCGEventMouseMoved));
}

TEST(OSXScreenTests, eventTapSourceAlwaysTargetsCocoaMainRunLoop)
{
    bool workerHasDistinctRunLoop = false;
    bool selectedMainRunLoop = false;
    std::thread worker([&]() {
        CFRunLoopRef currentRunLoop = CFRunLoopGetCurrent();
        CFRunLoopRef mainRunLoop = CFRunLoopGetMain();
        workerHasDistinctRunLoop = currentRunLoop != mainRunLoop;
        selectedMainRunLoop =
            OSXScreen::selectEventTapRunLoop(currentRunLoop, mainRunLoop) ==
                mainRunLoop;
    });
    worker.join();

    ASSERT_TRUE(workerHasDistinctRunLoop);
    EXPECT_TRUE(selectedMainRunLoop);
}


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

TEST(OSXScreenTests, buildsLaptopDisplayTopologies)
{
    const OSXScreen::TopologyDisplayRecord internal = {
        "internal", {0, 0, 1512, 982}, 0.0, true
    };
    const OSXScreen::TopologyDisplayRecord external = {
        "external", {1512, -98, 1920, 1080}, 90.0, false
    };

    const barrier::DisplayTopology internalOnly =
        OSXScreen::topologyFromDisplayRecords({internal});
    ASSERT_EQ(1u, internalOnly.displays.size());
    EXPECT_EQ("internal", internalOnly.displays[0].stableId);
    EXPECT_EQ(0, internalOnly.displays[0].logicalBounds.x);
    EXPECT_EQ(0, internalOnly.displays[0].logicalBounds.y);
    EXPECT_EQ(0, internalOnly.displays[0].rotationDegrees);
    EXPECT_TRUE(internalOnly.displays[0].primary);
    EXPECT_FALSE(internalOnly.profileKey().empty());

    const barrier::DisplayTopology mixed =
        OSXScreen::topologyFromDisplayRecords({internal, external});
    ASSERT_EQ(2u, mixed.displays.size());
    EXPECT_EQ("external", mixed.displays[0].stableId);
    EXPECT_EQ(1512, mixed.displays[0].logicalBounds.x);
    EXPECT_EQ(-98, mixed.displays[0].logicalBounds.y);
    EXPECT_FALSE(mixed.displays[0].primary);
    EXPECT_EQ("internal", mixed.displays[1].stableId);
    EXPECT_TRUE(mixed.displays[1].primary);

    OSXScreen::TopologyDisplayRecord closedLidExternal = external;
    closedLidExternal.logicalBounds = {0, 0, 1920, 1080};
    closedLidExternal.primary = true;
    const barrier::DisplayTopology externalOnly =
        OSXScreen::topologyFromDisplayRecords({closedLidExternal});
    ASSERT_EQ(1u, externalOnly.displays.size());
    EXPECT_EQ("external", externalOnly.displays[0].stableId);
    EXPECT_EQ(1920, externalOnly.displays[0].logicalBounds.w);
    EXPECT_EQ(1080, externalOnly.displays[0].logicalBounds.h);
    EXPECT_TRUE(externalOnly.displays[0].primary);
    EXPECT_NE(internalOnly.profileKey(), externalOnly.profileKey());

    EXPECT_TRUE(OSXScreen::topologyFromDisplayRecords({}).empty());
}

TEST(OSXScreenTests, topologyBuilderIsOrderStableAndRoundsQuarterTurns)
{
    const OSXScreen::TopologyDisplayRecord internal = {
        "internal", {0, 0, 1512, 982}, 0.001, true
    };
    const OSXScreen::TopologyDisplayRecord external = {
        "external", {1512, -98, 1920, 1080}, 89.999, false
    };

    const barrier::DisplayTopology first =
        OSXScreen::topologyFromDisplayRecords({internal, external});
    const barrier::DisplayTopology reversed =
        OSXScreen::topologyFromDisplayRecords({external, internal});
    EXPECT_EQ(first.profileKey(), reversed.profileKey());
    ASSERT_EQ(2u, first.displays.size());
    EXPECT_EQ("external", first.displays[0].stableId);
    EXPECT_EQ(90, first.displays[0].rotationDegrees);
    EXPECT_EQ(first.canonicalIdentity(), reversed.canonicalIdentity());
}

TEST(OSXScreenTests, topologyBuilderRejectsNonQuarterTurnRotation)
{
    const OSXScreen::TopologyDisplayRecord display = {
        "internal", {0, 0, 1512, 982}, 45.0, true
    };
    EXPECT_THROW(OSXScreen::topologyFromDisplayRecords({display}),
                 std::invalid_argument);
}

TEST(OSXScreenTests, topologyBuilderReturnsNormalizedSortedEntries)
{
    const OSXScreen::TopologyDisplayRecord primary = {
        "internal", {100, 200, 1512, 982}, 0.0, true
    };
    const OSXScreen::TopologyDisplayRecord secondary = {
        "external", {-1820, 102, 1920, 1080}, 90.0, false
    };

    const barrier::DisplayTopology topology =
        OSXScreen::topologyFromDisplayRecords({primary, secondary});
    ASSERT_EQ(2u, topology.displays.size());
    EXPECT_EQ("external", topology.displays[0].stableId);
    EXPECT_EQ(-1920, topology.displays[0].logicalBounds.x);
    EXPECT_EQ(-98, topology.displays[0].logicalBounds.y);
    EXPECT_EQ(1920, topology.displays[0].logicalBounds.w);
    EXPECT_EQ(1080, topology.displays[0].logicalBounds.h);
    EXPECT_EQ(90, topology.displays[0].rotationDegrees);
    EXPECT_FALSE(topology.displays[0].primary);
    EXPECT_EQ("internal", topology.displays[1].stableId);
    EXPECT_EQ(0, topology.displays[1].logicalBounds.x);
    EXPECT_EQ(0, topology.displays[1].logicalBounds.y);
    EXPECT_TRUE(topology.displays[1].primary);
}

TEST(OSXScreenTests, topologyBuilderRejectsMissingStableIdentifier)
{
    const OSXScreen::TopologyDisplayRecord display = {
        "", {0, 0, 1512, 982}, 0.0, true
    };
    EXPECT_THROW(OSXScreen::topologyFromDisplayRecords({display}),
                 std::invalid_argument);
}

TEST(OSXScreenTests, normalizesDisplayIdentifiersToLowercase)
{
    EXPECT_EQ("abcdef01-2345-6789-abcd-ef0123456789",
              OSXScreen::normalizeDisplayIdentifier(
                  "ABCDEF01-2345-6789-ABCD-EF0123456789"));
}
