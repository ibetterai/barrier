/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include <QObject>
#include <QString>
#include <QUuid>

class MacProximityControllerPrivate;

class MacProximityController : public QObject
{
    Q_OBJECT

public:
    enum class BluetoothState {
        Unknown,
        Unauthorized,
        PoweredOff,
        PoweredOn,
        Failed
    };
    Q_ENUM(BluetoothState)

    explicit MacProximityController(QObject* parent = nullptr);
    ~MacProximityController() override;

    void startAdvertising(const QString& proximityId);
    void stopAdvertising();
    void startClientPresenceAdvertising(const QString& clientProximityId);
    void stopClientPresenceAdvertising();
    virtual void startScanning();
    virtual void stopScanning();
    virtual void startClientPresenceScanning();
    virtual void stopClientPresenceScanning();
    virtual void readPairingIdentity(const QUuid& peripheralId);

signals:
    // bluetoothStateChanged is retained for the existing client-gating path.
    // It mirrors centralBluetoothStateChanged only; peripheral-manager state
    // must never make a working scanner look unavailable.
    void bluetoothStateChanged(BluetoothState state);
    void centralBluetoothStateChanged(BluetoothState state);
    void peripheralBluetoothStateChanged(BluetoothState state);
    void peripheralObserved(QUuid peripheralId, QString name, int rssiDbm);
    void pairingIdentityRead(QUuid peripheralId, QString proximityId);
    void clientPeripheralObserved(QUuid peripheralId, int rssiDbm);
    void clientPresenceIdentityRead(
        QUuid peripheralId, QString clientProximityId);
    void clientPresenceIdentityFailed(
        QUuid peripheralId, QString userMessage);
    // Emitted whenever a scan starts over with a new resolver generation.
    // Consumers must discard peripheral-to-routing-ID and RSSI associations.
    void clientPresenceScanSessionReset(quint64 generation);
    void operationFailed(QString operation, QString userMessage);

private:
    friend class MacProximityControllerPrivate;
    MacProximityControllerPrivate* m_private;
    int m_scanRequestCount{0};
    int m_clientPresenceScanRequestCount{0};
};
