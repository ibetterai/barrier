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

#pragma once

#include "ClientPresenceAssociation.h"
#include "ZeroconfEndpointLogPolicy.h"
#include "ZeroconfServer.h"
#include "ZeroconfRecord.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QMap>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimer>

typedef int32_t  DNSServiceErrorType;

class ZeroconfRegister;
class ZeroconfBrowser;
class MainWindow;

class ZeroconfService : public QObject
{
    Q_OBJECT

public:
    struct Requirements {
        bool active;
        bool publishClientService;
    };

    ZeroconfService(MainWindow* mainWindow, bool publishClientService);
    ~ZeroconfService();
    void setServerDisplayReady(bool ready);
    void wakeClient(const QString& screenName);
    void clientConnected(const QString& screenName);
    static Requirements requirements(bool autoConfigEnabled, bool serverMode,
                                     bool proximityClientGatingEnabled,
                                     bool clientSignalSharingEnabled = false)
    {
        return {
            autoConfigEnabled || serverMode ||
                proximityClientGatingEnabled ||
                clientSignalSharingEnabled,
            (autoConfigEnabled || proximityClientGatingEnabled ||
             clientSignalSharingEnabled) &&
                !serverMode
        };
    }

signals:
    void serversChanged(const QList<ZeroconfRecord>& records);

private slots:
    void serverDetected(const QList<ZeroconfRecord>& list);
    void clientDetected(const QList<ZeroconfRecord>& list);
    void registrationSucceeded(const ZeroconfRecord& record);
    void errorHandle(DNSServiceErrorType errorCode);

private:
    bool registerService(bool server);
    void scheduleClientRegistrationRetry();
    void clearWakeState(const QString& screenName);
    void tryAssociateClientPresence(const QString& screenName);

private:
    MainWindow* m_pMainWindow;
    bool m_PublishClientService;
    ZeroconfServer m_zeroconfServer;
    ZeroconfBrowser* m_pZeroconfBrowser;
    ZeroconfRegister* m_pZeroconfRegister;
    bool m_ServiceRegistered;
    bool m_RegistrationPending;
    int m_ClientRegistrationAttempts{0};
    QTimer m_ClientRegistrationRetryTimer;
    QMap<QString, QByteArray> m_ServerTxt;
    barrier::ZeroconfEndpointLogPolicy m_ServerEndpointLogPolicy;
    QMap<QString, ZeroconfRecord> m_PairedClientRecords;
    QList<ZeroconfRecord> m_ClientRecords;
    barrier::ClientPresenceConnectionEvidence
        m_ClientPresenceConnectionEvidence;
    QMap<QString, qint64> m_NextWakeAttemptMs;
    QMap<QString, int> m_WakeAttemptCounts;
    QSet<QString> m_WakeInFlight;
    QMap<QString, QPointer<QObject>> m_WakeOperations;
    QElapsedTimer m_WakeClock;

    static const char* m_ServerServiceName;
    static const char* m_ClientServiceName;
};
