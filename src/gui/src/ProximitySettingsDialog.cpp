/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "ProximitySettingsDialog.h"

#include "ZeroconfBrowser.h"

#include <QDialogButtonBox>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QSet>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVariant>

#include <cmath>

namespace {

const int kInvalidCoreBluetoothRssi = 127;
const char kBarrierServerServiceType[] =
    "_barrierServerZeroconf._tcp";

class ZeroconfProximityPairingBrowser final :
    public ProximityPairingBrowser
{
public:
    explicit ZeroconfProximityPairingBrowser(QObject* parent) :
        ProximityPairingBrowser(parent),
        m_browser(this)
    {
        connect(&m_browser,
                &ZeroconfBrowser::currentRecordsChanged,
                this,
                &ProximityPairingBrowser::recordsChanged);
        connect(&m_browser,
                &ZeroconfBrowser::error,
                this,
                [this](DNSServiceErrorType) { emit browseFailed(); });
    }

    void browseForType(const QString& type) override
    {
        m_browser.browseForType(type);
    }

private:
    ZeroconfBrowser m_browser;
};

QString peripheralLabel(const QString& name, int rssiDbm)
{
    const QString displayName = name.trimmed().isEmpty()
        ? QObject::tr("Nearby Barrier server")
        : name.trimmed();
    return QObject::tr("%1 (%2 dBm)").arg(displayName).arg(rssiDbm);
}

} // namespace

ProximitySettingsDialog::ProximitySettingsDialog(
    QWidget* parent,
    bool serverMode,
    barrier::ProximityConfig& config,
    MacProximityController& controller,
    const QList<ZeroconfRecord>& servers,
    ProximityPairingBrowser* pairingBrowser) :
    QDialog(parent),
    Ui::ProximitySettingsDialogBase(),
    m_serverMode(serverMode),
    m_config(config),
    m_controller(controller),
    m_servers(servers),
    m_pairingBrowser(pairingBrowser)
{
    setupUi(this);
    serverGroup->setVisible(serverMode);
    clientGroup->setVisible(!serverMode);
    advertisingCheckBox->setChecked(config.serverAdvertiserEnabled());
    gatingCheckBox->setChecked(config.clientGatingEnabled());
    serverIdentityStatusLabel->setText(config.hasServerProximityId()
        ? tr("A private pairing identity is ready.")
        : tr("A private pairing identity will be created when advertising starts."));
    resetIdentityButton->setEnabled(config.hasServerProximityId());

    connect(buttonBox, &QDialogButtonBox::accepted,
            this, &ProximitySettingsDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected,
            this, &ProximitySettingsDialog::reject);
    connect(pairButton, &QPushButton::clicked,
            this, &ProximitySettingsDialog::pairSelectedServer);
    connect(forgetButton, &QPushButton::clicked,
            this, &ProximitySettingsDialog::forgetPairing);
    connect(resetIdentityButton, &QPushButton::clicked,
            this, &ProximitySettingsDialog::resetServerIdentity);
    connect(resetThresholdsButton, &QPushButton::clicked,
            this, &ProximitySettingsDialog::resetThresholds);
    connect(connectDbmSpinBox,
            static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &ProximitySettingsDialog::updateThresholdValidation);
    connect(departureDbmSpinBox,
            static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &ProximitySettingsDialog::updateThresholdValidation);
    connect(&controller, &MacProximityController::peripheralObserved,
            this, &ProximitySettingsDialog::peripheralObserved);
    connect(&controller, &MacProximityController::pairingIdentityRead,
            this, &ProximitySettingsDialog::pairingIdentityRead);
    connect(&controller, &MacProximityController::operationFailed,
            this, &ProximitySettingsDialog::operationFailed);
    connect(&controller,
            &MacProximityController::clientPeripheralObserved,
            this, &ProximitySettingsDialog::clientPeripheralObserved);
    connect(&controller,
            &MacProximityController::clientPresenceIdentityRead,
            this, &ProximitySettingsDialog::clientPresenceIdentityRead);
    connect(&controller,
            &MacProximityController::clientPresenceIdentityFailed,
            this, &ProximitySettingsDialog::clientPresenceIdentityFailed);
    connect(&controller,
            &MacProximityController::clientPresenceScanSessionReset,
            this, &ProximitySettingsDialog::clientPresenceScanSessionReset);

    m_monotonicClock.start();
    m_clientPresenceRefreshTimer.setInterval(1000);
    connect(&m_clientPresenceRefreshTimer, &QTimer::timeout,
            this, &ProximitySettingsDialog::refreshNearbyClients);
    refreshPairingStatus();
    if (serverMode) {
        QString serverProximityId;
        if (config.hasServerProximityId()) {
            QString error;
            serverProximityId = config.serverProximityId(&error);
            if (!error.isEmpty()) {
                clientPresenceStatusLabel->setText(error);
            }
        }
        std::vector<barrier::ClientRouteAssociation> routes;
        m_clientPresenceRoutes =
            config.associatedClientPresenceRoutes(serverProximityId);
        for (auto route = m_clientPresenceRoutes.constBegin();
             route != m_clientPresenceRoutes.constEnd(); ++route) {
            routes.push_back({route.key().toStdString(),
                              route.value().toStdString()});
        }
        m_clientPresenceRegistry.replaceRoutes(routes);
        refreshNearbyClients();
        m_clientPresenceRefreshTimer.start();
        m_clientPresenceScanActive = true;
        controller.startClientPresenceScanning();
    }
    else {
        if (m_pairingBrowser == nullptr) {
            m_pairingBrowser =
                new ZeroconfProximityPairingBrowser(this);
        }
        connect(m_pairingBrowser,
                &ProximityPairingBrowser::recordsChanged,
                this,
                &ProximitySettingsDialog::pairingServersChanged);
        connect(m_pairingBrowser,
                &ProximityPairingBrowser::browseFailed,
                this,
                &ProximitySettingsDialog::pairingBrowseFailed);
        m_pairingBrowser->browseForType(
            QString::fromLatin1(kBarrierServerServiceType));
        controller.startScanning();
    }
}

ProximitySettingsDialog::~ProximitySettingsDialog()
{
    if (m_serverMode) {
        m_clientPresenceRefreshTimer.stop();
        if (m_clientPresenceScanActive) {
            m_controller.stopClientPresenceScanning();
            m_clientPresenceScanActive = false;
        }
    }
    else {
        m_controller.stopScanning();
    }
}

void ProximitySettingsDialog::accept()
{
    if (m_serverMode) {
        const bool changed =
            m_config.serverAdvertiserEnabled() !=
            advertisingCheckBox->isChecked();
        if (!m_config.setServerAdvertiserEnabled(
                advertisingCheckBox->isChecked())) {
            QMessageBox::warning(this, tr("Proximity settings"),
                                 m_config.error());
            return;
        }
        if (changed) {
            emit configurationChanged();
        }
        QDialog::accept();
        return;
    }

    barrier::ProximityPairing pairing;
    if (!m_config.pairing(pairing)) {
        if (gatingCheckBox->isChecked()) {
            QMessageBox::warning(
                this, tr("Proximity settings"),
                tr("Pair with a nearby server before enabling proximity gating."));
            return;
        }
        const bool changed = m_config.clientGatingEnabled();
        if (!m_config.setClientGatingEnabled(false)) {
            QMessageBox::warning(this, tr("Proximity settings"),
                                 m_config.error());
            return;
        }
        if (changed) {
            emit configurationChanged();
        }
        QDialog::accept();
        return;
    }

    const barrier::ProximityThresholdPolicy policy = thresholdDraft();
    if (!policy.isValid()) {
        updateThresholdValidation();
        return;
    }
    QString error;
    const barrier::ProximityConfigApplyResult result =
        m_config.applyClientSettings(
            gatingCheckBox->isChecked(),
            signalSharingCheckBox->isChecked(), pairing.proximityId,
            pairing.peripheralId, policy, &error);
    if (result == barrier::ProximityConfigApplyResult::Error) {
        QMessageBox::warning(this, tr("Proximity settings"),
                             error);
        return;
    }
    if (result == barrier::ProximityConfigApplyResult::Changed) {
        emit configurationChanged();
    }
    QDialog::accept();
}

void ProximitySettingsDialog::bonjourServersChanged()
{
    refreshNearbyServers();
}

void ProximitySettingsDialog::peripheralObserved(
    QUuid peripheralId, QString name, int rssiDbm)
{
    if (peripheralId.isNull() || rssiDbm == kInvalidCoreBluetoothRssi) {
        return;
    }
    NearbyPeripheral& nearby = m_nearby[peripheralId];
    nearby.name = name;
    nearby.lastRssiDbm = rssiDbm;

    if (!nearby.proximityId.isEmpty()) {
        refreshNearbyServers();
    }
    else if (!nearby.identityPending) {
        nearby.identityPending = true;
        m_controller.readPairingIdentity(peripheralId);
    }

    barrier::ProximityPairing pairing;
    if (m_config.pairing(pairing) && pairing.peripheralId == peripheralId) {
        if (!m_signalFilter) {
            m_signalFilter.reset(
                new barrier::ProximitySignalFilter(
                    pairing.thresholdPolicy));
        }
        m_signalFilter->addSample(rssiDbm, m_monotonicClock.elapsed());
        double filtered = 0.0;
        if (m_signalFilter->filteredDbm(filtered)) {
            filteredRssiLabel->setText(
                tr("Filtered signal: %1 dBm").arg(std::lround(filtered)));
        }
    }

}

void ProximitySettingsDialog::pairingIdentityRead(
    QUuid peripheralId, QString proximityId)
{
    auto nearby = m_nearby.find(peripheralId);
    if (nearby == m_nearby.end()) {
        return;
    }
    nearby->identityPending = false;
    nearby->proximityId = proximityId;
    refreshNearbyServers();
}

void ProximitySettingsDialog::pairingServersChanged(
    const QList<ZeroconfRecord>& servers)
{
    m_pairingServers = servers;
    refreshNearbyServers();
}

void ProximitySettingsDialog::pairingBrowseFailed()
{
    m_pairingServers.clear();
    refreshNearbyServers();
    pairingStatusLabel->setText(
        tr("Barrier could not browse for nearby pairing servers. Close and "
           "reopen Proximity Settings to retry."));
}

void ProximitySettingsDialog::operationFailed(
    QString operation, QString userMessage)
{
    Q_UNUSED(operation);
    for (auto nearby = m_nearby.begin(); nearby != m_nearby.end(); ++nearby) {
        nearby->identityPending = false;
    }
    pairingStatusLabel->setText(userMessage);
}

void ProximitySettingsDialog::clientPeripheralObserved(
    QUuid peripheralId, int rssiDbm)
{
    if (!m_serverMode || peripheralId.isNull() ||
        rssiDbm == kInvalidCoreBluetoothRssi) {
        return;
    }
    const qint64 nowMs = m_monotonicClock.elapsed();
    m_clientPresenceObservations[peripheralId] = {rssiDbm, nowMs};
    const auto routingId =
        m_clientPresenceRoutingIds.constFind(peripheralId);
    if (routingId != m_clientPresenceRoutingIds.constEnd()) {
        m_clientPresenceRegistry.observe(
            peripheralId.toString(QUuid::WithoutBraces).toStdString(),
            routingId.value().toStdString(), rssiDbm,
            static_cast<std::uint64_t>(nowMs));
    }
    refreshNearbyClients();
}

void ProximitySettingsDialog::clientPresenceIdentityRead(
    QUuid peripheralId, QString routingId)
{
    if (!m_serverMode || peripheralId.isNull() ||
        !barrier::ClientPresenceRegistry::isValidRoutingId(
            routingId.toStdString())) {
        return;
    }
    m_clientPresenceRoutingIds.insert(peripheralId, routingId);
    const auto observation =
        m_clientPresenceObservations.constFind(peripheralId);
    if (observation != m_clientPresenceObservations.constEnd()) {
        m_clientPresenceRegistry.observe(
            peripheralId.toString(QUuid::WithoutBraces).toStdString(),
            routingId.toStdString(), observation->rssiDbm,
            static_cast<std::uint64_t>(observation->monotonicMs));
    }
    refreshNearbyClients();
}

void ProximitySettingsDialog::clientPresenceIdentityFailed(
    QUuid peripheralId, QString message)
{
    if (!m_serverMode) {
        return;
    }
    m_clientPresenceRoutingIds.remove(peripheralId);
    clientPresenceStatusLabel->setText(message);
}

void ProximitySettingsDialog::clientPresenceScanSessionReset(
    quint64 generation)
{
    Q_UNUSED(generation);
    if (!m_serverMode) {
        return;
    }
    clearClientPresenceScanSession();
    clientPresenceStatusLabel->setText(
        tr("Refreshing nearby client signals…"));
    refreshNearbyClients();
}

void ProximitySettingsDialog::clearClientPresenceScanSession()
{
    m_clientPresenceObservations.clear();
    m_clientPresenceRoutingIds.clear();
    m_clientPresenceRegistry.clearObservations();
}

void ProximitySettingsDialog::refreshNearbyClients()
{
    if (!m_serverMode) {
        return;
    }
    QString serverProximityId;
    if (m_config.hasServerProximityId()) {
        QString error;
        serverProximityId = m_config.serverProximityId(&error);
        if (!error.isEmpty()) {
            clientPresenceStatusLabel->setText(error);
        }
    }
    const QMap<QString, QString> associated =
        m_config.associatedClientPresenceRoutes(serverProximityId);
    if (associated != m_clientPresenceRoutes) {
        m_clientPresenceRoutes = associated;
        clearClientPresenceScanSession();
        if (m_clientPresenceScanActive) {
            m_clientPresenceScanActive = false;
            m_controller.stopClientPresenceScanning();
            m_clientPresenceScanActive = true;
            m_controller.startClientPresenceScanning();
        }
    }

    std::vector<barrier::ClientRouteAssociation> currentRoutes;
    for (auto route = m_clientPresenceRoutes.constBegin();
         route != m_clientPresenceRoutes.constEnd(); ++route) {
        currentRoutes.push_back({route.key().toStdString(),
                                 route.value().toStdString()});
    }
    m_clientPresenceRegistry.replaceRoutes(currentRoutes);

    const qint64 elapsed = m_monotonicClock.isValid()
        ? m_monotonicClock.elapsed()
        : 0;
    const std::vector<barrier::ClientPresenceRow> rows =
        m_clientPresenceRegistry.rows(
            static_cast<std::uint64_t>(qMax<qint64>(0, elapsed)));
    QSet<QString> currentIds;
    for (const barrier::ClientPresenceRow& row : rows) {
        const QString routingId = QString::fromStdString(row.routingId);
        currentIds.insert(routingId);
        QTreeWidgetItem* item = nullptr;
        for (int index = 0; index < nearbyClientsTree->topLevelItemCount();
             ++index) {
            QTreeWidgetItem* candidate =
                nearbyClientsTree->topLevelItem(index);
            if (candidate->data(0, Qt::UserRole).toString() == routingId) {
                item = candidate;
                break;
            }
        }
        if (item == nullptr) {
            item = new QTreeWidgetItem(nearbyClientsTree);
            item->setData(0, Qt::UserRole, routingId);
        }
        item->setText(0, QString::fromStdString(row.screenName));
        item->setText(1, row.hasFilteredRssi
            ? tr("%1 dBm").arg(std::lround(row.filteredRssiDbm))
            : tr("Unavailable"));
        switch (row.state) {
        case barrier::ClientPresenceState::Available:
            item->setText(2, tr("Nearby"));
            break;
        case barrier::ClientPresenceState::Stale:
            item->setText(2, tr("Signal lost"));
            break;
        case barrier::ClientPresenceState::Unavailable:
            item->setText(2, tr("Not observed"));
            break;
        }
        if (!row.hasLastSeen) {
            item->setText(3, tr("Never"));
        }
        else {
            const std::uint64_t nowMs =
                static_cast<std::uint64_t>(qMax<qint64>(0, elapsed));
            const std::uint64_t ageSeconds = nowMs >= row.lastSeenMs
                ? (nowMs - row.lastSeenMs) / 1000
                : 0;
            item->setText(3, ageSeconds == 0
                ? tr("Now")
                : tr("%1 s ago").arg(ageSeconds));
        }
    }
    for (int index = nearbyClientsTree->topLevelItemCount() - 1;
         index >= 0; --index) {
        QTreeWidgetItem* item = nearbyClientsTree->topLevelItem(index);
        if (!currentIds.contains(
                item->data(0, Qt::UserRole).toString())) {
            delete nearbyClientsTree->takeTopLevelItem(index);
        }
    }
    if (rows.empty()) {
        clientPresenceStatusLabel->setText(
            tr("Opted-in clients appear after completing one Barrier connection."));
    }
    else {
        clientPresenceStatusLabel->setText(
            tr("Signal is measured only while this window is open. Bonjour wake remains independent."));
    }
}

void ProximitySettingsDialog::pairSelectedServer()
{
    QListWidgetItem* item = nearbyServersList->currentItem();
    if (item == nullptr) {
        pairingStatusLabel->setText(
            tr("Select a nearby Barrier server first."));
        return;
    }
    const QUuid peripheralId = item->data(Qt::UserRole).toUuid();
    const auto nearby = m_nearby.constFind(peripheralId);
    if (nearby == m_nearby.constEnd() || nearby->proximityId.isEmpty()) {
        pairingStatusLabel->setText(
            tr("The selected server identity is not ready yet."));
        return;
    }
    const ZeroconfRecord* currentServer =
        matchingServer(nearby->proximityId);
    if (currentServer == nullptr) {
        pairingStatusLabel->setText(
            tr("The matching Barrier network service disappeared. "
               "The previous pairing was kept."));
        return;
    }
    const QString displayName = currentServer->serviceName.isEmpty()
        ? currentServer->hostName
        : currentServer->serviceName;
    const barrier::ProximityPairing pairing{
        nearby->proximityId,
        peripheralId,
        displayName
    };
    QString error;
    if (!m_config.replacePairing(pairing, &error)) {
        pairingStatusLabel->setText(error);
        return;
    }

    m_signalFilter.reset(new barrier::ProximitySignalFilter(
        pairing.thresholdPolicy));
    refreshPairingStatus();
    pairingStatusLabel->setText(tr("Pairing saved."));
    emit configurationChanged();
}

void ProximitySettingsDialog::forgetPairing()
{
    if (QMessageBox::question(
            this, tr("Forget proximity pairing"),
            tr("Forget the paired server and stop matching it automatically?"))
        != QMessageBox::Yes) {
        return;
    }
    QString error;
    if (!m_config.clearPairing(&error)) {
        QMessageBox::warning(this, tr("Proximity settings"), error);
        return;
    }
    m_signalFilter.reset();
    refreshPairingStatus();
    emit configurationChanged();
}

void ProximitySettingsDialog::resetServerIdentity()
{
    if (QMessageBox::warning(
            this, tr("Reset pairing identity"),
            tr("Existing client pairings will stop matching this server. "
               "Reset the private pairing identity?"),
            QMessageBox::Reset | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Reset) {
        return;
    }
    QString error;
    if (!m_config.resetServerProximityId(&error)) {
        QMessageBox::warning(this, tr("Proximity settings"), error);
        return;
    }
    serverIdentityStatusLabel->setText(
        tr("The old identity was removed. A new private identity will be "
           "created when advertising starts."));
    resetIdentityButton->setEnabled(false);
    emit configurationChanged();
}

void ProximitySettingsDialog::resetThresholds()
{
    const barrier::ProximityThresholdPolicy defaults;
    connectDbmSpinBox->setValue(defaults.connectDbm);
    departureDbmSpinBox->setValue(defaults.departureDbm);
    updateThresholdValidation();
}

barrier::ProximityThresholdPolicy
ProximitySettingsDialog::thresholdDraft() const
{
    return {connectDbmSpinBox->value(), departureDbmSpinBox->value()};
}

void ProximitySettingsDialog::updateThresholdValidation()
{
    const bool valid = thresholdDraft().isValid();
    thresholdValidationLabel->setText(valid
        ? tr("The 15 dB anti-flapping gap is active.")
        : tr("Connect must be at least 15 dB stronger than departure."));
    QPushButton* ok = buttonBox->button(QDialogButtonBox::Ok);
    if (ok != nullptr) {
        barrier::ProximityPairing pairing;
        const bool paired = m_config.pairing(pairing);
        ok->setEnabled(m_serverMode || !paired || valid);
    }
}

void ProximitySettingsDialog::refreshPairingStatus()
{
    barrier::ProximityPairing pairing;
    const bool paired = m_config.pairing(pairing);
    pairedServerLabel->setText(paired
        ? tr("Paired server: %1").arg(pairing.displayName)
        : tr("Paired server: None"));
    filteredRssiLabel->setText(tr("Filtered signal: Unavailable"));
    forgetButton->setEnabled(paired);
    signalSharingCheckBox->setEnabled(paired);
    signalSharingCheckBox->setChecked(
        paired && pairing.signalSharingEnabled);
    connectDbmSpinBox->setEnabled(paired);
    departureDbmSpinBox->setEnabled(paired);
    resetThresholdsButton->setEnabled(paired);
    if (paired) {
        connectDbmSpinBox->setValue(pairing.thresholdPolicy.connectDbm);
        departureDbmSpinBox->setValue(
            pairing.thresholdPolicy.departureDbm);
        m_signalFilter.reset(
            new barrier::ProximitySignalFilter(
                pairing.thresholdPolicy));
    }
    else {
        const barrier::ProximityThresholdPolicy defaults;
        connectDbmSpinBox->setValue(defaults.connectDbm);
        departureDbmSpinBox->setValue(defaults.departureDbm);
        m_signalFilter.reset();
    }
    updateThresholdValidation();
}

const ZeroconfRecord* ProximitySettingsDialog::matchingServer(
    const QString& proximityId) const
{
    for (const ZeroconfRecord& server : m_pairingServers) {
        if (server.matchesProximityServer(proximityId)) {
            return &server;
        }
    }
    for (const ZeroconfRecord& server : m_servers) {
        if (server.matchesProximityServer(proximityId)) {
            return &server;
        }
    }
    return nullptr;
}

void ProximitySettingsDialog::refreshNearbyServers()
{
    for (auto nearby = m_nearby.begin(); nearby != m_nearby.end(); ++nearby) {
        QListWidgetItem* item = nullptr;
        int itemIndex = -1;
        for (int index = 0; index < nearbyServersList->count(); ++index) {
            QListWidgetItem* candidate = nearbyServersList->item(index);
            if (candidate->data(Qt::UserRole).toUuid() == nearby.key()) {
                item = candidate;
                itemIndex = index;
                break;
            }
        }

        const ZeroconfRecord* server = nearby->proximityId.isEmpty()
            ? nullptr
            : matchingServer(nearby->proximityId);
        if (server == nullptr) {
            if (item != nullptr) {
                delete nearbyServersList->takeItem(itemIndex);
            }
            nearby->displayName.clear();
            continue;
        }

        nearby->displayName = server->serviceName.isEmpty()
            ? server->hostName
            : server->serviceName;
        if (item == nullptr) {
            item = new QListWidgetItem(nearbyServersList);
            item->setData(Qt::UserRole, nearby.key());
        }
        item->setText(
            peripheralLabel(nearby->displayName, nearby->lastRssiDbm));
    }
}
