/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "ZeroconfRecord.h"

#include <QList>
#include <QMap>
#include <QString>

namespace barrier {

enum class ClientPresenceSelectionStatus {
    Unavailable,
    Ready,
    Ambiguous
};

struct ClientPresenceSelection {
    ClientPresenceSelectionStatus status{
        ClientPresenceSelectionStatus::Unavailable};
    QString routingId;
};

// A real Barrier protocol connection is short-lived evidence that a Bonjour
// route belongs to that screen. Keep it only long enough for an out-of-order
// Bonjour update, consume it after one association decision, and cap pending
// screens so discovery traffic cannot grow state forever.
class ClientPresenceConnectionEvidence {
public:
    static constexpr qint64 kDefaultLifetimeMs = 30 * 1000;
    static constexpr int kDefaultCapacity = 64;

    explicit ClientPresenceConnectionEvidence(
        qint64 lifetimeMs = kDefaultLifetimeMs,
        int capacity = kDefaultCapacity) :
        m_lifetimeMs(qMax<qint64>(1, lifetimeMs)),
        m_capacity(qMax(1, capacity))
    {
    }

    void record(const QString& screenName, qint64 nowMs)
    {
        discardExpired(nowMs);
        if (screenName.isEmpty()) {
            return;
        }
        if (!m_deadlines.contains(screenName) &&
            m_deadlines.size() >= m_capacity) {
            auto oldest = m_deadlines.begin();
            for (auto candidate = m_deadlines.begin();
                 candidate != m_deadlines.end(); ++candidate) {
                if (candidate.value() < oldest.value()) {
                    oldest = candidate;
                }
            }
            m_deadlines.erase(oldest);
        }
        m_deadlines.insert(screenName, nowMs + m_lifetimeMs);
    }

    bool contains(const QString& screenName, qint64 nowMs)
    {
        discardExpired(nowMs);
        return m_deadlines.contains(screenName);
    }

    void discard(const QString& screenName)
    {
        m_deadlines.remove(screenName);
    }

    QList<QString> pending(qint64 nowMs)
    {
        discardExpired(nowMs);
        return m_deadlines.keys();
    }

    int size(qint64 nowMs)
    {
        discardExpired(nowMs);
        return m_deadlines.size();
    }

private:
    void discardExpired(qint64 nowMs)
    {
        for (auto evidence = m_deadlines.begin();
             evidence != m_deadlines.end();) {
            if (evidence.value() <= nowMs) {
                evidence = m_deadlines.erase(evidence);
            }
            else {
                ++evidence;
            }
        }
    }

    qint64 m_lifetimeMs;
    int m_capacity;
    QMap<QString, qint64> m_deadlines;
};

inline ClientPresenceSelection selectClientPresenceAssociation(
    const QList<ZeroconfRecord>& records,
    const QString& canonicalScreenName,
    const QString& serverProximityId)
{
    const ZeroconfRecord* candidate = nullptr;
    int currentRoutesForScreen = 0;
    for (const ZeroconfRecord& record : records) {
        if (record.serviceName == canonicalScreenName &&
            record.isCurrentLocalWakeRoute(serverProximityId)) {
            ++currentRoutesForScreen;
            candidate = &record;
        }
    }
    if (currentRoutesForScreen == 0 || candidate == nullptr) {
        return {};
    }
    if (currentRoutesForScreen != 1) {
        return {ClientPresenceSelectionStatus::Ambiguous, QString()};
    }

    const QString routingId = candidate->clientRoutingId();
    if (routingId.isEmpty()) {
        return {};
    }
    for (const ZeroconfRecord& record : records) {
        if (&record != candidate &&
            record.isCurrentLocalWakeRoute(serverProximityId) &&
            record.clientRoutingId() == routingId) {
            return {ClientPresenceSelectionStatus::Ambiguous, QString()};
        }
    }
    return {ClientPresenceSelectionStatus::Ready, routingId};
}

} // namespace barrier
