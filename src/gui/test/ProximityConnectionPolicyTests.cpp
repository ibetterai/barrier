/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "../src/ProximityConnectionPolicy.h"

#include <gtest/gtest.h>

namespace {

barrier::ProximityInputs readyInputs()
{
    barrier::ProximityInputs inputs;
    inputs.enabled = true;
    inputs.userWantsBarrier = true;
    inputs.bluetoothAuthorized = true;
    inputs.bluetoothAvailable = true;
    inputs.pairedPeerNear = true;
    inputs.pairedBonjourPresent = true;
    inputs.serverDisplayReady = true;
    return inputs;
}

} // namespace

TEST(ProximityConnectionPolicyTests, requiresEveryGateBeforeInitialLaunch)
{
    barrier::ProximityConnectionPolicy policy;
    barrier::ProximityInputs inputs = readyInputs();
    EXPECT_TRUE(policy.evaluate(inputs).canStartChild);

    policy.reset();
    inputs.pairedPeerNear = false;
    EXPECT_FALSE(policy.evaluate(inputs).shouldRunChild);
    inputs.pairedPeerNear = true;
    inputs.pairedBonjourPresent = false;
    EXPECT_FALSE(policy.evaluate(inputs).shouldRunChild);
    inputs.pairedBonjourPresent = true;
    inputs.serverDisplayReady = false;
    EXPECT_FALSE(policy.evaluate(inputs).shouldRunChild);
}

TEST(ProximityConnectionPolicyTests, overrideBypassesOnlyBluetooth)
{
    barrier::ProximityConnectionPolicy policy;
    barrier::ProximityInputs inputs = readyInputs();
    inputs.bluetoothAuthorized = false;
    inputs.bluetoothAvailable = false;
    inputs.pairedPeerNear = false;
    inputs.manualOverride = true;
    barrier::ProximityDecision decision = policy.evaluate(inputs);
    EXPECT_EQ(barrier::ProximityPolicyState::ManualOverride, decision.state);
    EXPECT_TRUE(decision.shouldRunChild);
    EXPECT_TRUE(decision.canStartChild);

    policy.reset();
    inputs.pairedBonjourPresent = false;
    decision = policy.evaluate(inputs);
    EXPECT_EQ(barrier::ProximityPolicyState::ManualOverride, decision.state);
    EXPECT_FALSE(decision.shouldRunChild);
    inputs.pairedBonjourPresent = true;
    inputs.serverDisplayReady = false;
    EXPECT_FALSE(policy.evaluate(inputs).shouldRunChild);
    inputs.serverDisplayReady = true;
    inputs.userWantsBarrier = false;
    EXPECT_FALSE(policy.evaluate(inputs).shouldRunChild);
}

TEST(ProximityConnectionPolicyTests, disabledModePreservesExistingRunIntent)
{
    barrier::ProximityConnectionPolicy policy;
    barrier::ProximityInputs inputs = readyInputs();
    inputs.enabled = false;

    barrier::ProximityDecision decision = policy.evaluate(inputs);
    EXPECT_EQ(barrier::ProximityPolicyState::Disabled, decision.state);
    EXPECT_TRUE(decision.shouldRunChild);
    EXPECT_TRUE(decision.canStartChild);

    inputs.userWantsBarrier = false;
    decision = policy.evaluate(inputs);
    EXPECT_EQ(barrier::ProximityPolicyState::Disabled, decision.state);
    EXPECT_FALSE(decision.shouldRunChild);
    EXPECT_FALSE(decision.canStartChild);
}

TEST(ProximityConnectionPolicyTests, distinguishesPendingDeniedAndPoweredOff)
{
    barrier::ProximityConnectionPolicy policy;
    barrier::ProximityInputs inputs = readyInputs();
    inputs.bluetoothPermissionPending = true;
    inputs.bluetoothAuthorized = false;
    EXPECT_EQ(barrier::ProximityPolicyState::WaitingForPermission,
              policy.evaluate(inputs).state);

    inputs.bluetoothPermissionPending = false;
    EXPECT_EQ(barrier::ProximityPolicyState::SensorUnavailable,
              policy.evaluate(inputs).state);

    inputs.bluetoothAuthorized = true;
    inputs.bluetoothAvailable = false;
    EXPECT_EQ(barrier::ProximityPolicyState::SensorUnavailable,
              policy.evaluate(inputs).state);
}

TEST(ProximityConnectionPolicyTests, reportsInitialPeerAndNetworkWaits)
{
    barrier::ProximityConnectionPolicy policy;
    barrier::ProximityInputs inputs = readyInputs();
    inputs.pairedPeerNear = false;
    EXPECT_EQ(barrier::ProximityPolicyState::WaitingForPeer,
              policy.evaluate(inputs).state);

    inputs.pairedPeerNear = true;
    inputs.pairedBonjourPresent = false;
    EXPECT_EQ(barrier::ProximityPolicyState::WaitingForNetwork,
              policy.evaluate(inputs).state);

    inputs.pairedBonjourPresent = true;
    inputs.serverDisplayReady = false;
    EXPECT_EQ(barrier::ProximityPolicyState::WaitingForNetwork,
              policy.evaluate(inputs).state);
}

TEST(ProximityConnectionPolicyTests, connectedRequiresProtocolObservation)
{
    barrier::ProximityConnectionPolicy policy;
    barrier::ProximityInputs inputs = readyInputs();
    inputs.childRunning = true;

    barrier::ProximityDecision decision = policy.evaluate(inputs);
    EXPECT_EQ(barrier::ProximityPolicyState::Starting, decision.state);
    EXPECT_TRUE(decision.shouldRunChild);

    inputs.protocolConnected = true;
    decision = policy.evaluate(inputs);
    EXPECT_EQ(barrier::ProximityPolicyState::Connected, decision.state);
    EXPECT_TRUE(decision.shouldRunChild);
}

TEST(ProximityConnectionPolicyTests, bonjourLossHasTenSecondGrace)
{
    barrier::ProximityConnectionPolicy policy;
    barrier::ProximityInputs inputs = readyInputs();
    inputs.childRunning = true;
    inputs.protocolConnected = true;
    inputs.monotonicMs = 100;
    ASSERT_EQ(barrier::ProximityPolicyState::Connected,
              policy.evaluate(inputs).state);

    inputs.pairedBonjourPresent = false;
    inputs.monotonicMs = 200;
    barrier::ProximityDecision decision = policy.evaluate(inputs);
    EXPECT_EQ(barrier::ProximityPolicyState::DepartureGrace, decision.state);
    EXPECT_TRUE(decision.shouldRunChild);
    EXPECT_FALSE(decision.canStartChild);

    inputs.monotonicMs = 10199;
    EXPECT_EQ(barrier::ProximityPolicyState::DepartureGrace,
              policy.evaluate(inputs).state);
    inputs.monotonicMs = 10200;
    decision = policy.evaluate(inputs);
    EXPECT_EQ(barrier::ProximityPolicyState::WaitingForNetwork,
              decision.state);
    EXPECT_FALSE(decision.shouldRunChild);
}

TEST(ProximityConnectionPolicyTests, displayLossGraceCancelsOnRecovery)
{
    barrier::ProximityConnectionPolicy policy;
    barrier::ProximityInputs inputs = readyInputs();
    inputs.childRunning = true;
    inputs.protocolConnected = true;
    inputs.monotonicMs = 0;
    ASSERT_TRUE(policy.evaluate(inputs).canStartChild);

    inputs.serverDisplayReady = false;
    inputs.monotonicMs = 1000;
    EXPECT_EQ(barrier::ProximityPolicyState::DepartureGrace,
              policy.evaluate(inputs).state);

    inputs.serverDisplayReady = true;
    inputs.monotonicMs = 9000;
    barrier::ProximityDecision decision = policy.evaluate(inputs);
    EXPECT_EQ(barrier::ProximityPolicyState::Connected, decision.state);
    EXPECT_TRUE(decision.canStartChild);

    inputs.serverDisplayReady = false;
    inputs.monotonicMs = 15000;
    EXPECT_EQ(barrier::ProximityPolicyState::DepartureGrace,
              policy.evaluate(inputs).state);
    inputs.monotonicMs = 24999;
    EXPECT_EQ(barrier::ProximityPolicyState::DepartureGrace,
              policy.evaluate(inputs).state);
}

TEST(ProximityConnectionPolicyTests, graceNeverLaunchesWithoutAnExistingChild)
{
    barrier::ProximityConnectionPolicy policy;
    barrier::ProximityInputs inputs = readyInputs();
    ASSERT_TRUE(policy.evaluate(inputs).canStartChild);

    inputs.pairedBonjourPresent = false;
    inputs.monotonicMs = 1;
    barrier::ProximityDecision decision = policy.evaluate(inputs);
    EXPECT_EQ(barrier::ProximityPolicyState::DepartureGrace, decision.state);
    EXPECT_FALSE(decision.shouldRunChild);
    EXPECT_FALSE(decision.canStartChild);
}

TEST(ProximityConnectionPolicyTests, bluetoothGraceIsNotExtendedByPolicy)
{
    barrier::ProximityConnectionPolicy policy;
    barrier::ProximityInputs inputs = readyInputs();
    inputs.childRunning = true;
    ASSERT_TRUE(policy.evaluate(inputs).canStartChild);

    inputs.pairedPeerDepartureGrace = true;
    inputs.pairedPeerDepartureWarning = true;
    inputs.monotonicMs = 5000;
    barrier::ProximityDecision decision = policy.evaluate(inputs);
    EXPECT_EQ(barrier::ProximityPolicyState::DepartureGrace, decision.state);
    EXPECT_TRUE(decision.shouldRunChild);
    EXPECT_FALSE(decision.canStartChild);

    // The signal filter returns false only after its own ten seconds. The
    // policy must stop immediately rather than starting another timer.
    inputs.pairedPeerDepartureGrace = false;
    inputs.pairedPeerNear = false;
    inputs.monotonicMs = 10000;
    decision = policy.evaluate(inputs);
    EXPECT_EQ(barrier::ProximityPolicyState::WaitingForPeer, decision.state);
    EXPECT_FALSE(decision.shouldRunChild);
}

TEST(ProximityConnectionPolicyTests, bluetoothGraceNeverLaunchesANewChild)
{
    barrier::ProximityConnectionPolicy policy;
    barrier::ProximityInputs inputs = readyInputs();
    inputs.pairedPeerDepartureGrace = true;
    inputs.pairedPeerDepartureWarning = true;

    const barrier::ProximityDecision decision = policy.evaluate(inputs);
    EXPECT_EQ(barrier::ProximityPolicyState::DepartureGrace, decision.state);
    EXPECT_FALSE(decision.shouldRunChild);
    EXPECT_FALSE(decision.canStartChild);
}

TEST(ProximityConnectionPolicyTests,
     hiddenBluetoothGracePreservesStatusButBlocksLaunch)
{
    barrier::ProximityConnectionPolicy policy;
    barrier::ProximityInputs inputs = readyInputs();
    inputs.childRunning = true;
    inputs.protocolConnected = true;
    inputs.pairedPeerDepartureGrace = true;
    inputs.pairedPeerDepartureWarning = false;

    barrier::ProximityDecision decision = policy.evaluate(inputs);
    EXPECT_EQ(barrier::ProximityPolicyState::Connected, decision.state);
    EXPECT_TRUE(decision.shouldRunChild);
    EXPECT_FALSE(decision.canStartChild);

    inputs.childRunning = false;
    inputs.protocolConnected = false;
    decision = policy.evaluate(inputs);
    EXPECT_EQ(barrier::ProximityPolicyState::WaitingForPeer, decision.state);
    EXPECT_FALSE(decision.shouldRunChild);
    EXPECT_FALSE(decision.canStartChild);
}

TEST(ProximityConnectionPolicyTests,
     reconfigurationGraceKeepsOnlyAnExistingChild)
{
    barrier::ProximityConnectionPolicy policy;
    barrier::ProximityInputs inputs = readyInputs();
    inputs.pairedPeerNear = false;
    inputs.pairedPeerReconfigurationGrace = true;
    inputs.childRunning = true;
    inputs.protocolConnected = true;

    barrier::ProximityDecision decision = policy.evaluate(inputs);
    EXPECT_EQ(barrier::ProximityPolicyState::ReconfigurationGrace,
              decision.state);
    EXPECT_TRUE(decision.shouldRunChild);
    EXPECT_FALSE(decision.canStartChild);

    inputs.childRunning = false;
    inputs.protocolConnected = false;
    decision = policy.evaluate(inputs);
    EXPECT_EQ(barrier::ProximityPolicyState::ReconfigurationGrace,
              decision.state);
    EXPECT_FALSE(decision.shouldRunChild);
    EXPECT_FALSE(decision.canStartChild);
}

TEST(ProximityConnectionPolicyTests, repeatedReadyEvaluationIsStable)
{
    barrier::ProximityConnectionPolicy policy;
    const barrier::ProximityInputs inputs = readyInputs();
    const barrier::ProximityDecision first = policy.evaluate(inputs);

    for (int i = 0; i < 100; ++i) {
        const barrier::ProximityDecision repeated = policy.evaluate(inputs);
        EXPECT_EQ(first.state, repeated.state);
        EXPECT_EQ(first.shouldRunChild, repeated.shouldRunChild);
        EXPECT_EQ(first.canStartChild, repeated.canStartChild);
    }
}

TEST(ProximityConnectionPolicyTests, serverAdvertisesOnlyWhileStartIsIntended)
{
    EXPECT_TRUE(barrier::shouldAdvertiseProximityServer(true, true, true));
    EXPECT_FALSE(barrier::shouldAdvertiseProximityServer(false, true, true));
    EXPECT_FALSE(barrier::shouldAdvertiseProximityServer(true, false, true));
    EXPECT_FALSE(barrier::shouldAdvertiseProximityServer(true, true, false));
}

TEST(ProximityRestartPolicyTests, permitsOneDelayedRetryPerEligibleEpoch)
{
    barrier::ProximityRestartPolicy policy;
    policy.updateEligibility(true);
    EXPECT_TRUE(policy.automaticLaunchAllowed());

    EXPECT_EQ(barrier::ProximityChildExitAction::RetryAfterDelay,
              policy.childExitedUnexpectedly());
    EXPECT_TRUE(policy.retryPending());
    EXPECT_FALSE(policy.automaticLaunchAllowed());
    EXPECT_TRUE(policy.takeScheduledRetry());
    EXPECT_TRUE(policy.automaticLaunchAllowed());

    EXPECT_EQ(barrier::ProximityChildExitAction::Suppress,
              policy.childExitedUnexpectedly());
    EXPECT_TRUE(policy.suppressed());
    EXPECT_FALSE(policy.automaticLaunchAllowed());
    EXPECT_FALSE(policy.takeScheduledRetry());
}

TEST(ProximityRestartPolicyTests, suppressionSurvivesSteadyReadyGate)
{
    barrier::ProximityRestartPolicy policy;
    policy.updateEligibility(true);
    ASSERT_EQ(barrier::ProximityChildExitAction::RetryAfterDelay,
              policy.childExitedUnexpectedly());
    ASSERT_TRUE(policy.takeScheduledRetry());
    ASSERT_EQ(barrier::ProximityChildExitAction::Suppress,
              policy.childExitedUnexpectedly());

    for (int i = 0; i < 10; ++i) {
        policy.updateEligibility(true);
        EXPECT_TRUE(policy.suppressed());
        EXPECT_FALSE(policy.automaticLaunchAllowed());
    }
}

TEST(ProximityRestartPolicyTests, falseThenTrueGateStartsNewEpoch)
{
    barrier::ProximityRestartPolicy policy;
    policy.updateEligibility(true);
    ASSERT_EQ(barrier::ProximityChildExitAction::RetryAfterDelay,
              policy.childExitedUnexpectedly());
    ASSERT_TRUE(policy.takeScheduledRetry());
    ASSERT_EQ(barrier::ProximityChildExitAction::Suppress,
              policy.childExitedUnexpectedly());

    policy.updateEligibility(false);
    EXPECT_FALSE(policy.automaticLaunchAllowed());
    EXPECT_EQ(barrier::ProximityChildExitAction::Ignore,
              policy.childExitedUnexpectedly());
    policy.updateEligibility(true);
    EXPECT_TRUE(policy.automaticLaunchAllowed());
    EXPECT_FALSE(policy.suppressed());
    EXPECT_EQ(barrier::ProximityChildExitAction::RetryAfterDelay,
              policy.childExitedUnexpectedly());
}

TEST(ProximityRestartPolicyTests, gateLossCancelsPendingRetry)
{
    barrier::ProximityRestartPolicy policy;
    policy.updateEligibility(true);
    ASSERT_EQ(barrier::ProximityChildExitAction::RetryAfterDelay,
              policy.childExitedUnexpectedly());
    policy.updateEligibility(false);
    EXPECT_FALSE(policy.retryPending());
    EXPECT_FALSE(policy.takeScheduledRetry());
}

TEST(ProximityProcessExitTrackerTests,
     expectedForcedStopIsInformationalAndConsumedOnce)
{
    barrier::ProximityProcessExitTracker tracker;
    const quint64 generation = tracker.beginProcess();
    tracker.expectPolicyStop(generation);

    const barrier::ProximityProcessExitClassification expected =
        tracker.classifyAndConsume(generation, 9, true, 9);
    EXPECT_TRUE(expected.expectedPolicyStop);
    EXPECT_EQ(barrier::ProximityProcessExitLogDisposition::ExpectedPolicyStop,
              expected.logDisposition);

    const barrier::ProximityProcessExitClassification repeated =
        tracker.classifyAndConsume(generation, 9, true, 9);
    EXPECT_FALSE(repeated.expectedPolicyStop);
    EXPECT_EQ(barrier::ProximityProcessExitLogDisposition::Error,
              repeated.logDisposition);
}

TEST(ProximityProcessExitTrackerTests, pureClassifierRequiresEveryExpectedKillFact)
{
    const barrier::ProximityProcessExitClassification expected =
        barrier::classifyProximityProcessExit(true, 9, true, 9);
    EXPECT_TRUE(expected.expectedPolicyStop);
    EXPECT_EQ(barrier::ProximityProcessExitLogDisposition::ExpectedPolicyStop,
              expected.logDisposition);

    EXPECT_EQ(
        barrier::ProximityProcessExitLogDisposition::Error,
        barrier::classifyProximityProcessExit(false, 9, true, 9)
            .logDisposition);
    EXPECT_EQ(
        barrier::ProximityProcessExitLogDisposition::Error,
        barrier::classifyProximityProcessExit(true, 9, false, 9)
            .logDisposition);
    EXPECT_EQ(
        barrier::ProximityProcessExitLogDisposition::Error,
        barrier::classifyProximityProcessExit(true, 6, true, 9)
            .logDisposition);
}

TEST(ProximityProcessExitTrackerTests, stopIntentIsScopedToItsProcessGeneration)
{
    barrier::ProximityProcessExitTracker tracker;
    const quint64 expectedGeneration = tracker.beginProcess();
    const quint64 unrelatedGeneration = tracker.beginProcess();
    tracker.expectPolicyStop(expectedGeneration);

    const barrier::ProximityProcessExitClassification unrelated =
        tracker.classifyAndConsume(unrelatedGeneration, 9, true, 9);
    EXPECT_FALSE(unrelated.expectedPolicyStop);
    EXPECT_EQ(barrier::ProximityProcessExitLogDisposition::Error,
              unrelated.logDisposition);

    const barrier::ProximityProcessExitClassification expected =
        tracker.classifyAndConsume(expectedGeneration, 9, true, 9);
    EXPECT_TRUE(expected.expectedPolicyStop);
    EXPECT_EQ(barrier::ProximityProcessExitLogDisposition::ExpectedPolicyStop,
              expected.logDisposition);
}

TEST(ProximityProcessExitTrackerTests,
     expectedGracefulAndUnexpectedCrashKeepExistingLogSemantics)
{
    barrier::ProximityProcessExitTracker tracker;
    const quint64 gracefulGeneration = tracker.beginProcess();
    tracker.expectPolicyStop(gracefulGeneration);

    const barrier::ProximityProcessExitClassification graceful =
        tracker.classifyAndConsume(gracefulGeneration, 0, false, 9);
    EXPECT_TRUE(graceful.expectedPolicyStop);
    EXPECT_EQ(barrier::ProximityProcessExitLogDisposition::Normal,
              graceful.logDisposition);

    const quint64 unexpectedGeneration = tracker.beginProcess();
    const barrier::ProximityProcessExitClassification unexpected =
        tracker.classifyAndConsume(unexpectedGeneration, 9, true, 9);
    EXPECT_FALSE(unexpected.expectedPolicyStop);
    EXPECT_EQ(barrier::ProximityProcessExitLogDisposition::Error,
              unexpected.logDisposition);
}

TEST(ProximityProcessExitTrackerTests,
     expectedDifferentCrashStillSuppressesRetryButLogsError)
{
    barrier::ProximityProcessExitTracker tracker;
    const quint64 generation = tracker.beginProcess();
    tracker.expectPolicyStop(generation);

    const barrier::ProximityProcessExitClassification classification =
        tracker.classifyAndConsume(generation, 6, true, 9);
    EXPECT_TRUE(classification.expectedPolicyStop);
    EXPECT_EQ(barrier::ProximityProcessExitLogDisposition::Error,
              classification.logDisposition);
}
