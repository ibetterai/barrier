/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "ProximityConfig.h"
#include "ProximitySignalFilter.h"
#include <QLockFile>

#include <QMap>
#include <QSettings>
#include <QVariant>

namespace barrier {
namespace {

const QString kProximityGroup = QStringLiteral("proximity");
const QString kPairingGroup = QStringLiteral("proximity/pairing");
const QString kAssociatedClientsGroup =
    QStringLiteral("proximity/associatedClients");


bool isValidProximityId(const QString& id)
{
    if (id.size() != 32) {
        return false;
    }
    for (const QChar character : id) {
        const bool asciiDigit =
            character >= QLatin1Char('0') &&
            character <= QLatin1Char('9');
        const bool lowercaseHex =
            character >= QLatin1Char('a') &&
            character <= QLatin1Char('f');
        if (!asciiDigit && !lowercaseHex) {
            return false;
        }
    }
    return true;
}

QString generateOpaqueId()
{
    return QString::fromLatin1(QUuid::createUuid().toRfc4122().toHex());
}

bool isValidPairing(const ProximityPairing& pairing)
{
    return isValidProximityId(pairing.proximityId) &&
           !pairing.peripheralId.isNull() &&
           pairing.displayName.size() <= 256;
}

using SettingsGroup = QMap<QString, QVariant>;

SettingsGroup snapshotGroup(QSettings& settings, const QString& group)
{
    SettingsGroup snapshot;
    settings.beginGroup(group);
    for (const QString& key : settings.allKeys()) {
        snapshot.insert(key, settings.value(key));
    }
    settings.endGroup();
    return snapshot;
}

void restoreGroup(QSettings& settings,
                  const QString& group,
                  const SettingsGroup& snapshot)
{
    settings.beginGroup(group);
    settings.remove(QString());
    for (auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it) {
        settings.setValue(it.key(), it.value());
    }
    settings.endGroup();
}

void writePairingGroup(QSettings& settings,
                       const QString& group,
                       const ProximityPairing& pairing)
{
    settings.beginGroup(group);
    settings.remove(QString());
    settings.setValue(QStringLiteral("proximityId"), pairing.proximityId);
    settings.setValue(QStringLiteral("peripheralId"),
                      pairing.peripheralId.toString(QUuid::WithoutBraces));
    settings.setValue(QStringLiteral("displayName"), pairing.displayName);
    // These values are ignored by this build. Keep writing the fixed policy
    // so a rollback to the earlier 3.4.0 pairing schema remains readable.
    settings.setValue(QStringLiteral("enterDbm"),
                      kProximityEnterThresholdDbm);
    settings.setValue(QStringLiteral("exitDbm"),
                      kProximityExitThresholdDbm);
    settings.setValue(QStringLiteral("thresholdPolicyVersion"), 1);
    settings.setValue(QStringLiteral("connectDbm"),
                      pairing.thresholdPolicy.connectDbm);
    settings.setValue(QStringLiteral("departureDbm"),
                      pairing.thresholdPolicy.departureDbm);
    if (!pairing.clientRoutingId.isEmpty()) {
        settings.setValue(QStringLiteral("clientRoutingId"),
                          pairing.clientRoutingId);
    }
    settings.setValue(QStringLiteral("signalSharingEnabled"),
                      pairing.signalSharingEnabled);
    settings.endGroup();
}

bool readPairingGroup(QSettings& settings,
                      const QString& group,
                      ProximityPairing& pairing,
                      bool& present,
                      bool& clientPolicyRecovered)
{
    clientPolicyRecovered = false;
    settings.beginGroup(group);
    present = !settings.allKeys().isEmpty();
    if (!present) {
        settings.endGroup();
        return false;
    }

    const QStringList requiredKeys{
        QStringLiteral("proximityId"),
        QStringLiteral("peripheralId"),
        QStringLiteral("displayName")
    };
    for (const QString& key : requiredKeys) {
        if (!settings.contains(key)) {
            settings.endGroup();
            return false;
        }
    }

    ProximityPairing candidate;
    candidate.proximityId = settings.value(QStringLiteral("proximityId")).toString();
    candidate.peripheralId = QUuid(
        settings.value(QStringLiteral("peripheralId")).toString());
    candidate.displayName = settings.value(QStringLiteral("displayName")).toString();
    if (settings.contains(QStringLiteral("thresholdPolicyVersion"))) {
        bool versionOk = false;
        bool connectOk = false;
        bool departureOk = false;
        const int version = settings.value(
            QStringLiteral("thresholdPolicyVersion")).toInt(&versionOk);
        const int connectDbm = settings.value(
            QStringLiteral("connectDbm")).toInt(&connectOk);
        const int departureDbm = settings.value(
            QStringLiteral("departureDbm")).toInt(&departureOk);
        const ProximityThresholdPolicy policy{connectDbm, departureDbm};
        if (versionOk && version == 1 && connectOk && departureOk &&
            policy.isValid()) {
            candidate.thresholdPolicy = policy;
        }
        else {
            clientPolicyRecovered = true;
        }
    }
    if (settings.contains(QStringLiteral("clientRoutingId"))) {
        candidate.clientRoutingId = settings.value(
            QStringLiteral("clientRoutingId")).toString();
        if (!isValidProximityId(candidate.clientRoutingId)) {
            candidate.clientRoutingId.clear();
            clientPolicyRecovered = true;
        }
    }
    candidate.signalSharingEnabled = settings.value(
        QStringLiteral("signalSharingEnabled"), false).toBool();
    if (candidate.signalSharingEnabled &&
        candidate.clientRoutingId.isEmpty()) {
        candidate.signalSharingEnabled = false;
        clientPolicyRecovered = true;
    }
    settings.endGroup();

    if (!isValidPairing(candidate)) {
        return false;
    }
    pairing = candidate;
    return true;
}

} // namespace

ProximityConfig::ProximityConfig(QSettings& settings) :
    m_settings(settings)
{
    m_settings.setAtomicSyncRequired(true);
    m_settings.beginGroup(kProximityGroup);
    m_serverAdvertiserEnabled =
        m_settings.value(QStringLiteral("serverAdvertiserEnabled"), false).toBool();
    m_clientGatingEnabled =
        m_settings.value(QStringLiteral("clientGatingEnabled"), false).toBool();
    m_serverProximityId =
        m_settings.value(QStringLiteral("serverProximityId")).toString();
    m_settings.endGroup();

    if (!m_serverProximityId.isEmpty() &&
        !isValidProximityId(m_serverProximityId)) {
        m_serverProximityIdMalformed = true;
        m_serverProximityId.clear();
        m_error = QStringLiteral("Stored server proximity ID is malformed.");
    }

    bool pairingPresent = false;
    m_hasPairing = readPairingGroup(
        m_settings, kPairingGroup, m_pairing, pairingPresent,
        m_pairingClientPolicyRecovered);
    if (pairingPresent && !m_hasPairing) {
        m_error = QStringLiteral("Stored proximity pairing is malformed.");
    }
    else if (m_pairingClientPolicyRecovered) {
        m_error = QStringLiteral(
            "Stored proximity client policy was invalid; safe defaults are active.");
    }


    m_settings.beginGroup(kAssociatedClientsGroup);
    for (const QString& routingId : m_settings.childGroups()) {
        if (!isValidProximityId(routingId)) {
            continue;
        }
        m_settings.beginGroup(routingId);
        const StoredClientPresenceAssociation association{
            m_settings.value(QStringLiteral("screenName")).toString(),
            m_settings.value(QStringLiteral("serverProximityId")).toString()
        };
        m_settings.endGroup();
        if (!association.screenName.isEmpty() &&
            association.screenName.size() <= 256 &&
            isValidProximityId(association.serverProximityId)) {
            m_associatedClientPresenceRoutes.insert(routingId, association);
        }
    }
    m_settings.endGroup();
}

bool ProximityConfig::writeScalar(const QString& key, const QVariant& value)
{
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError) {
        m_error = QStringLiteral("Unable to read proximity settings.");
        return false;
    }
    const QString path = kProximityGroup + QLatin1Char('/') + key;
    const bool existed = m_settings.contains(path);
    const QVariant previous = m_settings.value(path);

    m_settings.setValue(path, value);
    m_settings.sync();
    if (m_settings.status() == QSettings::NoError) {
        m_error.clear();
        return true;
    }

    if (existed) {
        m_settings.setValue(path, previous);
    }
    else {
        m_settings.remove(path);
    }
    m_settings.sync();
    m_error = QStringLiteral("Unable to persist proximity settings.");
    return false;
}

bool ProximityConfig::setServerAdvertiserEnabled(bool enabled)
{
    if (enabled == m_serverAdvertiserEnabled) {
        m_error.clear();
        return true;
    }
    if (!writeScalar(QStringLiteral("serverAdvertiserEnabled"), enabled)) {
        return false;
    }
    m_serverAdvertiserEnabled = enabled;
    return true;
}

bool ProximityConfig::setClientGatingEnabled(bool enabled)
{
    if (enabled == m_clientGatingEnabled) {
        m_error.clear();
        return true;
    }
    if (!writeScalar(QStringLiteral("clientGatingEnabled"), enabled)) {
        return false;
    }
    m_clientGatingEnabled = enabled;
    return true;
}

QString ProximityConfig::serverProximityId(QString* error)
{
    if (!m_serverProximityId.isEmpty()) {
        if (error != nullptr) {
            error->clear();
        }
        return m_serverProximityId;
    }
    const QString settingsFile = m_settings.fileName();
    if (settingsFile.isEmpty()) {
        setError(QStringLiteral("Proximity settings have no backing file."),
                 error);
        return QString();
    }
    const QString lockPath = settingsFile +
                             QStringLiteral(".proximity-id.lock");
    QLockFile lock(lockPath);
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(5000)) {
        setError(QStringLiteral("Unable to lock proximity settings."), error);
        return QString();
    }
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError) {
        setError(QStringLiteral("Unable to read proximity settings."), error);
        return QString();
    }
    const QString stored = m_settings.value(
        kProximityGroup + QStringLiteral("/serverProximityId")).toString();
    if (!stored.isEmpty()) {
        if (!isValidProximityId(stored)) {
            m_serverProximityIdMalformed = true;
            setError(QStringLiteral("Stored server proximity ID is malformed."),
                     error);
            return QString();
        }
        m_serverProximityId = stored;
        m_serverProximityIdMalformed = false;
        m_error.clear();
        if (error != nullptr) {
            error->clear();
        }
        return m_serverProximityId;
    }
    if (m_serverProximityIdMalformed) {
        setError(QStringLiteral("Stored server proximity ID is malformed."),
                 error);
        return QString();
    }
    const QString generated =
        QString::fromLatin1(QUuid::createUuid().toRfc4122().toHex());
    if (!writeScalar(QStringLiteral("serverProximityId"), generated)) {
        if (error != nullptr) {
            *error = m_error;
        }
        return QString();
    }
    m_serverProximityId = generated;
    if (error != nullptr) {
        error->clear();
    }
    return m_serverProximityId;
}

bool ProximityConfig::resetServerProximityId(QString* error)
{
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError) {
        setError(QStringLiteral("Unable to read proximity settings."), error);
        return false;
    }
    const SettingsGroup previous = snapshotGroup(
        m_settings, kProximityGroup);
    m_settings.setValue(
        kProximityGroup + QStringLiteral("/serverProximityId"), QString());
    restoreGroup(m_settings, kAssociatedClientsGroup, SettingsGroup());
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError) {
        restoreGroup(m_settings, kProximityGroup, previous);
        m_settings.sync();
        setError(QStringLiteral(
            "Unable to reset the server proximity identity."), error);
        return false;
    }
    m_serverProximityId.clear();
    m_serverProximityIdMalformed = false;
    m_associatedClientPresenceRoutes.clear();
    m_error.clear();
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool ProximityConfig::pairing(ProximityPairing& pairing) const
{
    if (!m_hasPairing) {
        return false;
    }
    pairing = m_pairing;
    return true;
}

bool ProximityConfig::replacePairing(const ProximityPairing& pairing,
                                     QString* error)
{
    if (!isValidPairing(pairing)) {
        setError(QStringLiteral("Proximity pairing is invalid."), error);
        return false;
    }
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError) {
        setError(QStringLiteral("Unable to read proximity pairing."), error);
        return false;
    }
    const SettingsGroup previous = snapshotGroup(m_settings, kPairingGroup);
    ProximityPairing replacement = pairing;
    replacement.thresholdPolicy = ProximityThresholdPolicy();
    replacement.clientRoutingId = generateOpaqueId();
    replacement.signalSharingEnabled = false;
    writePairingGroup(m_settings, kPairingGroup, replacement);
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError) {
        restoreGroup(m_settings, kPairingGroup, previous);
        m_settings.sync();
        setError(QStringLiteral("Unable to persist proximity pairing."), error);
        return false;
    }
    m_pairing = replacement;
    m_hasPairing = true;
    m_pairingClientPolicyRecovered = false;
    m_error.clear();
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool ProximityConfig::clearPairing(QString* error)
{
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError) {
        setError(QStringLiteral("Unable to read proximity pairing."), error);
        return false;
    }
    const SettingsGroup previous = snapshotGroup(m_settings, kPairingGroup);
    if (previous.isEmpty()) {
        m_pairing = ProximityPairing();
        m_hasPairing = false;
        m_pairingClientPolicyRecovered = false;
        m_error.clear();
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }
    restoreGroup(m_settings, kPairingGroup, SettingsGroup());
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError) {
        restoreGroup(m_settings, kPairingGroup, previous);
        m_settings.sync();
        setError(QStringLiteral("Unable to clear proximity pairing."), error);
        return false;
    }
    m_pairing = ProximityPairing();
    m_hasPairing = false;
    m_pairingClientPolicyRecovered = false;
    m_error.clear();
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

ProximityConfigApplyResult ProximityConfig::applyClientSettings(
    bool gatingEnabled,
    const QString& expectedProximityId,
    const QUuid& expectedPeripheralId,
    const ProximityThresholdPolicy& thresholdPolicy,
    QString* error)
{
    return applyClientSettings(
        gatingEnabled, m_pairing.signalSharingEnabled,
        expectedProximityId, expectedPeripheralId, thresholdPolicy, error);
}

ProximityConfigApplyResult ProximityConfig::applyClientSettings(
    bool gatingEnabled,
    bool signalSharingEnabled,
    const QString& expectedProximityId,
    const QUuid& expectedPeripheralId,
    const ProximityThresholdPolicy& thresholdPolicy,
    QString* error)
{
    if (!thresholdPolicy.isValid()) {
        setError(QStringLiteral(
            "Connect signal must be at least 15 dB stronger than the departure signal."),
            error);
        return ProximityConfigApplyResult::Error;
    }

    m_settings.sync();
    if (m_settings.status() != QSettings::NoError) {
        setError(QStringLiteral("Unable to read proximity settings."), error);
        return ProximityConfigApplyResult::Error;
    }

    ProximityPairing persistedPairing;
    bool pairingPresent = false;
    bool policyRecovered = false;
    const bool hasPersistedPairing = readPairingGroup(
        m_settings, kPairingGroup, persistedPairing, pairingPresent,
        policyRecovered);
    if (!hasPersistedPairing ||
        persistedPairing.proximityId != expectedProximityId ||
        persistedPairing.peripheralId != expectedPeripheralId) {
        setError(QStringLiteral(
            "The proximity pairing changed. Reopen settings and try again."),
            error);
        return ProximityConfigApplyResult::Error;
    }

    if (signalSharingEnabled &&
        persistedPairing.clientRoutingId.isEmpty()) {
        persistedPairing.clientRoutingId = generateOpaqueId();
        policyRecovered = true;
    }

    const bool persistedGating = m_settings.value(
        kProximityGroup + QStringLiteral("/clientGatingEnabled"),
        false).toBool();
    if (!policyRecovered && persistedGating == gatingEnabled &&
        persistedPairing.thresholdPolicy == thresholdPolicy &&
        persistedPairing.signalSharingEnabled == signalSharingEnabled) {
        m_clientGatingEnabled = persistedGating;
        m_pairing = persistedPairing;
        m_pairingClientPolicyRecovered = false;
        m_error.clear();
        if (error != nullptr) {
            error->clear();
        }
        return ProximityConfigApplyResult::Unchanged;
    }

    const SettingsGroup previous = snapshotGroup(
        m_settings, kProximityGroup);
    persistedPairing.thresholdPolicy = thresholdPolicy;
    persistedPairing.signalSharingEnabled = signalSharingEnabled;
    m_settings.setValue(
        kProximityGroup + QStringLiteral("/clientGatingEnabled"),
        gatingEnabled);
    writePairingGroup(m_settings, kPairingGroup, persistedPairing);
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError) {
        restoreGroup(m_settings, kProximityGroup, previous);
        m_settings.sync();
        setError(QStringLiteral(
            "Unable to persist client proximity settings."), error);
        return ProximityConfigApplyResult::Error;
    }

    m_clientGatingEnabled = gatingEnabled;
    m_pairing = persistedPairing;
    m_pairingClientPolicyRecovered = false;
    m_error.clear();
    if (error != nullptr) {
        error->clear();
    }
    return ProximityConfigApplyResult::Changed;
}

QMap<QString, QString> ProximityConfig::associatedClientPresenceRoutes(
    const QString& serverProximityId) const
{
    QMap<QString, QString> routes;
    if (!isValidProximityId(serverProximityId)) {
        return routes;
    }
    for (auto route = m_associatedClientPresenceRoutes.constBegin();
         route != m_associatedClientPresenceRoutes.constEnd(); ++route) {
        if (route.value().serverProximityId == serverProximityId) {
            routes.insert(route.key(), route.value().screenName);
        }
    }
    return routes;
}

ClientPresenceAssociationResult
ProximityConfig::associateClientPresenceRoute(
    const QString& screenName,
    const QString& clientRoutingId,
    const QString& serverProximityId,
    QString* error)
{
    if (screenName.isEmpty() || screenName.size() > 256 ||
        !isValidProximityId(clientRoutingId) ||
        !isValidProximityId(serverProximityId) ||
        serverProximityId != m_serverProximityId) {
        setError(QStringLiteral(
            "The client presence association is invalid."), error);
        return ClientPresenceAssociationResult::Error;
    }

    const auto collision =
        m_associatedClientPresenceRoutes.constFind(clientRoutingId);
    if (collision != m_associatedClientPresenceRoutes.constEnd() &&
        (collision.value().screenName != screenName ||
         collision.value().serverProximityId != serverProximityId)) {
        setError(QStringLiteral(
            "The client presence route conflicts with another screen."),
            error);
        return ClientPresenceAssociationResult::Conflict;
    }

    QMap<QString, StoredClientPresenceAssociation> updated =
        m_associatedClientPresenceRoutes;
    bool changed = false;
    for (auto route = updated.begin(); route != updated.end();) {
        if (route.value().serverProximityId == serverProximityId &&
            route.value().screenName == screenName &&
            route.key() != clientRoutingId) {
            route = updated.erase(route);
            changed = true;
        }
        else {
            ++route;
        }
    }
    if (updated.value(clientRoutingId).screenName != screenName ||
        updated.value(clientRoutingId).serverProximityId !=
            serverProximityId) {
        updated.insert(clientRoutingId,
                       {screenName, serverProximityId});
        changed = true;
    }
    if (!changed) {
        m_error.clear();
        if (error != nullptr) {
            error->clear();
        }
        return ClientPresenceAssociationResult::Unchanged;
    }

    m_settings.sync();
    if (m_settings.status() != QSettings::NoError) {
        setError(QStringLiteral(
            "Unable to read client presence associations."), error);
        return ClientPresenceAssociationResult::Error;
    }
    const SettingsGroup previous = snapshotGroup(
        m_settings, kAssociatedClientsGroup);
    restoreGroup(m_settings, kAssociatedClientsGroup, SettingsGroup());
    m_settings.beginGroup(kAssociatedClientsGroup);
    for (auto route = updated.constBegin(); route != updated.constEnd();
         ++route) {
        m_settings.beginGroup(route.key());
        m_settings.setValue(QStringLiteral("screenName"),
                            route.value().screenName);
        m_settings.setValue(QStringLiteral("serverProximityId"),
                            route.value().serverProximityId);
        m_settings.endGroup();
    }
    m_settings.endGroup();
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError) {
        restoreGroup(m_settings, kAssociatedClientsGroup, previous);
        m_settings.sync();
        setError(QStringLiteral(
            "Unable to persist client presence association."), error);
        return ClientPresenceAssociationResult::Error;
    }

    m_associatedClientPresenceRoutes.swap(updated);
    m_error.clear();
    if (error != nullptr) {
        error->clear();
    }
    return ClientPresenceAssociationResult::Changed;
}

void ProximityConfig::setError(const QString& message, QString* error)
{
    m_error = message;
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace barrier
