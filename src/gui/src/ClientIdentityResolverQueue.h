/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include <QHash>
#include <QQueue>
#include <QSet>
#include <QUuid>

namespace barrier {

struct ClientIdentityResolveRequest
{
    QUuid peripheralId;
    quint64 generation{0};

    bool isValid() const
    {
        return !peripheralId.isNull() && generation != 0;
    }
};

// CoreBluetooth only permits one identity read at a time in the controller.
// This class keeps scheduling deterministic and independent of delegate timing.
class ClientIdentityResolverQueue
{
public:
    explicit ClientIdentityResolverQueue(
        int maxOutstanding = 16,
        qint64 timeoutMs = 5000,
        qint64 retryCooldownMs = 15000,
        int maxAttemptsPerSession = 32,
        qint64 quarantineTimeoutMs = 5000,
        int maxQuarantined = 16);

    quint64 startSession();
    quint64 resetSession();
    quint64 stopSession();

    bool isRunning() const;
    quint64 generation() const;

    bool enqueue(const QUuid& peripheralId, qint64 nowMs);
    ClientIdentityResolveRequest takeNext(qint64 nowMs);

    bool completeSuccess(const ClientIdentityResolveRequest& request);
    bool completeFailure(
        const ClientIdentityResolveRequest& request, qint64 nowMs);
    bool expireActive(
        qint64 nowMs, ClientIdentityResolveRequest* expiredRequest = nullptr);

    ClientIdentityResolveRequest activeRequest() const;
    qint64 activeDeadlineMs() const;
    int pendingCount() const;
    int attemptCount() const;
    bool tracks(const QUuid& peripheralId) const;

    // CoreBluetooth cancellation is asynchronous. A canceled peripheral must
    // not be admitted in a later resolver generation until its terminal
    // callback arrives. If that callback never arrives, the controller must
    // retire its CBCentralManager before calling centralRetired().
    bool quarantinePeripheral(const QUuid& peripheralId, qint64 nowMs);
    bool peripheralRetired(const QUuid& peripheralId);
    bool isQuarantined(const QUuid& peripheralId) const;
    int quarantineCount() const;
    qint64 nextCentralRetirementDeadlineMs() const;
    bool centralRetirementDue(qint64 nowMs) const;
    void centralRetired();

private:
    void clearState();
    void pruneCooldowns(qint64 nowMs);
    bool matchesActive(const ClientIdentityResolveRequest& request) const;

    int m_maxOutstanding;
    qint64 m_timeoutMs;
    qint64 m_retryCooldownMs;
    int m_maxAttemptsPerSession;
    qint64 m_quarantineTimeoutMs;
    int m_maxQuarantined;
    int m_attemptCount{0};
    quint64 m_generation{0};
    bool m_running{false};
    bool m_centralRetirementRequired{false};
    QQueue<QUuid> m_pending;
    QSet<QUuid> m_queued;
    QSet<QUuid> m_resolved;
    QHash<QUuid, qint64> m_cooldownUntil;
    QHash<QUuid, qint64> m_quarantineDeadlines;
    ClientIdentityResolveRequest m_active;
    qint64 m_activeDeadlineMs{-1};
};

} // namespace barrier
