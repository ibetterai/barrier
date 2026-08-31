/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "../src/ProximitySettingsDialog.h"

#include <gtest/gtest.h>

#include <QListWidget>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTreeWidget>

namespace {

const QString kProximityId =
    QStringLiteral("00112233445566778899aabbccddeeff");

class FakeMacProximityController : public MacProximityController
{
public:
    void startScanning() override
    {
        ++startScanningCalls;
    }

    void stopScanning() override
    {
        ++stopScanningCalls;
    }

    void readPairingIdentity(const QUuid& peripheralId) override
    {
        ++identityReadCalls;
        emit pairingIdentityRead(peripheralId, kProximityId);
    }

    void startClientPresenceScanning() override
    {
        ++startClientPresenceScanningCalls;
    }

    void stopClientPresenceScanning() override
    {
        ++stopClientPresenceScanningCalls;
    }

    int startScanningCalls{0};
    int stopScanningCalls{0};
    int identityReadCalls{0};
    int startClientPresenceScanningCalls{0};
    int stopClientPresenceScanningCalls{0};
};

class FakeProximityPairingBrowser : public ProximityPairingBrowser
{
public:
    void browseForType(const QString& type) override
    {
        ++browseCalls;
        browsedType = type;
        if (failOnBrowse) {
            emit browseFailed();
        }
    }

    void publish(const QList<ZeroconfRecord>& records)
    {
        emit recordsChanged(records);
    }

    int browseCalls{0};
    QString browsedType;
    bool failOnBrowse{false};
};

ZeroconfRecord matchingServer()
{
    ZeroconfRecord record(
        QStringLiteral("Desk server"),
        QStringLiteral("_barrierServerZeroconf._tcp"),
        QStringLiteral("local."));
    record.hostName = QStringLiteral("desk.local.");
    record.port = 49165;
    record.txt.insert(QStringLiteral("proximity-id"),
                      kProximityId.toLatin1());
    return record;
}

} // namespace

TEST(ProximitySettingsDialogTests,
     discoversPairingServerWhenMainAutoConfigBrowserIsAbsent)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("barrier.ini")),
                       QSettings::IniFormat);
    barrier::ProximityConfig config(settings);
    FakeMacProximityController controller;
    FakeProximityPairingBrowser pairingBrowser;
    const QList<ZeroconfRecord> noMainWindowRecords;

    {
        ProximitySettingsDialog dialog(
            nullptr, false, config, controller, noMainWindowRecords,
            &pairingBrowser);

        EXPECT_EQ(controller.startScanningCalls, 1);
        EXPECT_EQ(pairingBrowser.browseCalls, 1);
        EXPECT_EQ(pairingBrowser.browsedType,
                  QStringLiteral("_barrierServerZeroconf._tcp"));

        const QUuid peripheralId = QUuid::createUuid();
        emit controller.peripheralObserved(
            peripheralId, QStringLiteral("Nearby Barrier server"), -48);

        EXPECT_EQ(controller.identityReadCalls, 1);
        EXPECT_EQ(dialog.nearbyServersList->count(), 0);

        const ZeroconfRecord matching = matchingServer();
        ZeroconfRecord unrelated = matching;
        unrelated.serviceName = QStringLiteral("Other server");
        unrelated.txt[QStringLiteral("proximity-id")] =
            QByteArrayLiteral("ffeeddccbbaa99887766554433221100");
        pairingBrowser.publish({unrelated, matching, matching});

        ASSERT_EQ(dialog.nearbyServersList->count(), 1);
        EXPECT_EQ(
            dialog.nearbyServersList->item(0)->data(Qt::UserRole).toUuid(),
            peripheralId);
        EXPECT_TRUE(dialog.nearbyServersList->item(0)->text().contains(
            QStringLiteral("Desk server")));

        pairingBrowser.publish({matching, unrelated});

        EXPECT_EQ(dialog.nearbyServersList->count(), 1);

        pairingBrowser.publish({});

        EXPECT_EQ(dialog.nearbyServersList->count(), 0);
    }

    EXPECT_EQ(controller.stopScanningCalls, 1);
}

TEST(ProximitySettingsDialogTests, surfacesPairingBrowserFailure)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("barrier.ini")),
                       QSettings::IniFormat);
    barrier::ProximityConfig config(settings);
    FakeMacProximityController controller;
    FakeProximityPairingBrowser pairingBrowser;
    pairingBrowser.failOnBrowse = true;
    const QList<ZeroconfRecord> noMainWindowRecords;

    ProximitySettingsDialog dialog(
        nullptr, false, config, controller, noMainWindowRecords,
        &pairingBrowser);

    EXPECT_TRUE(dialog.pairingStatusLabel->text().contains(
        QStringLiteral("could not browse")));
}

TEST(ProximitySettingsDialogTests, pairPersistsImmediatelyWithoutCalibration)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("barrier.ini")),
                       QSettings::IniFormat);
    barrier::ProximityConfig config(settings);
    FakeMacProximityController controller;
    FakeProximityPairingBrowser pairingBrowser;
    ProximitySettingsDialog dialog(
        nullptr, false, config, controller, {}, &pairingBrowser);

    const QUuid peripheralId = QUuid::createUuid();
    emit controller.peripheralObserved(
        peripheralId, QStringLiteral("Nearby Barrier server"), -80);
    pairingBrowser.publish({matchingServer()});
    ASSERT_EQ(1, dialog.nearbyServersList->count());
    dialog.nearbyServersList->setCurrentRow(0);
    dialog.pairButton->click();

    barrier::ProximityPairing paired;
    ASSERT_TRUE(config.pairing(paired));
    EXPECT_EQ(kProximityId, paired.proximityId);
    EXPECT_EQ(peripheralId, paired.peripheralId);
    EXPECT_EQ(QString::fromLatin1("Desk server"), paired.displayName);
    EXPECT_TRUE(dialog.pairingStatusLabel->text().contains(
        QStringLiteral("Pairing saved")));
}

TEST(ProximitySettingsDialogTests, savesValidThresholdsOnce)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("barrier.ini")),
                       QSettings::IniFormat);
    barrier::ProximityConfig config(settings);
    const QUuid peripheralId = QUuid::createUuid();
    ASSERT_TRUE(config.replacePairing({
        kProximityId, peripheralId, QStringLiteral("Desk server")}));
    FakeMacProximityController controller;
    FakeProximityPairingBrowser pairingBrowser;
    ProximitySettingsDialog dialog(
        nullptr, false, config, controller, {}, &pairingBrowser);
    int changeCount = 0;
    QObject::connect(
        &dialog, &ProximitySettingsDialog::configurationChanged,
        [&changeCount]() { ++changeCount; });

    EXPECT_EQ(-75, dialog.connectDbmSpinBox->value());
    EXPECT_EQ(-90, dialog.departureDbmSpinBox->value());
    dialog.connectDbmSpinBox->setValue(-65);
    dialog.departureDbmSpinBox->setValue(-85);
    dialog.signalSharingCheckBox->setChecked(true);
    ASSERT_TRUE(dialog.buttonBox->button(QDialogButtonBox::Ok)->isEnabled());
    dialog.accept();

    barrier::ProximityPairing loaded;
    ASSERT_TRUE(config.pairing(loaded));
    EXPECT_EQ((barrier::ProximityThresholdPolicy{-65, -85}),
              loaded.thresholdPolicy);
    EXPECT_TRUE(loaded.signalSharingEnabled);
    EXPECT_EQ(1, changeCount);
    EXPECT_EQ(QDialog::Accepted, dialog.result());
}

TEST(ProximitySettingsDialogTests, serverShowsAssociatedClientSignal)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("barrier.ini")),
                       QSettings::IniFormat);
    barrier::ProximityConfig config(settings);
    QString error;
    const QString serverId = config.serverProximityId(&error);
    ASSERT_TRUE(error.isEmpty());
    const QString routingId =
        QStringLiteral("11112222333344445555666677778888");
    ASSERT_EQ(barrier::ClientPresenceAssociationResult::Changed,
              config.associateClientPresenceRoute(
                  QStringLiteral("client-alpha"), routingId, serverId, &error));
    FakeMacProximityController controller;

    {
        ProximitySettingsDialog dialog(
            nullptr, true, config, controller, {});
        EXPECT_EQ(1, controller.startClientPresenceScanningCalls);

        const QUuid peripheralId = QUuid::createUuid();
        emit controller.clientPeripheralObserved(peripheralId, -48);
        emit controller.clientPresenceIdentityRead(
            peripheralId, routingId);

        ASSERT_EQ(1, dialog.nearbyClientsTree->topLevelItemCount());
        QTreeWidgetItem* row =
            dialog.nearbyClientsTree->topLevelItem(0);
        EXPECT_EQ(QString::fromLatin1("client-alpha"), row->text(0));
        EXPECT_EQ(QString::fromLatin1("-48 dBm"), row->text(1));
        EXPECT_EQ(QString::fromLatin1("Nearby"), row->text(2));
        for (int column = 0; column < row->columnCount(); ++column) {
            EXPECT_FALSE(row->text(column).contains(routingId));
        }

        emit controller.clientPresenceScanSessionReset(2);
        ASSERT_EQ(1, dialog.nearbyClientsTree->topLevelItemCount());
        row = dialog.nearbyClientsTree->topLevelItem(0);
        EXPECT_EQ(QString::fromLatin1("Unavailable"), row->text(1));
        EXPECT_EQ(QString::fromLatin1("Not observed"), row->text(2));

        emit controller.clientPeripheralObserved(peripheralId, -42);
        EXPECT_EQ(QString::fromLatin1("Unavailable"), row->text(1));
        emit controller.clientPresenceIdentityRead(
            peripheralId, routingId);
        EXPECT_EQ(QString::fromLatin1("-42 dBm"), row->text(1));
    }

    EXPECT_EQ(1, controller.stopClientPresenceScanningCalls);
}

TEST(ProximitySettingsDialogTests,
     routeRotationRestartsResolutionBeforeAttributingSignal)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("barrier.ini")),
                       QSettings::IniFormat);
    barrier::ProximityConfig config(settings);
    QString error;
    const QString serverId = config.serverProximityId(&error);
    ASSERT_TRUE(error.isEmpty());
    const QString oldRoutingId =
        QStringLiteral("11112222333344445555666677778888");
    const QString newRoutingId =
        QStringLiteral("9999aaaabbbbccccddddeeeeffff0000");
    ASSERT_EQ(barrier::ClientPresenceAssociationResult::Changed,
              config.associateClientPresenceRoute(
                  QStringLiteral("client-alpha"), oldRoutingId,
                  serverId, &error));
    FakeMacProximityController controller;
    ProximitySettingsDialog dialog(
        nullptr, true, config, controller, {});
    const QUuid peripheralId = QUuid::createUuid();
    emit controller.clientPeripheralObserved(peripheralId, -48);
    emit controller.clientPresenceIdentityRead(
        peripheralId, oldRoutingId);

    ASSERT_EQ(barrier::ClientPresenceAssociationResult::Changed,
              config.associateClientPresenceRoute(
                  QStringLiteral("client-alpha"), newRoutingId,
                  serverId, &error));
    ASSERT_TRUE(QMetaObject::invokeMethod(
        &dialog, "refreshNearbyClients", Qt::DirectConnection));

    EXPECT_EQ(2, controller.startClientPresenceScanningCalls);
    EXPECT_EQ(1, controller.stopClientPresenceScanningCalls);
    ASSERT_EQ(1, dialog.nearbyClientsTree->topLevelItemCount());
    QTreeWidgetItem* row =
        dialog.nearbyClientsTree->topLevelItem(0);
    EXPECT_EQ(QString::fromLatin1("Unavailable"), row->text(1));

    emit controller.clientPeripheralObserved(peripheralId, -42);
    EXPECT_EQ(QString::fromLatin1("Unavailable"), row->text(1));
    emit controller.clientPresenceIdentityRead(
        peripheralId, newRoutingId);
    EXPECT_EQ(QString::fromLatin1("-42 dBm"), row->text(1));
}

TEST(ProximitySettingsDialogTests, noOpAcceptEmitsNoChange)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("barrier.ini")),
                       QSettings::IniFormat);
    barrier::ProximityConfig config(settings);
    ASSERT_TRUE(config.replacePairing({
        kProximityId, QUuid::createUuid(), QStringLiteral("Desk server")}));
    FakeMacProximityController controller;
    FakeProximityPairingBrowser pairingBrowser;
    ProximitySettingsDialog dialog(
        nullptr, false, config, controller, {}, &pairingBrowser);
    int changeCount = 0;
    QObject::connect(
        &dialog, &ProximitySettingsDialog::configurationChanged,
        [&changeCount]() { ++changeCount; });

    dialog.accept();

    EXPECT_EQ(0, changeCount);
    EXPECT_EQ(QDialog::Accepted, dialog.result());
}

TEST(ProximitySettingsDialogTests, invalidGapCannotBeAccepted)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("barrier.ini")),
                       QSettings::IniFormat);
    barrier::ProximityConfig config(settings);
    ASSERT_TRUE(config.replacePairing({
        kProximityId, QUuid::createUuid(), QStringLiteral("Desk server")}));
    FakeMacProximityController controller;
    FakeProximityPairingBrowser pairingBrowser;
    ProximitySettingsDialog dialog(
        nullptr, false, config, controller, {}, &pairingBrowser);

    dialog.connectDbmSpinBox->setValue(-80);
    dialog.departureDbmSpinBox->setValue(-90);

    EXPECT_FALSE(dialog.buttonBox->button(QDialogButtonBox::Ok)->isEnabled());
    EXPECT_TRUE(dialog.thresholdValidationLabel->text().contains(
        QStringLiteral("15 dB")));
    dialog.accept();
    EXPECT_NE(QDialog::Accepted, dialog.result());
}
