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

#include "ZeroconfRecord.h"

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QMap>
#define _MSL_STDINT_H
#include <stdint.h>
#include <dns_sd.h>

class QSocketNotifier;
class ZeroconfBrowserTestPeer;

class ZeroconfRecordAggregator
{
public:
    static QString serviceKey(const ZeroconfRecord& record);
    static QString resolverKey(const ZeroconfRecord& record,
                               quint32 interfaceIndex);

    bool update(const ZeroconfRecord& record, quint32 interfaceIndex);
    bool remove(const ZeroconfRecord& record, quint32 interfaceIndex);
    void clear();
    bool isEmpty() const;
    QList<ZeroconfRecord> records() const;

private:
    struct ResolvedRecord {
        ZeroconfRecord record;
        QString serviceKey;
        quint64 revision = 0;
    };

    bool recordForService(const QString& key, ZeroconfRecord& record) const;

    QMap<QString, ResolvedRecord> m_ResolvedRecords;
    QList<QString> m_ServiceOrder;
    quint64 m_NextRevision = 0;
};

class ZeroconfBrowser : public QObject
{
    Q_OBJECT

public:
    ZeroconfBrowser(QObject* parent = 0);
    ~ZeroconfBrowser();
    void browseForType(const QString& type);
    inline QList<ZeroconfRecord> currentRecords() const
    {
        return m_RecordAggregator.records();
    }
    inline QString serviceType() const { return m_BrowsingType; }

signals:
    void currentRecordsChanged(const QList<ZeroconfRecord>& list);
    // Browse-level failures only. A resolver is scoped to one service on one
    // interface, so its failure retires only that resolver.
    void error(DNSServiceErrorType err);

private slots:
    void socketReadyRead();
    void resolverSocketReadyRead();

private:
    friend class ZeroconfBrowserTestPeer;

    struct Resolver {
        ZeroconfBrowser* browser = nullptr;
        QString key;
        ZeroconfRecord record;
        QString fullName;
        quint32 interfaceIndex = 0;
        DNSServiceRef serviceRef = nullptr;
        bool failed = false;
        bool needsTxtMonitor = false;
        bool resolvePublishPending = false;
        bool txtPublishPending = false;
        QSocketNotifier* socket = nullptr;
    };

    struct BrowsedService {
        ZeroconfRecord record;
        quint32 interfaceIndex = 0;
    };

    void startResolve(const ZeroconfRecord& record, quint32 interfaceIndex);
    void scheduleResolverRetry(const QString& key);
    DNSServiceErrorType startTxtMonitor(Resolver* resolver);
    static DNSServiceErrorType parseTxt(
        quint16 length, const void* bytes,
        QMap<QString, QByteArray>& txt);
    bool stopResolver(const QString& key);
    void finishBrowseFailure(DNSServiceErrorType errorCode);
    void resolverFailed(const QString& key);
    void resolverReadyRead(Resolver* resolver);
    void publishRecords();
    static void DNSSD_API browseReply(
        DNSServiceRef, DNSServiceFlags flags, quint32 interfaceIndex,
        DNSServiceErrorType errorCode, const char* serviceName,
        const char* regType, const char* replyDomain, void* context);
    static void DNSSD_API resolveReply(
        DNSServiceRef, DNSServiceFlags flags, quint32 interfaceIndex,
        DNSServiceErrorType errorCode, const char* fullName,
        const char* hostTarget, quint16 port, quint16 txtLength,
        const unsigned char* txtRecord, void* context);
    static void DNSSD_API queryReply(
        DNSServiceRef, DNSServiceFlags flags, quint32 interfaceIndex,
        DNSServiceErrorType errorCode, const char* fullName,
        quint16 rrtype, quint16 rrclass, quint16 rdlen,
        const void* rdata, quint32 ttl, void* context);

private:
    DNSServiceRef m_DnsServiceRef;
    QSocketNotifier* m_pSocket;
    ZeroconfRecordAggregator m_RecordAggregator;
    QMap<QString, Resolver*> m_Resolvers;
    QMap<QString, BrowsedService> m_BrowsedServices;
    QMap<QString, quint64> m_ResolverRetryTokens;
    quint64 m_NextResolverRetryToken = 0;
    bool m_BrowseBatchPending = false;
    bool m_PublishPending = false;
    DNSServiceErrorType m_BrowseCallbackError = kDNSServiceErr_NoError;
    QString m_BrowsingType;
};
