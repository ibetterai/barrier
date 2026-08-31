/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2014-2016 Symless Ltd.
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ZeroconfService.h"

#include "MainWindow.h"
#include "ClientPresenceAssociation.h"
#include "ZeroconfRegister.h"
#include "ZeroconfBrowser.h"
#include "ZeroconfWake.h"

#include <QPointer>
#include <QMessageBox>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <stdlib.h>
#endif


const char* ZeroconfService:: m_ServerServiceName = "_barrierServerZeroconf._tcp";
const char* ZeroconfService:: m_ClientServiceName = "_barrierClientZeroconf._tcp";
const int kMaximumClientRegistrationAttempts = 3;

static void silence_avahi_warning()
{
    // the libavahi folks seemingly find Apple's bonjour API distasteful
    // and are quite liberal in taking it out on users...unless we set
    // this environmental variable before calling the avahi library.
    // additionally, Microsoft does not give us a POSIX setenv() so
    // we use their OS-specific API instead
    const char *name  = "AVAHI_COMPAT_NOWARN";
    const char *value = "1";
#ifdef _WIN32
    SetEnvironmentVariable(name, value);
#else
    setenv(name, value, 1);
#endif
}

static bool sameWakeRoute(const ZeroconfRecord& left,
                          const ZeroconfRecord& right)
{
    return left == right &&
           left.hostName == right.hostName &&
           left.port == right.port &&
           left.interfaceIndex == right.interfaceIndex &&
           left.txt == right.txt;
}

ZeroconfService::ZeroconfService(MainWindow* mainWindow,
                                 bool publishClientService) :
    m_pMainWindow(mainWindow),
    m_PublishClientService(publishClientService),
    m_pZeroconfBrowser(0),
    m_pZeroconfRegister(nullptr),
    m_ServiceRegistered(false),
    m_RegistrationPending(false)
{
    silence_avahi_warning();
    m_WakeClock.start();
    m_ClientRegistrationRetryTimer.setSingleShot(true);
    connect(&m_ClientRegistrationRetryTimer, &QTimer::timeout,
            this, [this]() {
                if (m_PublishClientService && !m_ServiceRegistered &&
                    !m_RegistrationPending &&
                    m_ClientRegistrationAttempts <
                        kMaximumClientRegistrationAttempts) {
                    registerService(false);
                }
            });
    connect(this, &ZeroconfService::serversChanged,
            m_pMainWindow, &MainWindow::serverDetected);
    if (m_pMainWindow->barrier_type() == BarrierType::Server) {
        if (registerService(true)) {
            m_pZeroconfBrowser = new ZeroconfBrowser(this);
            connect(m_pZeroconfBrowser, SIGNAL(
                currentRecordsChanged(const QList<ZeroconfRecord>&)),
                this, SLOT(clientDetected(const QList<ZeroconfRecord>&)));
            m_pZeroconfBrowser->browseForType(
                QLatin1String(m_ClientServiceName));
        }
    }
    else {
        // Publish immediately so the system sleep proxy can retain the
        // client's wake route even when the paired server is temporarily
        // absent. Waiting for a browse result makes a sleeping client
        // impossible to discover after a cold launch.
        if (m_PublishClientService) {
            registerService(false);
        }
        m_pZeroconfBrowser = new ZeroconfBrowser(this);
        connect(m_pZeroconfBrowser, SIGNAL(
            currentRecordsChanged(const QList<ZeroconfRecord>&)),
            this, SLOT(serverDetected(const QList<ZeroconfRecord>&)));
        m_pZeroconfBrowser->browseForType(
            QLatin1String(m_ServerServiceName));
    }

    if (m_pZeroconfBrowser != nullptr) {
        connect(m_pZeroconfBrowser, SIGNAL(error(DNSServiceErrorType)),
                this, SLOT(errorHandle(DNSServiceErrorType)));
    }
}

void ZeroconfService::setServerDisplayReady(bool ready)
{
    m_ServerTxt.insert(
        QStringLiteral("display-ready"),
        ready ? QByteArrayLiteral("1") : QByteArrayLiteral("0"));
    if (m_pZeroconfRegister != nullptr) {
        m_pZeroconfRegister->updateTxt(m_ServerTxt);
    }
}

ZeroconfService::~ZeroconfService()
{
    if (m_pZeroconfBrowser) {
        delete m_pZeroconfBrowser;
    }
    if (m_pZeroconfRegister) {
        delete m_pZeroconfRegister;
    }
}

void ZeroconfService::serverDetected(const QList<ZeroconfRecord>& list)
{
    if (m_PublishClientService && !m_ServiceRegistered &&
        !m_RegistrationPending && !list.isEmpty() &&
        m_ClientRegistrationAttempts < kMaximumClientRegistrationAttempts) {
        registerService(false);
    }
    for (const ZeroconfRecord& record :
         m_ServerEndpointLogPolicy.newOrChangedEndpoints(list)) {
        m_pMainWindow->appendLogInfo(
            tr("zeroconf server resolved: %1 at %2:%3")
                .arg(record.serviceName, record.hostName)
                .arg(record.port));
    }
    emit serversChanged(list);
}

void ZeroconfService::clientDetected(const QList<ZeroconfRecord>& list)
{
    m_ClientRecords = list;
    QString serverProximityId;
    barrier::ProximityConfig& proximity =
        m_pMainWindow->proximityConfig();
    if (proximity.serverAdvertiserEnabled()) {
        QString error;
        serverProximityId = proximity.serverProximityId(&error);
        if (serverProximityId.isEmpty() && !error.isEmpty()) {
            m_pMainWindow->appendLogError(
                tr("Unable to identify paired wake clients: %1")
                    .arg(error));
        }
    }

    QMap<QString, QList<ZeroconfRecord>> wakeCandidates;
    for (const ZeroconfRecord& record : list) {
        m_pMainWindow->appendLogInfo(tr("zeroconf client detected: %1").arg(
            record.serviceName));
        m_pMainWindow->autoAddScreen(record.serviceName);
        if (!serverProximityId.isEmpty() &&
            record.isCurrentLocalWakeRoute(serverProximityId)) {
            wakeCandidates[record.serviceName].append(record);
        }
    }

    QMap<QString, ZeroconfRecord> nextPairedClientRecords;
    for (auto candidate = wakeCandidates.constBegin();
         candidate != wakeCandidates.constEnd(); ++candidate) {
        if (candidate.value().size() == 1) {
            nextPairedClientRecords.insert(
                candidate.key(), candidate.value().first());
        }
        else {
            m_pMainWindow->appendLogError(
                tr("Ignoring ambiguous Bonjour wake routes for %1")
                    .arg(candidate.key()));
        }
    }

    for (auto current = m_PairedClientRecords.constBegin();
         current != m_PairedClientRecords.constEnd(); ++current) {
        const auto next = nextPairedClientRecords.constFind(current.key());
        if (next == nextPairedClientRecords.constEnd() ||
            !sameWakeRoute(current.value(), next.value())) {
            clearWakeState(current.key());
        }
    }
    m_PairedClientRecords.swap(nextPairedClientRecords);

    const qint64 associationNowMs = m_WakeClock.elapsed();
    for (const QString& screenName :
         m_ClientPresenceConnectionEvidence.pending(associationNowMs)) {
        tryAssociateClientPresence(screenName);
    }
}

void ZeroconfService::wakeClient(const QString& screenName)
{
    const auto record = m_PairedClientRecords.constFind(screenName);
    if (record == m_PairedClientRecords.constEnd()) {
        m_pMainWindow->appendLogDebug(
            tr("No paired Bonjour wake route for %1").arg(screenName));
        return;
    }

    const qint64 nowMs = m_WakeClock.elapsed();
    if (m_WakeInFlight.contains(screenName) ||
        nowMs < m_NextWakeAttemptMs.value(screenName, -1)) {
        return;
    }
    const int attempts = m_WakeAttemptCounts.value(screenName, 0);
    m_WakeAttemptCounts.insert(screenName, attempts + 1);
    m_NextWakeAttemptMs.insert(
        screenName, nowMs + barrier::zeroconfWakeBackoffMs(attempts));
    m_WakeInFlight.insert(screenName);

    m_pMainWindow->appendLogInfo(
        tr("requesting network wake for paired client %1")
            .arg(screenName));
    QPointer<ZeroconfService> guard(this);
    QObject* operation = barrier::scheduleZeroconfWake(
        record.value(), this,
        [guard, screenName](bool success, const QString& message) {
            if (guard.isNull()) {
                return;
            }
            guard->m_WakeInFlight.remove(screenName);
            guard->m_WakeOperations.remove(screenName);
            if (success) {
                guard->m_pMainWindow->appendLogInfo(
                    tr("paired client %1 answered its network wake endpoint")
                        .arg(screenName));
            }
            else {
                guard->m_pMainWindow->appendLogError(
                    tr("Network wake for %1 failed: %2")
                        .arg(screenName, message));
            }
        });
    m_WakeOperations.insert(screenName, QPointer<QObject>(operation));
}

void ZeroconfService::clientConnected(const QString& screenName)
{
    // Only the real Barrier protocol connection clears the exponential
    // backoff. Reaching the unauthenticated auxiliary wake port is merely a
    // readiness signal, not client authentication.
    clearWakeState(screenName);
    m_ClientPresenceConnectionEvidence.record(
        screenName, m_WakeClock.elapsed());
    tryAssociateClientPresence(screenName);
}

void ZeroconfService::tryAssociateClientPresence(
    const QString& screenName)
{
    const qint64 nowMs = m_WakeClock.elapsed();
    if (!m_ClientPresenceConnectionEvidence.contains(
            screenName, nowMs)) {
        return;
    }
    barrier::ProximityConfig& proximity =
        m_pMainWindow->proximityConfig();
    if (!proximity.serverAdvertiserEnabled()) {
        m_ClientPresenceConnectionEvidence.discard(screenName);
        return;
    }
    QString error;
    const QString serverProximityId =
        proximity.serverProximityId(&error);
    if (serverProximityId.isEmpty()) {
        m_ClientPresenceConnectionEvidence.discard(screenName);
        if (!error.isEmpty()) {
            m_pMainWindow->appendLogError(
                tr("Unable to associate client presence route: %1")
                    .arg(error));
        }
        return;
    }

    const barrier::ClientPresenceSelection selection =
        barrier::selectClientPresenceAssociation(
            m_ClientRecords, screenName, serverProximityId);
    if (selection.status ==
        barrier::ClientPresenceSelectionStatus::Ambiguous) {
        m_ClientPresenceConnectionEvidence.discard(screenName);
        m_pMainWindow->appendLogError(
            tr("Ignoring ambiguous client presence route for %1")
                .arg(screenName));
        return;
    }
    if (selection.status !=
        barrier::ClientPresenceSelectionStatus::Ready) {
        return;
    }

    // One protocol connection permits one association decision. Consume the
    // evidence before persistence/logging so a re-entrant Bonjour update
    // cannot reuse it to rotate a route without another real connection.
    m_ClientPresenceConnectionEvidence.discard(screenName);

    const barrier::ClientPresenceAssociationResult result =
        proximity.associateClientPresenceRoute(
            screenName, selection.routingId,
            serverProximityId, &error);
    if (result == barrier::ClientPresenceAssociationResult::Conflict ||
        result == barrier::ClientPresenceAssociationResult::Error) {
        m_pMainWindow->appendLogError(
            tr("Unable to associate presence for %1: %2")
                .arg(screenName, error));
    }
    else if (result ==
             barrier::ClientPresenceAssociationResult::Changed) {
        m_pMainWindow->appendLogInfo(
            tr("associated nearby signal route for %1")
                .arg(screenName));
    }
}

void ZeroconfService::clearWakeState(const QString& screenName)
{
    const QPointer<QObject> operation =
        m_WakeOperations.take(screenName);
    if (!operation.isNull()) {
        delete operation.data();
    }
    m_WakeInFlight.remove(screenName);
    m_NextWakeAttemptMs.remove(screenName);
    m_WakeAttemptCounts.remove(screenName);
}

void ZeroconfService::registrationSucceeded(const ZeroconfRecord&)
{
    m_RegistrationPending = false;
    m_ServiceRegistered = true;
    m_ClientRegistrationRetryTimer.stop();
}

void ZeroconfService::errorHandle(DNSServiceErrorType errorCode)
{
    if (sender() == m_pZeroconfRegister) {
        m_RegistrationPending = false;
        m_ServiceRegistered = false;
        ZeroconfRegister* failedRegister = m_pZeroconfRegister;
        m_pZeroconfRegister = nullptr;
        failedRegister->deleteLater();
        scheduleClientRegistrationRetry();
    }
    QMessageBox::critical(0, tr("Zero configuration service"),
        tr("Error code: %1.").arg(errorCode));
}


bool ZeroconfService::registerService(bool server)
{
    if (m_ServiceRegistered || m_RegistrationPending) {
        return true;
    }
    if (!server && m_PublishClientService) {
        ++m_ClientRegistrationAttempts;
    }
    if (!m_zeroconfServer.isListening() &&
        !m_zeroconfServer.listen()) {
        QMessageBox::critical(0, tr("Zero configuration service"),
            tr("Unable to start the zeroconf: %1.")
            .arg(m_zeroconfServer.errorString()));
        scheduleClientRegistrationRetry();
        return false;
    }

    delete m_pZeroconfRegister;
    m_pZeroconfRegister = new ZeroconfRegister(this);
    connect(m_pZeroconfRegister, &ZeroconfRegister::error,
            this, &ZeroconfService::errorHandle);
    connect(m_pZeroconfRegister, &ZeroconfRegister::serviceRegistered,
            this, &ZeroconfService::registrationSucceeded);

    ZeroconfRecord record(
        m_pMainWindow->getScreenName(),
        QLatin1String(server ? m_ServerServiceName : m_ClientServiceName),
        QString());
    if (server) {
        m_ServerTxt.insert(
            QStringLiteral("display-ready"),
            m_pMainWindow->serverDisplayReady()
                ? QByteArrayLiteral("1")
                : QByteArrayLiteral("0"));
        barrier::ProximityConfig& proximity =
            m_pMainWindow->proximityConfig();
        if (proximity.serverAdvertiserEnabled()) {
            QString proximityError;
            const QString proximityId =
                proximity.serverProximityId(&proximityError);
            if (proximityId.isEmpty()) {
                m_pMainWindow->appendLogError(
                    tr("Unable to advertise proximity identity: %1")
                        .arg(proximityError));
                delete m_pZeroconfRegister;
                m_pZeroconfRegister = nullptr;
                return false;
            }
            m_ServerTxt.insert(
                QStringLiteral("proximity-id"), proximityId.toLatin1());
        }
        else {
            m_ServerTxt.remove(QStringLiteral("proximity-id"));
        }
        record.txt = m_ServerTxt;
    }
    else {
        barrier::ProximityConfig& proximity =
            m_pMainWindow->proximityConfig();
        barrier::ProximityPairing pairing;
        if (proximity.pairing(pairing) &&
            (proximity.clientGatingEnabled() ||
             pairing.signalSharingEnabled)) {
            record.txt.insert(
                QStringLiteral("paired-server-id"),
                pairing.proximityId.toLatin1());
            if (pairing.signalSharingEnabled &&
                m_pMainWindow->barrierRunIntended() &&
                !pairing.clientRoutingId.isEmpty()) {
                record.txt.insert(
                    QStringLiteral("client-routing-id"),
                    pairing.clientRoutingId.toLatin1());
            }
        }
    }
    m_RegistrationPending = true;
    m_pZeroconfRegister->registerService(
        record, m_zeroconfServer.serverPort(), record.txt);
    return m_RegistrationPending || m_ServiceRegistered;
}

void ZeroconfService::scheduleClientRegistrationRetry()
{
    if (!m_PublishClientService || m_ServiceRegistered ||
        m_RegistrationPending ||
        m_ClientRegistrationAttempts >= kMaximumClientRegistrationAttempts ||
        m_ClientRegistrationRetryTimer.isActive()) {
        return;
    }
    const int delayMs = 1000 << qMax(0, m_ClientRegistrationAttempts - 1);
    m_ClientRegistrationRetryTimer.start(delayMs);
}
