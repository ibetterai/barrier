/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "MacProximityController.h"
#include "ClientPresenceRegistry.h"
#include "ProximityConfig.h"
#include "ProximitySignalFilter.h"
#include "ZeroconfRecord.h"
#include "ui_ProximitySettingsDialogBase.h"

#include <QDialog>
#include <QElapsedTimer>
#include <QMap>
#include <QTimer>

#include <memory>

class ProximityPairingBrowser : public QObject
{
    Q_OBJECT

public:
    explicit ProximityPairingBrowser(QObject* parent = nullptr) :
        QObject(parent)
    {
    }
    ~ProximityPairingBrowser() override = default;

    virtual void browseForType(const QString& type) = 0;

signals:
    void recordsChanged(const QList<ZeroconfRecord>& records);
    void browseFailed();
};

class ProximitySettingsDialog : public QDialog,
                                public Ui::ProximitySettingsDialogBase
{
    Q_OBJECT

public:
    ProximitySettingsDialog(
        QWidget* parent,
        bool serverMode,
        barrier::ProximityConfig& config,
        MacProximityController& controller,
        const QList<ZeroconfRecord>& servers,
        ProximityPairingBrowser* pairingBrowser = nullptr);
    ~ProximitySettingsDialog() override;

public slots:
    void accept() override;
    void bonjourServersChanged();

signals:
    void configurationChanged();

private slots:
    void peripheralObserved(QUuid peripheralId, QString name, int rssiDbm);
    void pairingIdentityRead(QUuid peripheralId, QString proximityId);
    void pairingServersChanged(const QList<ZeroconfRecord>& servers);
    void pairingBrowseFailed();
    void operationFailed(QString operation, QString userMessage);
    void pairSelectedServer();
    void forgetPairing();
    void resetServerIdentity();
    void resetThresholds();
    void updateThresholdValidation();
    void clientPeripheralObserved(QUuid peripheralId, int rssiDbm);
    void clientPresenceIdentityRead(QUuid peripheralId, QString routingId);
    void clientPresenceIdentityFailed(QUuid peripheralId, QString message);
    void clientPresenceScanSessionReset(quint64 generation);
    void refreshNearbyClients();

private:
    struct NearbyPeripheral {
        QString name;
        QString proximityId;
        QString displayName;
        int lastRssiDbm{127};
        bool identityPending{false};
    };

    struct ClientPresenceObservation {
        int rssiDbm{127};
        qint64 monotonicMs{0};
    };

    void refreshPairingStatus();
    void clearClientPresenceScanSession();
    const ZeroconfRecord* matchingServer(const QString& proximityId) const;
    void refreshNearbyServers();
    barrier::ProximityThresholdPolicy thresholdDraft() const;

    bool m_serverMode;
    barrier::ProximityConfig& m_config;
    MacProximityController& m_controller;
    const QList<ZeroconfRecord>& m_servers;
    QList<ZeroconfRecord> m_pairingServers;
    ProximityPairingBrowser* m_pairingBrowser;
    QMap<QUuid, NearbyPeripheral> m_nearby;
    QElapsedTimer m_monotonicClock;
    std::unique_ptr<barrier::ProximitySignalFilter> m_signalFilter;
    barrier::ClientPresenceRegistry m_clientPresenceRegistry;
    QMap<QString, QString> m_clientPresenceRoutes;
    QMap<QUuid, ClientPresenceObservation> m_clientPresenceObservations;
    QMap<QUuid, QString> m_clientPresenceRoutingIds;
    QTimer m_clientPresenceRefreshTimer;
    bool m_clientPresenceScanActive{false};
};
