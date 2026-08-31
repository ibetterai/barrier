/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "../src/ClientIdentityResolverQueue.h"

#include <gtest/gtest.h>

#ifdef Q_OS_MACOS
#include "../src/MacProximityController.h"

#include <QCoreApplication>
#include <QPointer>
#endif

namespace {

QUuid uuid(const char* value)
{
    return QUuid(QString::fromLatin1(value));
}

const QUuid kFirst = uuid("{00000000-0000-0000-0000-000000000001}");
const QUuid kSecond = uuid("{00000000-0000-0000-0000-000000000002}");
const QUuid kThird = uuid("{00000000-0000-0000-0000-000000000003}");

} // namespace

TEST(ClientIdentityResolverQueueTests, BoundsAndDeduplicatesInFairFifoOrder)
{
    barrier::ClientIdentityResolverQueue queue(2, 100, 1000);
    queue.startSession();

    EXPECT_TRUE(queue.enqueue(kFirst, 0));
    EXPECT_FALSE(queue.enqueue(kFirst, 0));
    EXPECT_TRUE(queue.enqueue(kSecond, 0));
    EXPECT_FALSE(queue.enqueue(kThird, 0));

    const auto first = queue.takeNext(10);
    EXPECT_EQ(first.peripheralId, kFirst);
    EXPECT_TRUE(queue.completeFailure(first, 20));

    const auto second = queue.takeNext(20);
    EXPECT_EQ(second.peripheralId, kSecond);
    EXPECT_TRUE(queue.completeSuccess(second));
    EXPECT_FALSE(queue.takeNext(20).isValid());

    EXPECT_FALSE(queue.enqueue(kFirst, 1019));
    EXPECT_TRUE(queue.enqueue(kFirst, 1020));
    EXPECT_EQ(queue.takeNext(1020).peripheralId, kFirst);
}

TEST(ClientIdentityResolverQueueTests, TimeoutIsTerminalAndStartsCooldown)
{
    barrier::ClientIdentityResolverQueue queue(4, 100, 500);
    queue.startSession();
    ASSERT_TRUE(queue.enqueue(kFirst, 0));
    const auto request = queue.takeNext(10);

    EXPECT_EQ(queue.activeDeadlineMs(), 110);
    EXPECT_FALSE(queue.expireActive(109));
    barrier::ClientIdentityResolveRequest expired;
    EXPECT_TRUE(queue.expireActive(110, &expired));
    EXPECT_EQ(expired.peripheralId, kFirst);
    EXPECT_FALSE(queue.activeRequest().isValid());
    EXPECT_FALSE(queue.enqueue(kFirst, 609));
    EXPECT_TRUE(queue.enqueue(kFirst, 610));
}

TEST(ClientIdentityResolverQueueTests, GenerationRejectsEveryLateCompletion)
{
    barrier::ClientIdentityResolverQueue queue;
    const quint64 firstGeneration = queue.startSession();
    ASSERT_TRUE(queue.enqueue(kFirst, 0));
    const auto stale = queue.takeNext(0);
    ASSERT_EQ(stale.generation, firstGeneration);

    const quint64 secondGeneration = queue.resetSession();
    EXPECT_NE(secondGeneration, firstGeneration);
    EXPECT_FALSE(queue.completeSuccess(stale));
    EXPECT_FALSE(queue.completeFailure(stale, 1));

    ASSERT_TRUE(queue.enqueue(kFirst, 1));
    const auto current = queue.takeNext(1);
    EXPECT_EQ(current.generation, secondGeneration);
    EXPECT_TRUE(queue.completeSuccess(current));
}

TEST(ClientIdentityResolverQueueTests, StopCancelsActiveAndPendingWork)
{
    barrier::ClientIdentityResolverQueue queue;
    queue.startSession();
    ASSERT_TRUE(queue.enqueue(kFirst, 0));
    ASSERT_TRUE(queue.enqueue(kSecond, 0));
    const auto active = queue.takeNext(0);

    const quint64 stoppedGeneration = queue.stopSession();
    EXPECT_FALSE(queue.isRunning());
    EXPECT_FALSE(queue.activeRequest().isValid());
    EXPECT_EQ(queue.pendingCount(), 0);
    EXPECT_FALSE(queue.completeSuccess(active));
    EXPECT_FALSE(queue.enqueue(kThird, 1));

    EXPECT_NE(queue.startSession(), stoppedGeneration);
    EXPECT_TRUE(queue.enqueue(kFirst, 1));
}

TEST(ClientIdentityResolverQueueTests, SuccessfulIdentityIsReadOncePerSession)
{
    barrier::ClientIdentityResolverQueue queue;
    queue.startSession();
    ASSERT_TRUE(queue.enqueue(kFirst, 0));
    const auto request = queue.takeNext(0);
    ASSERT_TRUE(queue.completeSuccess(request));

    EXPECT_FALSE(queue.enqueue(kFirst, 100000));
    queue.resetSession();
    EXPECT_TRUE(queue.enqueue(kFirst, 100001));
}

TEST(ClientIdentityResolverQueueTests, SessionAttemptBudgetBoundsUniqueChurn)
{
    barrier::ClientIdentityResolverQueue queue(4, 100, 0, 2);
    queue.startSession();
    ASSERT_TRUE(queue.enqueue(kFirst, 0));
    ASSERT_TRUE(queue.enqueue(kSecond, 0));
    ASSERT_TRUE(queue.enqueue(kThird, 0));

    const auto first = queue.takeNext(0);
    ASSERT_TRUE(queue.completeFailure(first, 1));
    const auto second = queue.takeNext(1);
    ASSERT_TRUE(queue.completeFailure(second, 2));

    EXPECT_EQ(queue.attemptCount(), 2);
    EXPECT_FALSE(queue.takeNext(2).isValid());
    EXPECT_EQ(queue.pendingCount(), 0);
    EXPECT_FALSE(queue.enqueue(kFirst, 3));

    queue.resetSession();
    EXPECT_EQ(queue.attemptCount(), 0);
    EXPECT_TRUE(queue.enqueue(kFirst, 3));
}

TEST(ClientIdentityResolverQueueTests,
     TracksOnlyBoundedPendingActiveAndResolvedPeripherals)
{
    barrier::ClientIdentityResolverQueue queue(2, 100, 500, 2);
    queue.startSession();

    ASSERT_TRUE(queue.enqueue(kFirst, 0));
    ASSERT_TRUE(queue.enqueue(kSecond, 0));
    EXPECT_FALSE(queue.enqueue(kThird, 0));
    EXPECT_TRUE(queue.tracks(kFirst));
    EXPECT_TRUE(queue.tracks(kSecond));
    EXPECT_FALSE(queue.tracks(kThird));

    const auto first = queue.takeNext(0);
    EXPECT_TRUE(queue.tracks(kFirst));
    ASSERT_TRUE(queue.completeFailure(first, 1));
    EXPECT_FALSE(queue.tracks(kFirst));

    const auto second = queue.takeNext(1);
    ASSERT_TRUE(queue.completeSuccess(second));
    EXPECT_TRUE(queue.tracks(kSecond));
    queue.resetSession();
    EXPECT_FALSE(queue.tracks(kSecond));
}

TEST(ClientIdentityResolverQueueTests,
     QuarantineSurvivesRestartAndRejectsLateCompletionUntilTerminalEvent)
{
    barrier::ClientIdentityResolverQueue queue(4, 100, 0, 8, 1000, 4);
    queue.startSession();
    ASSERT_TRUE(queue.enqueue(kFirst, 0));
    const auto oldRequest = queue.takeNext(0);
    ASSERT_TRUE(oldRequest.isValid());

    ASSERT_TRUE(queue.quarantinePeripheral(kFirst, 10));
    const quint64 stoppedGeneration = queue.stopSession();
    EXPECT_NE(queue.startSession(), stoppedGeneration);

    EXPECT_TRUE(queue.isQuarantined(kFirst));
    EXPECT_FALSE(queue.enqueue(kFirst, 11));
    EXPECT_FALSE(queue.completeSuccess(oldRequest));
    EXPECT_FALSE(queue.completeFailure(oldRequest, 11));

    EXPECT_TRUE(queue.peripheralRetired(kFirst));
    EXPECT_FALSE(queue.isQuarantined(kFirst));
    EXPECT_TRUE(queue.enqueue(kFirst, 12));
    EXPECT_EQ(kFirst, queue.takeNext(12).peripheralId);
}

TEST(ClientIdentityResolverQueueTests,
     QuarantineTimeoutRequiresCentralRetirementBeforeReadmission)
{
    barrier::ClientIdentityResolverQueue queue(4, 100, 0, 8, 1000, 4);
    queue.startSession();
    ASSERT_TRUE(queue.quarantinePeripheral(kFirst, 100));

    EXPECT_EQ(1100, queue.nextCentralRetirementDeadlineMs());
    EXPECT_FALSE(queue.centralRetirementDue(1099));
    EXPECT_TRUE(queue.centralRetirementDue(1100));
    EXPECT_FALSE(queue.enqueue(kFirst, 1100));

    queue.centralRetired();
    EXPECT_EQ(-1, queue.nextCentralRetirementDeadlineMs());
    EXPECT_FALSE(queue.centralRetirementDue(1101));
    EXPECT_TRUE(queue.enqueue(kFirst, 1101));
}

TEST(ClientIdentityResolverQueueTests,
     QuarantineCapacityFailsClosedUntilCentralRetirement)
{
    barrier::ClientIdentityResolverQueue queue(4, 100, 0, 8, 1000, 2);
    queue.startSession();

    EXPECT_TRUE(queue.quarantinePeripheral(kFirst, 0));
    EXPECT_TRUE(queue.quarantinePeripheral(kSecond, 1));
    EXPECT_FALSE(queue.quarantinePeripheral(kThird, 2));
    EXPECT_EQ(2, queue.quarantineCount());
    EXPECT_TRUE(queue.centralRetirementDue(2));
    EXPECT_FALSE(queue.enqueue(kThird, 2));

    queue.centralRetired();
    EXPECT_EQ(0, queue.quarantineCount());
    EXPECT_TRUE(queue.enqueue(kThird, 3));
}

#ifdef Q_OS_MACOS
TEST(MacProximityControllerLifetimeTests,
     ScanResetHandlerMaySynchronouslyDeleteController)
{
    QPointer<MacProximityController> controller =
        new MacProximityController;
    QObject::connect(
        controller.data(),
        &MacProximityController::clientPresenceScanSessionReset,
        [&controller](quint64) {
            delete controller.data();
        });

    controller->startClientPresenceScanning();

    EXPECT_TRUE(controller.isNull());
    QCoreApplication::processEvents();
}
#endif
