/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "ProximitySignalFilter.h"

namespace barrier {
namespace {

const int kInvalidCoreBluetoothRssi = 127;
const int kRequiredEntrySamples = 3;
const qint64 kDepartureTimeoutMs = 10000;
const qint64 kSignalFreshnessMs = 1500;
const qint64 kDepartureWarningDelayMs = 3000;
const double kSampleWeight = 0.25;

quint64 elapsedSince(qint64 nowMs, qint64 thenMs)
{
    return static_cast<quint64>(nowMs) -
           static_cast<quint64>(thenMs);
}

} // namespace

bool ProximityThresholdPolicy::isValid() const
{
    return connectDbm >= kProximityMinimumThresholdDbm &&
           connectDbm <= kProximityMaximumThresholdDbm &&
           departureDbm >= kProximityMinimumThresholdDbm &&
           departureDbm <= kProximityMaximumThresholdDbm &&
           connectDbm - departureDbm >=
               kProximityMinimumHysteresisDb;
}

bool operator==(const ProximityThresholdPolicy& left,
                const ProximityThresholdPolicy& right)
{
    return left.connectDbm == right.connectDbm &&
           left.departureDbm == right.departureDbm;
}

bool operator!=(const ProximityThresholdPolicy& left,
                const ProximityThresholdPolicy& right)
{
    return !(left == right);
}

ProximitySignalFilter::ProximitySignalFilter(
    const ProximityThresholdPolicy& policy) :
    m_policy(policy.isValid() ? policy : ProximityThresholdPolicy()),
    m_filteredDbm(0.0),
    m_hasFilteredSample(false),
    m_entrySamples(0),
    m_lastExitQualifyingSampleMs(0),
    m_lastValidSampleMs(0),
    m_near(false),
    m_reconfigurationGrace(false),
    m_reconfigurationStartedMs(0),
    m_reconfigurationDurationMs(0)
{
}

bool ProximitySignalFilter::reconfigure(
    const ProximityThresholdPolicy& policy, qint64 monotonicMs)
{
    if (!policy.isValid() || policy == m_policy) {
        return false;
    }

    const bool preserveBoundedPresence = isNear(monotonicMs);
    quint64 remainingPresenceMs = 0;
    if (preserveBoundedPresence) {
        const quint64 elapsed = monotonicMs < m_lastExitQualifyingSampleMs
            ? 0
            : elapsedSince(monotonicMs, m_lastExitQualifyingSampleMs);
        if (elapsed < static_cast<quint64>(kDepartureTimeoutMs)) {
            remainingPresenceMs =
                static_cast<quint64>(kDepartureTimeoutMs) - elapsed;
        }
    }
    m_policy = policy;
    reset();
    if (remainingPresenceMs != 0) {
        // A policy change never carries the old EWMA or entry counter. This
        // separate grace can keep an existing child alive, but it is not
        // launch eligibility and cannot outlive the old departure deadline.
        m_reconfigurationGrace = true;
        m_reconfigurationStartedMs = monotonicMs;
        m_reconfigurationDurationMs = remainingPresenceMs;
    }
    return true;
}

void ProximitySignalFilter::reset()
{
    m_filteredDbm = 0.0;
    m_hasFilteredSample = false;
    m_entrySamples = 0;
    m_lastExitQualifyingSampleMs = 0;
    m_lastValidSampleMs = 0;
    m_near = false;
    m_reconfigurationGrace = false;
    m_reconfigurationStartedMs = 0;
    m_reconfigurationDurationMs = 0;
}

void ProximitySignalFilter::addSample(int rssiDbm, qint64 monotonicMs)
{
    if (rssiDbm == kInvalidCoreBluetoothRssi) {
        return;
    }

    if (m_reconfigurationGrace &&
        !isReconfigurationGrace(monotonicMs)) {
        m_reconfigurationGrace = false;
    }

    if (m_near && !isNear(monotonicMs)) {
        // A completed departure starts a new observation epoch. Retaining the
        // old strong EWMA here could let weak samples inherit enough signal
        // to satisfy the cold-entry counter after the client already left.
        reset();
    }

    if (!m_hasFilteredSample) {
        m_filteredDbm = rssiDbm;
        m_hasFilteredSample = true;
    }
    else {
        m_filteredDbm = kSampleWeight * rssiDbm +
                        (1.0 - kSampleWeight) * m_filteredDbm;
    }
    m_lastValidSampleMs = monotonicMs;

    if (!m_near) {
        if (m_filteredDbm >= m_policy.connectDbm) {
            ++m_entrySamples;
            if (m_entrySamples >= kRequiredEntrySamples) {
                m_near = true;
                m_lastExitQualifyingSampleMs = monotonicMs;
                m_reconfigurationGrace = false;
            }
        }
        else {
            m_entrySamples = 0;
        }
        return;
    }

    if (m_filteredDbm > m_policy.departureDbm &&
        monotonicMs > m_lastExitQualifyingSampleMs) {
        m_lastExitQualifyingSampleMs = monotonicMs;
    }
}

bool ProximitySignalFilter::filteredDbm(double& value) const
{
    if (!m_hasFilteredSample) {
        return false;
    }
    value = m_filteredDbm;
    return true;
}

bool ProximitySignalFilter::isNear(qint64 monotonicMs) const
{
    if (!m_near || monotonicMs < m_lastExitQualifyingSampleMs) {
        return m_near;
    }
    const quint64 elapsed = static_cast<quint64>(monotonicMs) -
                            static_cast<quint64>(
                                m_lastExitQualifyingSampleMs);
    return elapsed < quint64(kDepartureTimeoutMs);
}

bool ProximitySignalFilter::isReconfigurationGrace(
    qint64 monotonicMs) const
{
    if (!m_reconfigurationGrace ||
        monotonicMs < m_reconfigurationStartedMs) {
        return m_reconfigurationGrace;
    }
    return elapsedSince(monotonicMs, m_reconfigurationStartedMs) <
           m_reconfigurationDurationMs;
}

bool ProximitySignalFilter::isDepartureGrace(qint64 monotonicMs) const
{
    if (!isNear(monotonicMs) || !m_hasFilteredSample ||
        monotonicMs < m_lastValidSampleMs) {
        return false;
    }

    return m_filteredDbm <= m_policy.departureDbm ||
           elapsedSince(monotonicMs, m_lastValidSampleMs) >=
               static_cast<quint64>(kSignalFreshnessMs);
}

bool ProximitySignalFilter::isDepartureWarning(qint64 monotonicMs) const
{
    if (!isDepartureGrace(monotonicMs) ||
        monotonicMs < m_lastExitQualifyingSampleMs) {
        return false;
    }

    // Keep the launch gate fail-closed as soon as grace begins, but avoid
    // flashing a disconnect warning for one weak sample or an ordinary
    // advertisement gap after a laptop lid changes antenna orientation.
    return elapsedSince(monotonicMs, m_lastExitQualifyingSampleMs) >=
           static_cast<quint64>(kDepartureWarningDelayMs);
}

} // namespace barrier
