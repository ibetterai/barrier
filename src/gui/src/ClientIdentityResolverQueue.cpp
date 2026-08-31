/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "ClientIdentityResolverQueue.h"

#include <QtGlobal>

#include <limits>

namespace barrier {

ClientIdentityResolverQueue::ClientIdentityResolverQueue(
    int maxOutstanding, qint64 timeoutMs, qint64 retryCooldownMs,
    int maxAttemptsPerSession, qint64 quarantineTimeoutMs,
    int maxQuarantined) :
    m_maxOutstanding(qMax(1, maxOutstanding)),
    m_timeoutMs(qMax<qint64>(1, timeoutMs)),
    m_retryCooldownMs(qMax<qint64>(0, retryCooldownMs)),
    m_maxAttemptsPerSession(qMax(1, maxAttemptsPerSession)),
    m_quarantineTimeoutMs(qMax<qint64>(1, quarantineTimeoutMs)),
    m_maxQuarantined(qMax(1, maxQuarantined))
{
}

quint64 ClientIdentityResolverQueue::startSession()
{
    m_running = true;
    return resetSession();
}

quint64 ClientIdentityResolverQueue::resetSession()
{
    ++m_generation;
    if (m_generation == 0) {
        ++m_generation;
    }
    clearState();
    return m_generation;
}

quint64 ClientIdentityResolverQueue::stopSession()
{
    m_running = false;
    return resetSession();
}

bool ClientIdentityResolverQueue::isRunning() const
{
    return m_running;
}

quint64 ClientIdentityResolverQueue::generation() const
{
    return m_generation;
}

bool ClientIdentityResolverQueue::enqueue(
    const QUuid& peripheralId, qint64 nowMs)
{
    if (!m_running || peripheralId.isNull() ||
        m_attemptCount >= m_maxAttemptsPerSession ||
        m_centralRetirementRequired ||
        m_quarantineDeadlines.contains(peripheralId)) {
        return false;
    }

    pruneCooldowns(nowMs);
    if (m_active.peripheralId == peripheralId ||
        m_queued.contains(peripheralId) ||
        m_resolved.contains(peripheralId) ||
        m_cooldownUntil.contains(peripheralId)) {
        return false;
    }

    const int outstanding = m_pending.size() + (m_active.isValid() ? 1 : 0);
    if (outstanding >= m_maxOutstanding) {
        return false;
    }

    m_pending.enqueue(peripheralId);
    m_queued.insert(peripheralId);
    return true;
}

ClientIdentityResolveRequest ClientIdentityResolverQueue::takeNext(qint64 nowMs)
{
    if (!m_running || m_active.isValid() || m_pending.isEmpty() ||
        centralRetirementDue(nowMs)) {
        return {};
    }
    if (m_attemptCount >= m_maxAttemptsPerSession) {
        m_pending.clear();
        m_queued.clear();
        return {};
    }

    m_active.peripheralId = m_pending.dequeue();
    m_active.generation = m_generation;
    m_queued.remove(m_active.peripheralId);
    m_activeDeadlineMs = nowMs + m_timeoutMs;
    ++m_attemptCount;
    return m_active;
}

bool ClientIdentityResolverQueue::completeSuccess(
    const ClientIdentityResolveRequest& request)
{
    if (!matchesActive(request)) {
        return false;
    }

    m_resolved.insert(request.peripheralId);
    m_active = {};
    m_activeDeadlineMs = -1;
    return true;
}

bool ClientIdentityResolverQueue::completeFailure(
    const ClientIdentityResolveRequest& request, qint64 nowMs)
{
    if (!matchesActive(request)) {
        return false;
    }

    if (m_retryCooldownMs > 0) {
        m_cooldownUntil.insert(
            request.peripheralId, nowMs + m_retryCooldownMs);
    }
    m_active = {};
    m_activeDeadlineMs = -1;
    return true;
}

bool ClientIdentityResolverQueue::expireActive(
    qint64 nowMs, ClientIdentityResolveRequest* expiredRequest)
{
    if (!m_active.isValid() || nowMs < m_activeDeadlineMs) {
        return false;
    }

    const ClientIdentityResolveRequest request = m_active;
    if (!completeFailure(request, nowMs)) {
        return false;
    }
    if (expiredRequest != nullptr) {
        *expiredRequest = request;
    }
    return true;
}

ClientIdentityResolveRequest ClientIdentityResolverQueue::activeRequest() const
{
    return m_active;
}

qint64 ClientIdentityResolverQueue::activeDeadlineMs() const
{
    return m_activeDeadlineMs;
}

int ClientIdentityResolverQueue::pendingCount() const
{
    return m_pending.size();
}

int ClientIdentityResolverQueue::attemptCount() const
{
    return m_attemptCount;
}

bool ClientIdentityResolverQueue::tracks(const QUuid& peripheralId) const
{
    return !peripheralId.isNull() &&
        (m_active.peripheralId == peripheralId ||
         m_queued.contains(peripheralId) ||
         m_resolved.contains(peripheralId));
}

bool ClientIdentityResolverQueue::quarantinePeripheral(
    const QUuid& peripheralId, qint64 nowMs)
{
    if (peripheralId.isNull()) {
        return false;
    }
    if (m_quarantineDeadlines.contains(peripheralId)) {
        return true;
    }
    if (m_quarantineDeadlines.size() >= m_maxQuarantined) {
        // Dropping a canceled UUID would permit stale callbacks to cross into
        // a later request. Fail closed until the owning central is retired.
        m_centralRetirementRequired = true;
        return false;
    }

    const qint64 boundedNow = qMax<qint64>(0, nowMs);
    const qint64 maximum = std::numeric_limits<qint64>::max();
    const qint64 deadline = boundedNow > maximum - m_quarantineTimeoutMs
        ? maximum
        : boundedNow + m_quarantineTimeoutMs;
    m_quarantineDeadlines.insert(peripheralId, deadline);
    return true;
}

bool ClientIdentityResolverQueue::peripheralRetired(
    const QUuid& peripheralId)
{
    return m_quarantineDeadlines.remove(peripheralId) != 0;
}

bool ClientIdentityResolverQueue::isQuarantined(
    const QUuid& peripheralId) const
{
    return m_quarantineDeadlines.contains(peripheralId);
}

int ClientIdentityResolverQueue::quarantineCount() const
{
    return m_quarantineDeadlines.size();
}

qint64 ClientIdentityResolverQueue::nextCentralRetirementDeadlineMs() const
{
    if (m_centralRetirementRequired) {
        return 0;
    }
    qint64 earliest = -1;
    for (auto quarantine = m_quarantineDeadlines.constBegin();
         quarantine != m_quarantineDeadlines.constEnd(); ++quarantine) {
        if (earliest < 0 || quarantine.value() < earliest) {
            earliest = quarantine.value();
        }
    }
    return earliest;
}

bool ClientIdentityResolverQueue::centralRetirementDue(qint64 nowMs) const
{
    const qint64 deadline = nextCentralRetirementDeadlineMs();
    return deadline >= 0 && qMax<qint64>(0, nowMs) >= deadline;
}

void ClientIdentityResolverQueue::centralRetired()
{
    m_quarantineDeadlines.clear();
    m_centralRetirementRequired = false;
}

void ClientIdentityResolverQueue::clearState()
{
    m_pending.clear();
    m_queued.clear();
    m_resolved.clear();
    m_cooldownUntil.clear();
    m_active = {};
    m_activeDeadlineMs = -1;
    m_attemptCount = 0;
}

void ClientIdentityResolverQueue::pruneCooldowns(qint64 nowMs)
{
    for (auto it = m_cooldownUntil.begin(); it != m_cooldownUntil.end();) {
        if (it.value() <= nowMs) {
            it = m_cooldownUntil.erase(it);
        }
        else {
            ++it;
        }
    }
}

bool ClientIdentityResolverQueue::matchesActive(
    const ClientIdentityResolveRequest& request) const
{
    return request.isValid() && request.generation == m_generation &&
        request.generation == m_active.generation &&
        request.peripheralId == m_active.peripheralId;
}

} // namespace barrier
