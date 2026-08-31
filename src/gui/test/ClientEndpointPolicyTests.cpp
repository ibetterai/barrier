/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "../src/ClientEndpointPolicy.h"

#include <gtest/gtest.h>

TEST(ClientEndpointPolicyTests,
     manualAddressWinsAfterPairedProximityServerIsReady)
{
    const barrier::ClientEndpointSelection selection =
        barrier::selectClientEndpoint(
            true, false, QStringLiteral("192.0.2.219"),
            QStringLiteral("[server.local.]:24800"), 24800);
    const QString expected = QStringLiteral("[192.0.2.219]:24800");

    EXPECT_EQ(barrier::ClientEndpointStatus::Ready, selection.status);
    EXPECT_EQ(expected, selection.endpoint);
}

TEST(ClientEndpointPolicyTests,
     proximityStillBlocksManualAddressUntilPairedServerIsReady)
{
    const barrier::ClientEndpointSelection selection =
        barrier::selectClientEndpoint(
            true, false, QStringLiteral("192.0.2.219"), QString(), 24800);

    EXPECT_EQ(barrier::ClientEndpointStatus::WaitingForProximity,
              selection.status);
    EXPECT_TRUE(selection.endpoint.isEmpty());
}

TEST(ClientEndpointPolicyTests, autoConfigUsesDiscoveredEndpoint)
{
    const barrier::ClientEndpointSelection selection =
        barrier::selectClientEndpoint(
            true, true, QStringLiteral("192.0.2.219"),
            QStringLiteral("[server.local.]:24800"), 24800);
    const QString expected = QStringLiteral("[server.local.]:24800");

    EXPECT_EQ(barrier::ClientEndpointStatus::Ready, selection.status);
    EXPECT_EQ(expected, selection.endpoint);
}

TEST(ClientEndpointPolicyTests,
     autoConfigFallsBackToConfiguredAddressWithoutDiscovery)
{
    const barrier::ClientEndpointSelection selection =
        barrier::selectClientEndpoint(
            false, true, QStringLiteral("192.0.2.219"), QString(), 24800);
    const QString expected = QStringLiteral("[192.0.2.219]:24800");

    EXPECT_EQ(barrier::ClientEndpointStatus::Ready, selection.status);
    EXPECT_EQ(expected, selection.endpoint);
}

TEST(ClientEndpointPolicyTests, emptyManualAddressIsRejected)
{
    const barrier::ClientEndpointSelection selection =
        barrier::selectClientEndpoint(
            false, false, QString(), QString(), 24800);

    EXPECT_EQ(barrier::ClientEndpointStatus::MissingServerAddress,
              selection.status);
    EXPECT_TRUE(selection.endpoint.isEmpty());
}
