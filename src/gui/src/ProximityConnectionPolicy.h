/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include <QSet>
#include <QtGlobal>

namespace barrier {

enum class ProximityPolicyState {
    Disabled,
    WaitingForPermission,
    SensorUnavailable,
    WaitingForPeer,
    WaitingForNetwork,
    Starting,
    Connected,
    DepartureGrace,
    ReconfigurationGrace,
    ManualOverride,
    RetrySuppressed
};

struct ProximityInputs {
    ProximityInputs();

    bool enabled;
    bool userWantsBarrier;
    bool bluetoothPermissionPending;
    bool bluetoothAuthorized;
    bool bluetoothAvailable;
    bool pairedPeerNear;
    bool pairedPeerReconfigurationGrace;
    bool pairedPeerDepartureGrace;
    bool pairedPeerDepartureWarning;
    bool pairedBonjourPresent;
    bool serverDisplayReady;
    bool manualOverride;
    bool childRunning;
    bool protocolConnected;
    qint64 monotonicMs;
};

struct ProximityDecision {
    ProximityPolicyState state;
    bool shouldRunChild;
    bool canStartChild;
};

bool shouldAdvertiseProximityServer(bool serverMode,
                                    bool advertiserEnabled,
                                    bool userWantsBarrier);

class ProximityConnectionPolicy {
public:
    ProximityConnectionPolicy();

    ProximityDecision evaluate(const ProximityInputs& inputs);
    void reset();

private:
    bool m_networkWasReady;
    bool m_networkGraceActive;
    qint64 m_networkGraceStartedMs;
};

enum class ProximityChildExitAction {
    Ignore,
    RetryAfterDelay,
    Suppress
};

// Limits an otherwise healthy gate epoch to one delayed child retry. A new
// epoch begins only after launch eligibility becomes false and then true.
class ProximityRestartPolicy {
public:
    ProximityRestartPolicy();

    void updateEligibility(bool eligible);
    ProximityChildExitAction childExitedUnexpectedly();
    bool takeScheduledRetry();
    bool automaticLaunchAllowed() const;
    bool retryPending() const;
    bool suppressed() const;
    void reset();

private:
    bool m_eligible;
    bool m_retryUsed;
    bool m_retryPending;
    bool m_suppressed;
};

enum class ProximityProcessExitLogDisposition {
    Normal,
    ExpectedPolicyStop,
    Error
};

struct ProximityProcessExitClassification {
    bool expectedPolicyStop;
    ProximityProcessExitLogDisposition logDisposition;
};

ProximityProcessExitClassification classifyProximityProcessExit(
    bool expectedPolicyStop, int exitCode, bool crashed,
    int forcedTerminationExitCode);

// Associates an expected policy stop with the exact child process generation
// that received it. Classification consumes that intent once, so a late signal
// from another child cannot hide an unrelated crash.
class ProximityProcessExitTracker {
public:
    ProximityProcessExitTracker();

    quint64 beginProcess();
    void expectPolicyStop(quint64 generation);
    void forgetProcess(quint64 generation);
    ProximityProcessExitClassification classifyAndConsume(
        quint64 generation, int exitCode, bool crashed,
        int forcedTerminationExitCode);

private:
    quint64 m_nextGeneration;
    QSet<quint64> m_expectedPolicyStopGenerations;
};

} // namespace barrier
