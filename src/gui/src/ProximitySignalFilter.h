/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include <QtGlobal>

namespace barrier {

constexpr int kProximityEnterThresholdDbm = -75;
constexpr int kProximityExitThresholdDbm = -90;
constexpr int kProximityMinimumThresholdDbm = -100;
constexpr int kProximityMaximumThresholdDbm = -30;
constexpr int kProximityMinimumHysteresisDb = 15;

struct ProximityThresholdPolicy {
    int connectDbm{kProximityEnterThresholdDbm};
    int departureDbm{kProximityExitThresholdDbm};

    bool isValid() const;
};

bool operator==(const ProximityThresholdPolicy& left,
                const ProximityThresholdPolicy& right);
bool operator!=(const ProximityThresholdPolicy& left,
                const ProximityThresholdPolicy& right);

class ProximitySignalFilter {
public:
    explicit ProximitySignalFilter(
        const ProximityThresholdPolicy& policy =
            ProximityThresholdPolicy());

    void reset();
    bool reconfigure(const ProximityThresholdPolicy& policy,
                     qint64 monotonicMs);
    void addSample(int rssiDbm, qint64 monotonicMs);
    bool isNear(qint64 monotonicMs) const;
    bool isReconfigurationGrace(qint64 monotonicMs) const;
    bool isDepartureGrace(qint64 monotonicMs) const;
    bool isDepartureWarning(qint64 monotonicMs) const;
    bool filteredDbm(double& value) const;

private:
    ProximityThresholdPolicy m_policy;
    double m_filteredDbm;
    bool m_hasFilteredSample;
    int m_entrySamples;
    qint64 m_lastExitQualifyingSampleMs;
    qint64 m_lastValidSampleMs;
    bool m_near;
    bool m_reconfigurationGrace;
    qint64 m_reconfigurationStartedMs;
    quint64 m_reconfigurationDurationMs;
};

} // namespace barrier
