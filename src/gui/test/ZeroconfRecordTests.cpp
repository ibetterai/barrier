/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "../src/ZeroconfRecord.h"
#include "../src/ClientPresenceAssociation.h"

#include <gtest/gtest.h>

TEST(ZeroconfRecordTests, pairedServerRequiresExactIdentityAndReadiness)
{
    ZeroconfRecord record;
    record.txt[QStringLiteral("proximity-id")] =
        QByteArrayLiteral("00112233445566778899aabbccddeeff");
    record.txt[QStringLiteral("display-ready")] = QByteArrayLiteral("1");

    EXPECT_TRUE(record.matchesProximityServer(
        QStringLiteral("00112233445566778899aabbccddeeff")));
    EXPECT_TRUE(record.isDisplayReady());

    EXPECT_FALSE(record.matchesProximityServer(
        QStringLiteral("00112233445566778899AABBCCDDEEFF")));
    record.txt[QStringLiteral("display-ready")] = QByteArrayLiteral("0");
    EXPECT_FALSE(record.isDisplayReady());
    record.txt[QStringLiteral("display-ready")] = QByteArrayLiteral("true");
    EXPECT_FALSE(record.isDisplayReady());
}

TEST(ZeroconfRecordTests, endpointAndTxtDoNotChangeServiceIdentity)
{
    ZeroconfRecord first(QStringLiteral("Desk"),
                         QStringLiteral("_barrier._tcp"),
                         QStringLiteral("local."));
    first.hostName = QStringLiteral("desk.local.");
    first.port = 24800;
    first.txt[QStringLiteral("display-ready")] = QByteArrayLiteral("1");

    ZeroconfRecord update = first;
    update.hostName = QStringLiteral("desk-new.local.");
    update.port = 24801;
    update.txt[QStringLiteral("display-ready")] = QByteArrayLiteral("0");

    EXPECT_EQ(first, update);
}

TEST(ZeroconfRecordTests,
     barrierEndpointUsesConfiguredDataPortAcrossServicePortChurn)
{
    ZeroconfRecord record(QStringLiteral("Desk"),
                          QStringLiteral("_barrierServerZeroconf._tcp"),
                          QStringLiteral("local."));
    record.hostName = QStringLiteral("desk.local.");
    record.port = 49165;

    EXPECT_EQ(record.barrierEndpoint(24999),
              QStringLiteral("[desk.local.]:24999"));

    record.port = 64686;
    EXPECT_EQ(record.barrierEndpoint(24999),
              QStringLiteral("[desk.local.]:24999"));
}

TEST(ZeroconfRecordTests, invalidAdvertisedIdentityNeverMatches)
{
    ZeroconfRecord record;
    record.txt[QStringLiteral("proximity-id")] = QByteArrayLiteral("short");
    EXPECT_FALSE(record.matchesProximityServer(QStringLiteral("short")));

    record.txt[QStringLiteral("proximity-id")] =
        QByteArrayLiteral("00112233445566778899aabbccddeefg");
    EXPECT_FALSE(record.matchesProximityServer(
        QStringLiteral("00112233445566778899aabbccddeefg")));
}

TEST(ZeroconfRecordTests, pairedClientRequiresExactServerIdentity)
{
    const QString serverId =
        QStringLiteral("00112233445566778899aabbccddeeff");
    ZeroconfRecord record(
        QStringLiteral("Desk client"),
        QStringLiteral("_barrierClientZeroconf._tcp"),
        QStringLiteral("local."));

    EXPECT_FALSE(record.matchesPairedClient(serverId));
    record.txt[QStringLiteral("paired-server-id")] = serverId.toLatin1();
    EXPECT_TRUE(record.matchesPairedClient(serverId));
    EXPECT_FALSE(record.matchesPairedClient(
        QStringLiteral("ffeeddccbbaa99887766554433221100")));

    record.txt[QStringLiteral("paired-server-id")] =
        QByteArrayLiteral("00112233445566778899AABBCCDDEEFF");
    EXPECT_FALSE(record.matchesPairedClient(serverId));
}

TEST(ZeroconfRecordTests, clientRoutingIdRequiresCanonicalOpaqueValue)
{
    ZeroconfRecord record;
    EXPECT_TRUE(record.clientRoutingId().isEmpty());

    const QString id =
        QStringLiteral("00112233445566778899aabbccddeeff");
    record.txt[QStringLiteral("client-routing-id")] = id.toLatin1();
    EXPECT_EQ(id, record.clientRoutingId());

    record.txt[QStringLiteral("client-routing-id")] =
        QByteArrayLiteral("00112233445566778899AABBCCDDEEFF");
    EXPECT_TRUE(record.clientRoutingId().isEmpty());
    record.txt[QStringLiteral("client-routing-id")] =
        QByteArrayLiteral("too-short");
    EXPECT_TRUE(record.clientRoutingId().isEmpty());
}

namespace {

ZeroconfRecord presenceRecord(const QString& screenName,
                              const QString& routingId)
{
    ZeroconfRecord record(
        screenName, QStringLiteral("_barrierClientZeroconf._tcp"),
        QStringLiteral("local."));
    record.hostName = screenName + QStringLiteral(".local.");
    record.port = 49165;
    record.interfaceIndex = 4;
    record.txt.insert(
        QStringLiteral("paired-server-id"),
        QByteArrayLiteral("00112233445566778899aabbccddeeff"));
    record.txt.insert(QStringLiteral("client-routing-id"),
                      routingId.toLatin1());
    return record;
}

} // namespace

TEST(ZeroconfRecordTests,
     presenceAssociationRequiresOneUnambiguousCurrentRoute)
{
    const QString serverId =
        QStringLiteral("00112233445566778899aabbccddeeff");
    const QString routingId =
        QStringLiteral("11112222333344445555666677778888");
    const ZeroconfRecord current = presenceRecord(
        QStringLiteral("client-alpha"), routingId);

    barrier::ClientPresenceSelection selection =
        barrier::selectClientPresenceAssociation(
            {current}, QStringLiteral("client-alpha"), serverId);
    EXPECT_EQ(barrier::ClientPresenceSelectionStatus::Ready,
              selection.status);
    EXPECT_EQ(routingId, selection.routingId);

    selection = barrier::selectClientPresenceAssociation(
        {current, current}, QStringLiteral("client-alpha"), serverId);
    EXPECT_EQ(barrier::ClientPresenceSelectionStatus::Ambiguous,
              selection.status);

    ZeroconfRecord collision = presenceRecord(
        QStringLiteral("client-beta"), routingId);
    selection = barrier::selectClientPresenceAssociation(
        {current, collision}, QStringLiteral("client-alpha"), serverId);
    EXPECT_EQ(barrier::ClientPresenceSelectionStatus::Ambiguous,
              selection.status);

    ZeroconfRecord malformed = current;
    malformed.txt[QStringLiteral("client-routing-id")] =
        QByteArrayLiteral("invalid");
    selection = barrier::selectClientPresenceAssociation(
        {malformed}, QStringLiteral("client-alpha"), serverId);
    EXPECT_EQ(barrier::ClientPresenceSelectionStatus::Unavailable,
              selection.status);
}

TEST(ClientPresenceConnectionEvidenceTests,
     evidenceIsOneShotAndExpiresAtItsDeadline)
{
    barrier::ClientPresenceConnectionEvidence evidence(100, 4);
    evidence.record(QStringLiteral("client-alpha"), 10);

    EXPECT_TRUE(evidence.contains(QStringLiteral("client-alpha"), 109));
    evidence.discard(QStringLiteral("client-alpha"));
    EXPECT_FALSE(evidence.contains(QStringLiteral("client-alpha"), 109));

    evidence.record(QStringLiteral("client-beta"), 20);
    EXPECT_FALSE(evidence.contains(QStringLiteral("client-beta"), 120));
}

TEST(ClientPresenceConnectionEvidenceTests,
     pendingEvidenceIsBoundedAndReconnectRefreshesTheDeadline)
{
    barrier::ClientPresenceConnectionEvidence evidence(100, 2);
    evidence.record(QStringLiteral("oldest"), 0);
    evidence.record(QStringLiteral("kept"), 10);
    evidence.record(QStringLiteral("newest"), 20);

    EXPECT_EQ(2, evidence.size(20));
    EXPECT_FALSE(evidence.contains(QStringLiteral("oldest"), 20));
    EXPECT_TRUE(evidence.contains(QStringLiteral("kept"), 20));
    EXPECT_TRUE(evidence.contains(QStringLiteral("newest"), 20));

    evidence.record(QStringLiteral("kept"), 80);
    EXPECT_TRUE(evidence.contains(QStringLiteral("kept"), 179));
    EXPECT_FALSE(evidence.contains(QStringLiteral("newest"), 120));
}

TEST(ZeroconfRecordTests, pairedWakeRouteMustBeCurrentAndLocal)
{
    const QString id = QStringLiteral("0123456789abcdef0123456789abcdef");
    ZeroconfRecord record(
        QStringLiteral("client"),
        QStringLiteral("_barrierClientZeroconf._tcp."),
        QStringLiteral("local."));
    record.hostName = QStringLiteral("client.local.");
    record.port = 24801;
    record.interfaceIndex = 14;
    record.txt.insert(QStringLiteral("paired-server-id"), id.toLatin1());

    EXPECT_TRUE(record.isCurrentLocalWakeRoute(id));

    record.interfaceIndex = 0;
    EXPECT_FALSE(record.isCurrentLocalWakeRoute(id));
    record.interfaceIndex = 14;
    record.replyDomain = QStringLiteral("example.com.");
    EXPECT_FALSE(record.isCurrentLocalWakeRoute(id));
    record.replyDomain = QStringLiteral("local.");
    record.hostName = QStringLiteral("client.example.com.");
    EXPECT_FALSE(record.isCurrentLocalWakeRoute(id));
    record.hostName = QStringLiteral("client.local.");
    EXPECT_FALSE(record.isCurrentLocalWakeRoute(
        QStringLiteral("fedcba9876543210fedcba9876543210")));

    EXPECT_TRUE(record.matchesCurrentWakeResolution(
        QStringLiteral("client.local."), id.toLatin1()));
    EXPECT_FALSE(record.matchesCurrentWakeResolution(
        QStringLiteral("client.example.com."), id.toLatin1()));
    EXPECT_FALSE(record.matchesCurrentWakeResolution(
        QStringLiteral("impostor.local."), id.toLatin1()));
    EXPECT_FALSE(record.matchesCurrentWakeResolution(
        QStringLiteral("client.local."),
        QByteArrayLiteral("fedcba9876543210fedcba9876543210")));
}
