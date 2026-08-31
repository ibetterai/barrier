/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "ProximityConnectionPolicy.h"

namespace barrier {
namespace {

const qint64 kNetworkDepartureGraceMs = 10000;

bool intervalElapsed(qint64 nowMs, qint64 startMs, qint64 intervalMs)
{
    if (nowMs < startMs) {
        return false;
    }
    const quint64 elapsed = static_cast<quint64>(nowMs) -
                            static_cast<quint64>(startMs);
    return elapsed >= static_cast<quint64>(intervalMs);
}

} // namespace

ProximityInputs::ProximityInputs() :
    enabled(false),
    userWantsBarrier(false),
    bluetoothPermissionPending(false),
    bluetoothAuthorized(false),
    bluetoothAvailable(false),
    pairedPeerNear(false),
    pairedPeerReconfigurationGrace(false),
    pairedPeerDepartureGrace(false),
    pairedPeerDepartureWarning(false),
    pairedBonjourPresent(false),
    serverDisplayReady(false),
    manualOverride(false),
    childRunning(false),
    protocolConnected(false),
    monotonicMs(0)
{
}

ProximityConnectionPolicy::ProximityConnectionPolicy() :
    m_networkWasReady(false),
    m_networkGraceActive(false),
    m_networkGraceStartedMs(0)
{
}

bool shouldAdvertiseProximityServer(bool serverMode,
                                    bool advertiserEnabled,
                                    bool userWantsBarrier)
{
    return serverMode && advertiserEnabled && userWantsBarrier;
}

void ProximityConnectionPolicy::reset()
{
    m_networkWasReady = false;
    m_networkGraceActive = false;
    m_networkGraceStartedMs = 0;
}

ProximityDecision ProximityConnectionPolicy::evaluate(
    const ProximityInputs& inputs)
{
    if (!inputs.enabled || !inputs.userWantsBarrier) {
        reset();
        return {ProximityPolicyState::Disabled,
                !inputs.enabled && inputs.userWantsBarrier,
                !inputs.enabled && inputs.userWantsBarrier};
    }

    if (!inputs.manualOverride) {
        if (inputs.bluetoothPermissionPending) {
            reset();
            return {ProximityPolicyState::WaitingForPermission, false, false};
        }
        if (!inputs.bluetoothAuthorized || !inputs.bluetoothAvailable) {
            reset();
            return {ProximityPolicyState::SensorUnavailable, false, false};
        }
        if (!inputs.pairedPeerNear &&
            !inputs.pairedPeerReconfigurationGrace) {
            // ProximitySignalFilter already consumed the Bluetooth ten-second
            // grace period. Do not add a second grace window here.
            reset();
            return {ProximityPolicyState::WaitingForPeer, false, false};
        }
    }

    const bool networkReady = inputs.pairedBonjourPresent &&
                              inputs.serverDisplayReady;
    if (!networkReady) {
        if (m_networkWasReady && !m_networkGraceActive) {
            m_networkGraceActive = true;
            m_networkGraceStartedMs = inputs.monotonicMs;
        }
        if (m_networkGraceActive &&
            !intervalElapsed(inputs.monotonicMs,
                             m_networkGraceStartedMs,
                             kNetworkDepartureGraceMs)) {
            return {ProximityPolicyState::DepartureGrace,
                    inputs.childRunning,
                    false};
        }

        m_networkWasReady = false;
        m_networkGraceActive = false;
        return {inputs.manualOverride
                    ? ProximityPolicyState::ManualOverride
                    : ProximityPolicyState::WaitingForNetwork,
                false,
                false};
    }

    // A recovered Bonjour/display gate cancels the outstanding grace period.
    m_networkWasReady = true;
    m_networkGraceActive = false;

    if (!inputs.manualOverride &&
        inputs.pairedPeerReconfigurationGrace) {
        return {ProximityPolicyState::ReconfigurationGrace,
                inputs.childRunning,
                false};
    }
    if (!inputs.manualOverride && inputs.pairedPeerDepartureGrace) {
        const ProximityPolicyState state =
            inputs.pairedPeerDepartureWarning
                ? ProximityPolicyState::DepartureGrace
                : (inputs.childRunning
                    ? (inputs.protocolConnected
                        ? ProximityPolicyState::Connected
                        : ProximityPolicyState::Starting)
                    : ProximityPolicyState::WaitingForPeer);
        return {state,
                inputs.childRunning,
                false};
    }
    if (inputs.manualOverride) {
        return {ProximityPolicyState::ManualOverride, true, true};
    }
    return {
        inputs.protocolConnected
            ? ProximityPolicyState::Connected
            : ProximityPolicyState::Starting,
        true,
        true
    };
}

ProximityRestartPolicy::ProximityRestartPolicy()
{
    reset();
}

void ProximityRestartPolicy::reset()
{
    m_eligible = false;
    m_retryUsed = false;
    m_retryPending = false;
    m_suppressed = false;
}

void ProximityRestartPolicy::updateEligibility(bool eligible)
{
    if (!eligible) {
        m_eligible = false;
        m_retryPending = false;
        return;
    }
    if (!m_eligible) {
        m_retryUsed = false;
        m_retryPending = false;
        m_suppressed = false;
    }
    m_eligible = true;
}

ProximityChildExitAction
ProximityRestartPolicy::childExitedUnexpectedly()
{
    if (!m_eligible) {
        return ProximityChildExitAction::Ignore;
    }
    if (!m_retryUsed) {
        m_retryUsed = true;
        m_retryPending = true;
        return ProximityChildExitAction::RetryAfterDelay;
    }

    m_retryPending = false;
    m_suppressed = true;
    return ProximityChildExitAction::Suppress;
}

bool ProximityRestartPolicy::takeScheduledRetry()
{
    if (!m_retryPending || !m_eligible || m_suppressed) {
        return false;
    }
    m_retryPending = false;
    return true;
}

bool ProximityRestartPolicy::automaticLaunchAllowed() const
{
    return m_eligible && !m_retryPending && !m_suppressed;
}

bool ProximityRestartPolicy::retryPending() const
{
    return m_retryPending;
}

bool ProximityRestartPolicy::suppressed() const
{
    return m_suppressed;
}

ProximityProcessExitClassification
classifyProximityProcessExit(
    bool expectedPolicyStop, int exitCode, bool crashed,
    int forcedTerminationExitCode)
{
    if (expectedPolicyStop && crashed &&
        exitCode == forcedTerminationExitCode) {
        return {true,
                ProximityProcessExitLogDisposition::ExpectedPolicyStop};
    }
    return {
        expectedPolicyStop,
        exitCode == 0
            ? ProximityProcessExitLogDisposition::Normal
            : ProximityProcessExitLogDisposition::Error
    };
}

ProximityProcessExitTracker::ProximityProcessExitTracker() :
    m_nextGeneration(0)
{
}

quint64 ProximityProcessExitTracker::beginProcess()
{
    ++m_nextGeneration;
    if (m_nextGeneration == 0) {
        ++m_nextGeneration;
    }
    return m_nextGeneration;
}

void ProximityProcessExitTracker::expectPolicyStop(quint64 generation)
{
    if (generation != 0) {
        m_expectedPolicyStopGenerations.insert(generation);
    }
}

void ProximityProcessExitTracker::forgetProcess(quint64 generation)
{
    m_expectedPolicyStopGenerations.remove(generation);
}

ProximityProcessExitClassification
ProximityProcessExitTracker::classifyAndConsume(
    quint64 generation, int exitCode, bool crashed,
    int forcedTerminationExitCode)
{
    const bool expectedPolicyStop = generation != 0 &&
        m_expectedPolicyStopGenerations.remove(generation) != 0;
    return classifyProximityProcessExit(
        expectedPolicyStop, exitCode, crashed, forcedTerminationExitCode);
}

} // namespace barrier
