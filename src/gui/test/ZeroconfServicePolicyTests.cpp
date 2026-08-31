/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "../src/ZeroconfService.h"

#include <gtest/gtest.h>

namespace {

ZeroconfRecord serverRecord(const QString& serviceName,
                             const QString& hostName,
                             quint16 port,
                             quint32 interfaceIndex,
                             const QByteArray& txtValue)
{
    ZeroconfRecord record(
        serviceName, QStringLiteral("_barrierServerZeroconf._tcp"),
        QStringLiteral("local."));
    record.hostName = hostName;
    record.port = port;
    record.interfaceIndex = interfaceIndex;
    record.txt.insert(QStringLiteral("display-ready"), txtValue);
    return record;
}

} // namespace

TEST(ZeroconfServicePolicyTests,
     proximityClientKeepsDiscoveryActiveWhenAutoConfigIsOff)
{
    const ZeroconfService::Requirements requirements =
        ZeroconfService::requirements(false, false, true);

    EXPECT_TRUE(requirements.active);
    EXPECT_TRUE(requirements.publishClientService);
}

TEST(ZeroconfServicePolicyTests,
     autoConfigClientStillPublishesWhenProximityIsEnabled)
{
    const ZeroconfService::Requirements requirements =
        ZeroconfService::requirements(true, false, true);

    EXPECT_TRUE(requirements.active);
    EXPECT_TRUE(requirements.publishClientService);
}

TEST(ZeroconfServicePolicyTests, legacyActivationRulesRemainIntact)
{
    ZeroconfService::Requirements requirements =
        ZeroconfService::requirements(true, false, false);
    EXPECT_TRUE(requirements.active);
    EXPECT_TRUE(requirements.publishClientService);

    requirements = ZeroconfService::requirements(false, true, false);
    EXPECT_TRUE(requirements.active);
    EXPECT_FALSE(requirements.publishClientService);

    requirements = ZeroconfService::requirements(false, false, false);
    EXPECT_FALSE(requirements.active);
    EXPECT_FALSE(requirements.publishClientService);
}

TEST(ZeroconfServicePolicyTests,
     signalSharingPublishesClientRouteWithoutConnectionGating)
{
    const ZeroconfService::Requirements requirements =
        ZeroconfService::requirements(false, false, false, true);

    EXPECT_TRUE(requirements.active);
    EXPECT_TRUE(requirements.publishClientService);
}

TEST(ZeroconfServicePolicyTests,
     endpointLogPolicySuppressesIdenticalFullSnapshots)
{
    barrier::ZeroconfEndpointLogPolicy policy;
    const ZeroconfRecord record = serverRecord(
        QStringLiteral("Desk"), QStringLiteral("desk.local."), 49165, 4,
        QByteArrayLiteral("1"));

    const QList<ZeroconfRecord> first =
        policy.newOrChangedEndpoints({record});
    ASSERT_EQ(1, first.size());
    EXPECT_EQ(record.hostName, first.first().hostName);
    EXPECT_EQ(record.port, first.first().port);

    EXPECT_TRUE(policy.newOrChangedEndpoints({record}).isEmpty());
}

TEST(ZeroconfServicePolicyTests,
     endpointLogPolicyIgnoresTxtAndInterfaceOnlyUpdates)
{
    barrier::ZeroconfEndpointLogPolicy policy;
    const ZeroconfRecord original = serverRecord(
        QStringLiteral("Desk"), QStringLiteral("desk.local."), 49165, 4,
        QByteArrayLiteral("0"));
    ASSERT_EQ(1, policy.newOrChangedEndpoints({original}).size());

    ZeroconfRecord txtChanged = original;
    txtChanged.txt.insert(
        QStringLiteral("display-ready"), QByteArrayLiteral("1"));
    EXPECT_TRUE(policy.newOrChangedEndpoints({txtChanged}).isEmpty());

    ZeroconfRecord interfaceChanged = txtChanged;
    interfaceChanged.interfaceIndex = 9;
    EXPECT_TRUE(policy.newOrChangedEndpoints({interfaceChanged}).isEmpty());
}

TEST(ZeroconfServicePolicyTests,
     endpointLogPolicyReportsChangedAndReappearedEndpoints)
{
    barrier::ZeroconfEndpointLogPolicy policy;
    const ZeroconfRecord original = serverRecord(
        QStringLiteral("Desk"), QStringLiteral("desk.local."), 49165, 4,
        QByteArrayLiteral("1"));
    ASSERT_EQ(1, policy.newOrChangedEndpoints({original}).size());

    ZeroconfRecord hostChanged = original;
    hostChanged.hostName = QStringLiteral("desk-new.local.");
    QList<ZeroconfRecord> changed =
        policy.newOrChangedEndpoints({hostChanged});
    ASSERT_EQ(1, changed.size());
    EXPECT_EQ(hostChanged.hostName, changed.first().hostName);

    ZeroconfRecord portChanged = hostChanged;
    portChanged.port = 49166;
    changed = policy.newOrChangedEndpoints({portChanged});
    ASSERT_EQ(1, changed.size());
    EXPECT_EQ(portChanged.port, changed.first().port);

    EXPECT_TRUE(policy.newOrChangedEndpoints({}).isEmpty());
    changed = policy.newOrChangedEndpoints({portChanged});
    ASSERT_EQ(1, changed.size());
    EXPECT_EQ(portChanged.hostName, changed.first().hostName);
    EXPECT_EQ(portChanged.port, changed.first().port);
}

TEST(ZeroconfServicePolicyTests, endpointLogPolicyTracksServicesIndependently)
{
    barrier::ZeroconfEndpointLogPolicy policy;
    const ZeroconfRecord desk = serverRecord(
        QStringLiteral("Desk"), QStringLiteral("desk.local."), 49165, 4,
        QByteArrayLiteral("1"));
    const ZeroconfRecord studio = serverRecord(
        QStringLiteral("Studio"), QStringLiteral("studio.local."), 49165,
        5, QByteArrayLiteral("1"));

    ASSERT_EQ(2, policy.newOrChangedEndpoints({desk, studio}).size());

    ZeroconfRecord movedStudio = studio;
    movedStudio.port = 49166;
    const QList<ZeroconfRecord> changed =
        policy.newOrChangedEndpoints({desk, movedStudio});
    ASSERT_EQ(1, changed.size());
    EXPECT_EQ(QLatin1String("Studio"), changed.first().serviceName);

    EXPECT_TRUE(policy.newOrChangedEndpoints({desk}).isEmpty());
    const QList<ZeroconfRecord> reappeared =
        policy.newOrChangedEndpoints({desk, movedStudio});
    ASSERT_EQ(1, reappeared.size());
    EXPECT_EQ(QLatin1String("Studio"), reappeared.first().serviceName);
}
