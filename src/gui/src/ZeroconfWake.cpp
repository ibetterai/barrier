/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "ZeroconfWake.h"

#include "ZeroconfRecord.h"

#include <QtCore/QByteArray>
#include <QtCore/QSocketNotifier>
#include <QtCore/QTimer>
#include <QtCore/QtEndian>
#include <QtNetwork/QTcpSocket>

#define _MSL_STDINT_H
#include <stdint.h>
#include <dns_sd.h>

#include <utility>

namespace barrier {

namespace {

const qint64 kInitialWakeBackoffMs = 15000;
const qint64 kMaximumWakeBackoffMs = 240000;
const int kTcpRetryMilliseconds = 750;
const int kWakeTimeoutMilliseconds = 15000;
const char kPairedServerIdKey[] = "paired-server-id";

class WakeResolveOperation final : public QObject
{
public:
    WakeResolveOperation(const ZeroconfRecord& record,
                         QObject* parent,
                         ZeroconfWakeCompletion completion) :
        QObject(parent),
        m_record(record),
        m_completion(std::move(completion)),
        m_expectedPairedServerId(
            record.txt.value(QStringLiteral("paired-server-id")))
    {
        m_retryTimer.setSingleShot(true);
        m_timeoutTimer.setSingleShot(true);
        connect(&m_retryTimer, &QTimer::timeout,
                this, [this]() { tryTcpConnect(); });
        connect(&m_timeoutTimer, &QTimer::timeout, this, [this]() {
            complete(false,
                     tr("the paired client did not answer its wake endpoint"));
        });
        connect(&m_socket, &QTcpSocket::connected, this, [this]() {
            complete(true, QString());
        });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        connect(&m_socket, &QAbstractSocket::errorOccurred,
                this, [this](QAbstractSocket::SocketError) {
                    scheduleTcpRetry();
                });
#else
        connect(
            &m_socket,
            QOverload<QAbstractSocket::SocketError>::of(
                &QAbstractSocket::error),
            this,
            [this](QAbstractSocket::SocketError) {
                scheduleTcpRetry();
            });
#endif
    }

    ~WakeResolveOperation() override
    {
        cleanupDns();
    }

    void start()
    {
        if (!m_record.hasResolvedService() ||
            m_record.interfaceIndex == 0 ||
            m_record.replyDomain.compare(
                QStringLiteral("local."), Qt::CaseInsensitive) != 0 ||
            m_expectedPairedServerId.isEmpty()) {
            complete(false,
                     tr("the paired client has no current local Bonjour wake route"));
            return;
        }

        m_timeoutTimer.start(kWakeTimeoutMilliseconds);
        const QByteArray name = m_record.serviceName.toUtf8();
        const QByteArray type = m_record.registeredType.toUtf8();
        const QByteArray domain = m_record.replyDomain.toUtf8();
        DNSServiceFlags flags = 0;
#if defined(Q_OS_MAC)
        flags |= kDNSServiceFlagsWakeOnResolve;
#endif
        const DNSServiceErrorType error = DNSServiceResolve(
            &m_dnsRef, flags, m_record.interfaceIndex,
            name.constData(), type.constData(), domain.constData(),
            resolveReply, this);
        if (error != kDNSServiceErr_NoError) {
            complete(false,
                     tr("Bonjour could not start the targeted wake request"));
            return;
        }
        const int socketFd = DNSServiceRefSockFD(m_dnsRef);
        if (socketFd < 0) {
            complete(false,
                     tr("Bonjour returned no wake resolver socket"));
            return;
        }
        m_dnsSocket = new QSocketNotifier(
            socketFd, QSocketNotifier::Read, this);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        connect(
            m_dnsSocket, &QSocketNotifier::activated, this,
            [this](QSocketDescriptor, QSocketNotifier::Type) {
                processDnsResult();
            });
#else
        connect(
            m_dnsSocket,
            QOverload<int>::of(&QSocketNotifier::activated),
            this,
            [this](int) { processDnsResult(); });
#endif
    }

private:
    static void DNSSD_API resolveReply(
        DNSServiceRef, DNSServiceFlags, quint32 interfaceIndex,
        DNSServiceErrorType errorCode, const char*, const char* hostTarget,
        quint16 port, quint16 txtLength,
        const unsigned char* txtRecord, void* context)
    {
        WakeResolveOperation* operation =
            static_cast<WakeResolveOperation*>(context);
        if (operation->m_resolveCallbackPending || operation->m_finished) {
            return;
        }
        operation->m_resolveCallbackPending = true;
        operation->m_resolveError = errorCode;
        if (errorCode == kDNSServiceErr_NoError && hostTarget != nullptr &&
            interfaceIndex == operation->m_record.interfaceIndex) {
            operation->m_resolvedHost = QString::fromUtf8(hostTarget);
            operation->m_resolvedPort = qFromBigEndian(port);

            quint8 valueLength = 0;
            const void* value = txtRecord == nullptr
                ? nullptr
                : TXTRecordGetValuePtr(
                      txtLength, txtRecord, kPairedServerIdKey,
                      &valueLength);
            const QByteArray advertisedServerId = value == nullptr
                ? QByteArray()
                : QByteArray(static_cast<const char*>(value), valueLength);
            operation->m_pairingStillMatches =
                operation->m_record.matchesCurrentWakeResolution(
                    operation->m_resolvedHost, advertisedServerId);
        }
        QTimer::singleShot(0, operation, [operation]() {
            operation->finishResolve();
        });
    }

    void processDnsResult()
    {
        if (m_finished || m_dnsRef == nullptr) {
            return;
        }
        if (m_dnsSocket != nullptr) {
            m_dnsSocket->setEnabled(false);
        }
        const DNSServiceErrorType error =
            DNSServiceProcessResult(m_dnsRef);
        if (error != kDNSServiceErr_NoError &&
            !m_resolveCallbackPending) {
            complete(false,
                     tr("Bonjour could not resolve the paired wake service"));
        }
    }

    void finishResolve()
    {
        cleanupDns();
        if (m_finished) {
            return;
        }
        if (m_resolveError != kDNSServiceErr_NoError ||
            m_resolvedHost.isEmpty() || m_resolvedPort == 0) {
            complete(false,
                     tr("Bonjour could not resolve the paired wake service"));
            return;
        }
        if (!m_pairingStillMatches) {
            complete(false,
                     tr("the Bonjour wake route no longer matches the paired client"));
            return;
        }
        tryTcpConnect();
    }

    void tryTcpConnect()
    {
        if (m_finished) {
            return;
        }
        m_socket.abort();
        m_socket.connectToHost(m_resolvedHost, m_resolvedPort);
    }

    void scheduleTcpRetry()
    {
        if (!m_finished && m_timeoutTimer.isActive() &&
            !m_retryTimer.isActive()) {
            m_retryTimer.start(kTcpRetryMilliseconds);
        }
    }

    void cleanupDns()
    {
        if (m_dnsSocket != nullptr) {
            m_dnsSocket->setEnabled(false);
            m_dnsSocket->deleteLater();
            m_dnsSocket = nullptr;
        }
        if (m_dnsRef != nullptr) {
            DNSServiceRefDeallocate(m_dnsRef);
            m_dnsRef = nullptr;
        }
    }

    void complete(bool success, const QString& message)
    {
        if (m_finished) {
            return;
        }
        m_finished = true;
        m_retryTimer.stop();
        m_timeoutTimer.stop();
        m_socket.abort();
        cleanupDns();
        ZeroconfWakeCompletion completion = std::move(m_completion);
        if (completion) {
            completion(success, message);
        }
        deleteLater();
    }

    ZeroconfRecord m_record;
    ZeroconfWakeCompletion m_completion;
    QByteArray m_expectedPairedServerId;
    DNSServiceRef m_dnsRef{nullptr};
    QSocketNotifier* m_dnsSocket{nullptr};
    QTcpSocket m_socket;
    QTimer m_retryTimer;
    QTimer m_timeoutTimer;
    QString m_resolvedHost;
    quint16 m_resolvedPort{0};
    DNSServiceErrorType m_resolveError{kDNSServiceErr_NoError};
    bool m_resolveCallbackPending{false};
    bool m_pairingStillMatches{false};
    bool m_finished{false};
};

} // namespace

qint64 zeroconfWakeBackoffMs(int previousAttempts)
{
    qint64 delay = kInitialWakeBackoffMs;
    for (int attempt = 0;
         attempt < previousAttempts && delay < kMaximumWakeBackoffMs;
         ++attempt) {
        delay = qMin(delay * 2, kMaximumWakeBackoffMs);
    }
    return delay;
}

QObject* scheduleZeroconfWake(const ZeroconfRecord& record,
                              QObject* parent,
                              ZeroconfWakeCompletion completion)
{
    WakeResolveOperation* operation = new WakeResolveOperation(
        record, parent, std::move(completion));
    QTimer::singleShot(0, operation, [operation]() {
        operation->start();
    });
    return operation;
}

} // namespace barrier
