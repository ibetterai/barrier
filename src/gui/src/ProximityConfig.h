/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "ProximitySignalFilter.h"

#include <QMap>
#include <QString>
#include <QUuid>

class QSettings;
class QVariant;

namespace barrier {

struct ProximityPairing {
    QString proximityId;
    QUuid peripheralId;
    QString displayName;
    ProximityThresholdPolicy thresholdPolicy;
    QString clientRoutingId;
    bool signalSharingEnabled{false};
};

enum class ProximityConfigApplyResult {
    Unchanged,
    Changed,
    Error
};

enum class ClientPresenceAssociationResult {
    Unchanged,
    Changed,
    Conflict,
    Error
};

struct StoredClientPresenceAssociation {
    QString screenName;
    QString serverProximityId;
};

class ProximityConfig {
public:
    explicit ProximityConfig(QSettings& settings);

    bool serverAdvertiserEnabled() const { return m_serverAdvertiserEnabled; }
    bool clientGatingEnabled() const { return m_clientGatingEnabled; }
    bool setServerAdvertiserEnabled(bool enabled);
    bool setClientGatingEnabled(bool enabled);

    QString serverProximityId(QString* error = nullptr);
    bool hasServerProximityId() const
    {
        return !m_serverProximityId.isEmpty();
    }
    bool resetServerProximityId(QString* error = nullptr);

    bool pairing(ProximityPairing& pairing) const;
    bool replacePairing(const ProximityPairing& pairing,
                        QString* error = nullptr);
    bool clearPairing(QString* error = nullptr);
    ProximityConfigApplyResult applyClientSettings(
        bool gatingEnabled,
        const QString& expectedProximityId,
        const QUuid& expectedPeripheralId,
        const ProximityThresholdPolicy& thresholdPolicy,
        QString* error = nullptr);
    ProximityConfigApplyResult applyClientSettings(
        bool gatingEnabled,
        bool signalSharingEnabled,
        const QString& expectedProximityId,
        const QUuid& expectedPeripheralId,
        const ProximityThresholdPolicy& thresholdPolicy,
        QString* error = nullptr);

    QMap<QString, QString> associatedClientPresenceRoutes(
        const QString& serverProximityId) const;
    ClientPresenceAssociationResult associateClientPresenceRoute(
        const QString& screenName,
        const QString& clientRoutingId,
        const QString& serverProximityId,
        QString* error = nullptr);

    const QString& error() const { return m_error; }

private:
    bool writeScalar(const QString& key, const QVariant& value);
    void setError(const QString& message, QString* error = nullptr);

    QSettings& m_settings;
    bool m_serverAdvertiserEnabled{false};
    bool m_clientGatingEnabled{false};
    QString m_serverProximityId;
    bool m_serverProximityIdMalformed{false};
    bool m_hasPairing{false};
    bool m_pairingClientPolicyRecovered{false};
    ProximityPairing m_pairing;
    QMap<QString, StoredClientPresenceAssociation>
        m_associatedClientPresenceRoutes;
    QString m_error;
};

} // namespace barrier
