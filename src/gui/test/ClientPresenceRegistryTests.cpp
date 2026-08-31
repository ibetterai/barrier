/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "../src/ClientPresenceRegistry.h"

#include <gtest/gtest.h>

namespace {

const std::string kClientA = "00112233445566778899aabbccddeeff";
const std::string kClientB = "ffeeddccbbaa99887766554433221100";
const std::string kClientC = "0123456789abcdef0123456789abcdef";

const barrier::ClientPresenceRow* findRow(
    const std::vector<barrier::ClientPresenceRow>& rows,
    const std::string& routingId)
{
    for (const auto& row : rows) {
        if (row.routingId == routingId) {
            return &row;
        }
    }
    return nullptr;
}

} // namespace

TEST(ClientPresenceRegistryTests, interleavedClientsKeepIndependentFiltersAndIds)
{
    barrier::ClientPresenceRegistry registry;
    registry.replaceRoutes({{kClientA, "studio"}, {kClientB, "studio"}});

    ASSERT_TRUE(registry.observe("peripheral-a", kClientA, -40, 100));
    ASSERT_TRUE(registry.observe("peripheral-b", kClientB, -80, 110));
    ASSERT_TRUE(registry.observe("peripheral-a", kClientA, -60, 120));
    ASSERT_TRUE(registry.observe("peripheral-b", kClientB, -40, 130));

    const auto rows = registry.rows(130);
    ASSERT_EQ(2u, rows.size());
    const auto* a = findRow(rows, kClientA);
    const auto* b = findRow(rows, kClientB);
    ASSERT_NE(nullptr, a);
    ASSERT_NE(nullptr, b);
    EXPECT_EQ("studio", a->screenName);
    EXPECT_EQ("studio", b->screenName);
    EXPECT_DOUBLE_EQ(-45.0, a->filteredRssiDbm);
    EXPECT_DOUBLE_EQ(-70.0, b->filteredRssiDbm);
    EXPECT_EQ(120u, a->lastSeenMs);
    EXPECT_EQ(130u, b->lastSeenMs);
}

TEST(ClientPresenceRegistryTests, signalBecomesStaleAtConfiguredBoundary)
{
    barrier::ClientPresenceRegistry registry(1000);
    registry.replaceRoutes({{kClientA, "alpha"}, {kClientB, "beta"}});
    ASSERT_TRUE(registry.observe("peripheral-a", kClientA, -55, 100));

    auto rows = registry.rows(1099);
    ASSERT_EQ(2u, rows.size());
    EXPECT_EQ(barrier::ClientPresenceState::Available,
              findRow(rows, kClientA)->state);
    EXPECT_EQ(barrier::ClientPresenceState::Unavailable,
              findRow(rows, kClientB)->state);

    rows = registry.rows(1100);
    const auto* stale = findRow(rows, kClientA);
    ASSERT_NE(nullptr, stale);
    EXPECT_EQ(barrier::ClientPresenceState::Stale, stale->state);
    EXPECT_TRUE(stale->hasFilteredRssi);
    EXPECT_TRUE(stale->hasLastSeen);
    EXPECT_DOUBLE_EQ(-55.0, stale->filteredRssiDbm);
}

TEST(ClientPresenceRegistryTests, malformedAndUnassociatedIdsNeverCreateNamedRows)
{
    barrier::ClientPresenceRegistry registry;
    registry.replaceRoutes({
        {"ABCDEF0123456789ABCDEF0123456789", "uppercase"},
        {"too-short", "short"},
        {kClientA, "alpha"},
    });

    EXPECT_FALSE(registry.observe("bad-a", "ABCDEF0123456789ABCDEF0123456789", -40, 0));
    EXPECT_FALSE(registry.observe("bad-b", "0123456789abcdef0123456789abcdeg", -40, 0));
    EXPECT_TRUE(registry.observe("unknown", kClientB, -40, 0));

    const auto rows = registry.rows(0);
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(kClientA, rows.front().routingId);
    EXPECT_EQ(barrier::ClientPresenceState::Unavailable, rows.front().state);
}

TEST(ClientPresenceRegistryTests, conflictingAssociationsFailClosedButSameLabelsDoNot)
{
    barrier::ClientPresenceRegistry registry;
    registry.replaceRoutes({
        {kClientA, "alpha"},
        {kClientA, "different-alpha"},
        {kClientB, "same-label"},
        {kClientC, "same-label"},
    });

    ASSERT_FALSE(registry.observe("peripheral-a", kClientA, -40, 0));
    ASSERT_TRUE(registry.observe("peripheral-b", kClientB, -50, 0));
    ASSERT_TRUE(registry.observe("peripheral-c", kClientC, -60, 0));

    const auto rows = registry.rows(0);
    EXPECT_EQ(nullptr, findRow(rows, kClientA));
    EXPECT_NE(nullptr, findRow(rows, kClientB));
    EXPECT_NE(nullptr, findRow(rows, kClientC));
    EXPECT_EQ(2u, rows.size());
}

TEST(ClientPresenceRegistryTests, bleFirstAndRouteFirstConverge)
{
    barrier::ClientPresenceRegistry bleFirst;
    ASSERT_TRUE(bleFirst.observe("peripheral-a", kClientA, -48, 20));
    EXPECT_TRUE(bleFirst.rows(20).empty());
    bleFirst.replaceRoutes({{kClientA, "alpha"}});

    barrier::ClientPresenceRegistry routeFirst;
    routeFirst.replaceRoutes({{kClientA, "alpha"}});
    ASSERT_TRUE(routeFirst.observe("peripheral-a", kClientA, -48, 20));

    const auto bleRows = bleFirst.rows(20);
    const auto routeRows = routeFirst.rows(20);
    ASSERT_EQ(1u, bleRows.size());
    ASSERT_EQ(1u, routeRows.size());
    EXPECT_EQ(bleRows.front().state, routeRows.front().state);
    EXPECT_EQ(bleRows.front().routingId, routeRows.front().routingId);
    EXPECT_EQ(bleRows.front().screenName, routeRows.front().screenName);
    EXPECT_DOUBLE_EQ(bleRows.front().filteredRssiDbm,
                     routeRows.front().filteredRssiDbm);
}

TEST(ClientPresenceRegistryTests, routeRemovalAndRotationRequireFreshObservation)
{
    barrier::ClientPresenceRegistry registry;
    registry.replaceRoutes({{kClientA, "alpha"}});
    ASSERT_TRUE(registry.observe("peripheral-a", kClientA, -45, 10));
    ASSERT_EQ(barrier::ClientPresenceState::Available,
              registry.rows(10).front().state);

    registry.replaceRoutes({{kClientB, "alpha"}});
    auto rows = registry.rows(20);
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(kClientB, rows.front().routingId);
    EXPECT_EQ(barrier::ClientPresenceState::Unavailable, rows.front().state);

    ASSERT_TRUE(registry.observe("peripheral-a", kClientB, -55, 30));
    ASSERT_EQ(barrier::ClientPresenceState::Available,
              registry.rows(30).front().state);

    registry.replaceRoutes({});
    ASSERT_TRUE(registry.observe("peripheral-a", kClientB, -35, 40));
    registry.replaceRoutes({{kClientB, "alpha"}});
    rows = registry.rows(40);
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(barrier::ClientPresenceState::Unavailable, rows.front().state);

    ASSERT_TRUE(registry.observe("peripheral-a", kClientB, -60, 50));
    rows = registry.rows(50);
    EXPECT_EQ(barrier::ClientPresenceState::Available, rows.front().state);
    EXPECT_DOUBLE_EQ(-60.0, rows.front().filteredRssiDbm);
}

TEST(ClientPresenceRegistryTests, peripheralRotationInvalidatesOldRoutingState)
{
    barrier::ClientPresenceRegistry registry;
    registry.replaceRoutes({{kClientA, "alpha"}, {kClientB, "beta"}});
    ASSERT_TRUE(registry.observe("peripheral", kClientA, -40, 0));
    ASSERT_TRUE(registry.observe("peripheral", kClientB, -70, 10));

    const auto rows = registry.rows(10);
    EXPECT_EQ(barrier::ClientPresenceState::Unavailable,
              findRow(rows, kClientA)->state);
    EXPECT_EQ(barrier::ClientPresenceState::Available,
              findRow(rows, kClientB)->state);
    EXPECT_DOUBLE_EQ(-70.0, findRow(rows, kClientB)->filteredRssiDbm);
}

TEST(ClientPresenceRegistryTests, simultaneousRoutingIdCollisionFailsClosed)
{
    barrier::ClientPresenceRegistry registry(1000);
    registry.replaceRoutes({{kClientA, "alpha"}});
    ASSERT_TRUE(registry.observe("peripheral-a", kClientA, -40, 0));
    EXPECT_FALSE(registry.observe("peripheral-b", kClientA, -50, 100));

    auto rows = registry.rows(100);
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(barrier::ClientPresenceState::Unavailable, rows.front().state);
    EXPECT_FALSE(rows.front().hasFilteredRssi);

    EXPECT_FALSE(registry.observe("peripheral-a", kClientA, -40, 1099));
    EXPECT_TRUE(registry.observe("peripheral-a", kClientA, -60, 1100));
    rows = registry.rows(1100);
    EXPECT_EQ(barrier::ClientPresenceState::Available, rows.front().state);
    EXPECT_DOUBLE_EQ(-60.0, rows.front().filteredRssiDbm);
}

TEST(ClientPresenceRegistryTests, olderObservationCannotMoveLastSeenBackward)
{
    barrier::ClientPresenceRegistry registry;
    registry.replaceRoutes({{kClientA, "alpha"}});
    ASSERT_TRUE(registry.observe("peripheral-a", kClientA, -40, 100));
    EXPECT_FALSE(registry.observe("peripheral-a", kClientA, -80, 99));

    const auto rows = registry.rows(100);
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(100u, rows.front().lastSeenMs);
    EXPECT_DOUBLE_EQ(-40.0, rows.front().filteredRssiDbm);
}

TEST(ClientPresenceRegistryTests, olderCompetingPeripheralCannotStealRoutingId)
{
    barrier::ClientPresenceRegistry registry;
    registry.replaceRoutes({{kClientA, "alpha"}});
    ASSERT_TRUE(registry.observe("peripheral-a", kClientA, -40, 100));
    EXPECT_FALSE(registry.observe("peripheral-b", kClientA, -80, 99));

    const auto rows = registry.rows(100);
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(barrier::ClientPresenceState::Available, rows.front().state);
    EXPECT_EQ(100u, rows.front().lastSeenMs);
    EXPECT_DOUBLE_EQ(-40.0, rows.front().filteredRssiDbm);
}
