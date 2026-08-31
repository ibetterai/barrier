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

#include "ZeroconfBrowser.h"

#include <QtCore/QByteArray>
#include <QtCore/QPointer>
#include <QtCore/QSocketNotifier>
#include <QtCore/QTimer>
#include <QtCore/QtEndian>

namespace {

bool sameResolvedRecord(const ZeroconfRecord& left,
                        const ZeroconfRecord& right)
{
    return left == right &&
           left.hostName == right.hostName &&
           left.port == right.port &&
           left.interfaceIndex == right.interfaceIndex &&
           left.txt == right.txt;
}

const int kResolverRetryMilliseconds = 1000;

}

QString ZeroconfRecordAggregator::serviceKey(const ZeroconfRecord& record)
{
    const QChar separator(0x1f);
    return record.serviceName + separator +
           record.registeredType + separator +
           record.replyDomain;
}

QString ZeroconfRecordAggregator::resolverKey(
    const ZeroconfRecord& record, quint32 interfaceIndex)
{
    const QChar separator(0x1f);
    return serviceKey(record) + separator +
           QString::number(interfaceIndex);
}

bool ZeroconfRecordAggregator::update(const ZeroconfRecord& record,
                                      quint32 interfaceIndex)
{
    const QString logicalKey = serviceKey(record);
    ZeroconfRecord before;
    const bool hadRecord = recordForService(logicalKey, before);

    ResolvedRecord resolved;
    resolved.record = record;
    resolved.record.interfaceIndex = interfaceIndex;
    resolved.serviceKey = logicalKey;
    resolved.revision = ++m_NextRevision;
    m_ResolvedRecords.insert(resolverKey(record, interfaceIndex), resolved);
    if (!hadRecord) {
        m_ServiceOrder.append(logicalKey);
    }

    ZeroconfRecord after;
    const bool hasRecord = recordForService(logicalKey, after);
    return hadRecord != hasRecord ||
           (hasRecord && !sameResolvedRecord(before, after));
}

bool ZeroconfRecordAggregator::remove(const ZeroconfRecord& record,
                                      quint32 interfaceIndex)
{
    const QString logicalKey = serviceKey(record);
    ZeroconfRecord before;
    const bool hadRecord = recordForService(logicalKey, before);

    if (m_ResolvedRecords.remove(resolverKey(record, interfaceIndex)) == 0) {
        return false;
    }

    ZeroconfRecord after;
    const bool hasRecord = recordForService(logicalKey, after);
    if (!hasRecord) {
        m_ServiceOrder.removeAll(logicalKey);
    }
    return hadRecord != hasRecord ||
           (hasRecord && !sameResolvedRecord(before, after));
}

void ZeroconfRecordAggregator::clear()
{
    m_ResolvedRecords.clear();
    m_ServiceOrder.clear();
    m_NextRevision = 0;
}

bool ZeroconfRecordAggregator::isEmpty() const
{
    return m_ResolvedRecords.isEmpty();
}

QList<ZeroconfRecord> ZeroconfRecordAggregator::records() const
{
    QList<ZeroconfRecord> result;
    for (const QString& key : m_ServiceOrder) {
        ZeroconfRecord record;
        if (recordForService(key, record)) {
            result.append(record);
        }
    }
    return result;
}

bool ZeroconfRecordAggregator::recordForService(
    const QString& key, ZeroconfRecord& record) const
{
    const ResolvedRecord* selected = nullptr;
    for (auto it = m_ResolvedRecords.constBegin();
         it != m_ResolvedRecords.constEnd(); ++it) {
        const ResolvedRecord& candidate = it.value();
        if (candidate.serviceKey == key &&
            (selected == nullptr || candidate.revision > selected->revision)) {
            selected = &candidate;
        }
    }
    if (selected == nullptr) {
        return false;
    }
    record = selected->record;
    return true;
}

ZeroconfBrowser::ZeroconfBrowser(QObject* parent) :
    QObject(parent),
    m_DnsServiceRef(nullptr),
    m_pSocket(nullptr)
{
}

ZeroconfBrowser::~ZeroconfBrowser()
{
    m_ResolverRetryTokens.clear();
    m_BrowsedServices.clear();
    while (!m_Resolvers.isEmpty()) {
        stopResolver(m_Resolvers.constBegin().key());
    }
    delete m_pSocket;
    if (m_DnsServiceRef != nullptr) {
        DNSServiceRefDeallocate(m_DnsServiceRef);
    }
}

void ZeroconfBrowser::browseForType(const QString& type)
{
    const bool hadRecords = !m_RecordAggregator.isEmpty();
    m_BrowseBatchPending = false;
    m_PublishPending = false;
    m_BrowseCallbackError = kDNSServiceErr_NoError;
    m_ResolverRetryTokens.clear();
    m_BrowsedServices.clear();
    while (!m_Resolvers.isEmpty()) {
        stopResolver(m_Resolvers.constBegin().key());
    }
    m_RecordAggregator.clear();
    delete m_pSocket;
    m_pSocket = nullptr;
    if (m_DnsServiceRef != nullptr) {
        DNSServiceRefDeallocate(m_DnsServiceRef);
        m_DnsServiceRef = nullptr;
    }
    m_BrowsingType = type;
    if (hadRecords) {
        QPointer<ZeroconfBrowser> guard(this);
        publishRecords();
        if (guard.isNull()) {
            return;
        }
    }

    const DNSServiceErrorType err = DNSServiceBrowse(
        &m_DnsServiceRef, 0, 0, type.toUtf8().constData(), nullptr,
        browseReply, this);
    if (err != kDNSServiceErr_NoError) {
        emit error(err);
        return;
    }

    const int socketFd = DNSServiceRefSockFD(m_DnsServiceRef);
    if (socketFd == -1) {
        DNSServiceRefDeallocate(m_DnsServiceRef);
        m_DnsServiceRef = nullptr;
        emit error(kDNSServiceErr_Invalid);
        return;
    }
    m_pSocket = new QSocketNotifier(
        socketFd, QSocketNotifier::Read, this);
    connect(m_pSocket, SIGNAL(activated(int)), this,
            SLOT(socketReadyRead()));
}

void ZeroconfBrowser::socketReadyRead()
{
    QPointer<ZeroconfBrowser> guard(this);
    const DNSServiceErrorType err =
        DNSServiceProcessResult(m_DnsServiceRef);
    if (guard.isNull()) {
        return;
    }
    const DNSServiceErrorType callbackError = m_BrowseCallbackError;
    if (err == kDNSServiceErr_NoError &&
        callbackError == kDNSServiceErr_NoError) {
        return;
    }

    finishBrowseFailure(callbackError == kDNSServiceErr_NoError
                            ? err
                            : callbackError);
}

void ZeroconfBrowser::finishBrowseFailure(DNSServiceErrorType errorCode)
{
    m_ResolverRetryTokens.clear();
    m_BrowsedServices.clear();
    while (!m_Resolvers.isEmpty()) {
        stopResolver(m_Resolvers.constBegin().key());
    }
    m_RecordAggregator.clear();
    delete m_pSocket;
    m_pSocket = nullptr;
    if (m_DnsServiceRef != nullptr) {
        DNSServiceRefDeallocate(m_DnsServiceRef);
    }
    m_DnsServiceRef = nullptr;
    m_BrowseBatchPending = false;
    m_PublishPending = false;
    m_BrowseCallbackError = kDNSServiceErr_NoError;
    QPointer<ZeroconfBrowser> guard(this);
    emit error(errorCode);
    if (!guard.isNull()) {
        publishRecords();
    }
}

void ZeroconfBrowser::resolverSocketReadyRead()
{
    QSocketNotifier* socket =
        qobject_cast<QSocketNotifier*>(sender());
    for (Resolver* resolver : m_Resolvers) {
        if (resolver->socket == socket) {
            resolverReadyRead(resolver);
            return;
        }
    }
}

void ZeroconfBrowser::startResolve(const ZeroconfRecord& record,
                                   quint32 interfaceIndex)
{
    const QString key =
        ZeroconfRecordAggregator::resolverKey(record, interfaceIndex);
    if (m_Resolvers.contains(key)) {
        m_ResolverRetryTokens.remove(key);
        return;
    }

    Resolver* resolver = new Resolver;
    resolver->browser = this;
    resolver->key = key;
    resolver->record = record;
    resolver->interfaceIndex = interfaceIndex;
    const QByteArray name = record.serviceName.toUtf8();
    const QByteArray type = record.registeredType.toUtf8();
    const QByteArray domain = record.replyDomain.toUtf8();
    const DNSServiceErrorType err = DNSServiceResolve(
        &resolver->serviceRef, 0, interfaceIndex, name.constData(),
        type.constData(), domain.constData(), resolveReply, resolver);
    if (err != kDNSServiceErr_NoError) {
        delete resolver;
        scheduleResolverRetry(key);
        return;
    }

    const int socketFd = DNSServiceRefSockFD(resolver->serviceRef);
    if (socketFd == -1) {
        DNSServiceRefDeallocate(resolver->serviceRef);
        delete resolver;
        scheduleResolverRetry(key);
        return;
    }
    resolver->socket = new QSocketNotifier(
        socketFd, QSocketNotifier::Read, this);
    connect(resolver->socket, SIGNAL(activated(int)), this,
            SLOT(resolverSocketReadyRead()));
    m_Resolvers.insert(key, resolver);
    m_ResolverRetryTokens.remove(key);
}

void ZeroconfBrowser::scheduleResolverRetry(const QString& key)
{
    if (!m_BrowsedServices.contains(key)) {
        return;
    }

    const quint64 token = ++m_NextResolverRetryToken;
    m_ResolverRetryTokens.insert(key, token);
    QTimer::singleShot(
        kResolverRetryMilliseconds, this,
        [this, key, token]() {
            const auto tokenIt = m_ResolverRetryTokens.constFind(key);
            if (tokenIt == m_ResolverRetryTokens.constEnd() ||
                tokenIt.value() != token) {
                return;
            }
            m_ResolverRetryTokens.remove(key);

            const auto serviceIt = m_BrowsedServices.constFind(key);
            if (serviceIt == m_BrowsedServices.constEnd() ||
                m_Resolvers.contains(key)) {
                return;
            }
            const BrowsedService service = serviceIt.value();
            startResolve(service.record, service.interfaceIndex);
        });
}

DNSServiceErrorType ZeroconfBrowser::startTxtMonitor(Resolver* resolver)
{
    delete resolver->socket;
    resolver->socket = nullptr;
    DNSServiceRefDeallocate(resolver->serviceRef);
    resolver->serviceRef = nullptr;
    resolver->needsTxtMonitor = false;

    const QByteArray fullName = resolver->fullName.toUtf8();
    const DNSServiceErrorType err = DNSServiceQueryRecord(
        &resolver->serviceRef, 0, resolver->interfaceIndex,
        fullName.constData(), kDNSServiceType_TXT, kDNSServiceClass_IN,
        queryReply, resolver);
    if (err != kDNSServiceErr_NoError) {
        return err;
    }
    const int socketFd = DNSServiceRefSockFD(resolver->serviceRef);
    if (socketFd == -1) {
        DNSServiceRefDeallocate(resolver->serviceRef);
        resolver->serviceRef = nullptr;
        return kDNSServiceErr_Invalid;
    }
    resolver->socket = new QSocketNotifier(
        socketFd, QSocketNotifier::Read, this);
    connect(resolver->socket, SIGNAL(activated(int)), this,
            SLOT(resolverSocketReadyRead()));
    return kDNSServiceErr_NoError;
}

DNSServiceErrorType ZeroconfBrowser::parseTxt(
    quint16 length, const void* bytes, QMap<QString, QByteArray>& txt)
{
    txt.clear();
    const quint16 count = TXTRecordGetCount(length, bytes);
    for (quint16 index = 0; index < count; ++index) {
        char key[256] = {};
        quint8 valueLength = 0;
        const void* value = nullptr;
        const DNSServiceErrorType itemError = TXTRecordGetItemAtIndex(
            length, bytes, index, sizeof(key), key, &valueLength, &value);
        if (itemError != kDNSServiceErr_NoError) {
            txt.clear();
            return itemError;
        }
        txt.insert(QString::fromLatin1(key).toLower(),
                   QByteArray(static_cast<const char*>(value),
                              valueLength));
    }
    return kDNSServiceErr_NoError;
}

bool ZeroconfBrowser::stopResolver(const QString& key)
{
    Resolver* resolver = m_Resolvers.take(key);
    if (resolver == nullptr) {
        return false;
    }
    const bool publishPending = resolver->txtPublishPending;
    const bool recordsChanged =
        m_RecordAggregator.remove(resolver->record,
                                  resolver->interfaceIndex);
    delete resolver->socket;
    if (resolver->serviceRef != nullptr) {
        DNSServiceRefDeallocate(resolver->serviceRef);
    }
    delete resolver;
    return recordsChanged || publishPending;
}

void ZeroconfBrowser::resolverFailed(const QString& key)
{
    const bool recordsChanged = stopResolver(key);
    if (recordsChanged) {
        QPointer<ZeroconfBrowser> guard(this);
        publishRecords();
        if (guard.isNull()) {
            return;
        }
    }
    // The browse entry is still present. DNSServiceBrowse does not promise a
    // second Add after a transient resolver/TXT failure, so retry this one
    // interface without tearing down healthy siblings or the whole browser.
    scheduleResolverRetry(key);
}

void ZeroconfBrowser::resolverReadyRead(Resolver* resolver)
{
    const QString key = resolver->key;
    QPointer<ZeroconfBrowser> guard(this);
    DNSServiceErrorType processingError =
        DNSServiceProcessResult(resolver->serviceRef);
    if (guard.isNull()) {
        return;
    }

    Resolver* activeResolver = m_Resolvers.value(key, nullptr);
    if (activeResolver == nullptr) {
        return;
    }
    if (!activeResolver->failed &&
        processingError == kDNSServiceErr_NoError &&
        activeResolver->needsTxtMonitor) {
        processingError = startTxtMonitor(activeResolver);
        if (processingError == kDNSServiceErr_NoError) {
            return;
        }
        activeResolver->failed = true;
    }
    if (!activeResolver->failed &&
        processingError == kDNSServiceErr_NoError) {
        return;
    }

    resolverFailed(key);
}

void ZeroconfBrowser::publishRecords()
{
    if (m_BrowseBatchPending) {
        m_PublishPending = true;
        return;
    }
    m_PublishPending = false;
    const QList<ZeroconfRecord> records = m_RecordAggregator.records();
    emit currentRecordsChanged(records);
}

void ZeroconfBrowser::browseReply(
    DNSServiceRef, DNSServiceFlags flags, quint32 interfaceIndex,
    DNSServiceErrorType errorCode, const char* serviceName,
    const char* regType, const char* replyDomain, void* context)
{
    ZeroconfBrowser* browser = static_cast<ZeroconfBrowser*>(context);
    if (errorCode != kDNSServiceErr_NoError) {
        browser->m_BrowseCallbackError = errorCode;
        return;
    }

    browser->m_BrowseBatchPending =
        (flags & kDNSServiceFlagsMoreComing) != 0;
    const ZeroconfRecord record(serviceName, regType, replyDomain);
    const QString resolverKey =
        ZeroconfRecordAggregator::resolverKey(record, interfaceIndex);
    if ((flags & kDNSServiceFlagsAdd) != 0) {
        BrowsedService service;
        service.record = record;
        service.interfaceIndex = interfaceIndex;
        browser->m_BrowsedServices.insert(resolverKey, service);
        QPointer<ZeroconfBrowser> guard(browser);
        browser->startResolve(record, interfaceIndex);
        if (guard.isNull()) {
            return;
        }
        if (!browser->m_BrowseBatchPending && browser->m_PublishPending) {
            browser->publishRecords();
        }
        return;
    }

    browser->m_BrowsedServices.remove(resolverKey);
    browser->m_ResolverRetryTokens.remove(resolverKey);
    const bool recordsChanged = browser->stopResolver(
        resolverKey);
    if (recordsChanged ||
        (!browser->m_BrowseBatchPending && browser->m_PublishPending)) {
        browser->publishRecords();
    }
}

void ZeroconfBrowser::resolveReply(
    DNSServiceRef, DNSServiceFlags flags, quint32,
    DNSServiceErrorType errorCode, const char* fullName,
    const char* hostTarget, quint16 port, quint16 txtLength,
    const unsigned char* txtRecord, void* context)
{
    Resolver* resolver = static_cast<Resolver*>(context);
    ZeroconfBrowser* browser = resolver->browser;
    if (resolver->failed) {
        return;
    }
    if (errorCode != kDNSServiceErr_NoError) {
        resolver->failed = true;
        return;
    }

    QMap<QString, QByteArray> txt;
    const DNSServiceErrorType txtError =
        parseTxt(txtLength, txtRecord, txt);
    if (txtError != kDNSServiceErr_NoError) {
        resolver->failed = true;
        return;
    }

    ZeroconfRecord resolved = resolver->record;
    resolved.hostName = QString::fromUtf8(hostTarget);
    resolved.port = qFromBigEndian(port);
    resolved.interfaceIndex = resolver->interfaceIndex;
    resolved.txt = txt;
    resolver->record = resolved;
    resolver->fullName = QString::fromUtf8(fullName);
    resolver->needsTxtMonitor = true;

    const bool recordsChanged = browser->m_RecordAggregator.update(
        resolved, resolver->interfaceIndex);
    resolver->resolvePublishPending =
        resolver->resolvePublishPending || recordsChanged;
    if ((flags & kDNSServiceFlagsMoreComing) == 0 &&
        resolver->resolvePublishPending) {
        resolver->resolvePublishPending = false;
        browser->publishRecords();
    }
}

void ZeroconfBrowser::queryReply(
    DNSServiceRef, DNSServiceFlags flags, quint32,
    DNSServiceErrorType errorCode, const char*, quint16 rrtype,
    quint16 rrclass, quint16 rdlen, const void* rdata, quint32,
    void* context)
{
    Resolver* resolver = static_cast<Resolver*>(context);
    ZeroconfBrowser* browser = resolver->browser;
    if (resolver->failed) {
        return;
    }
    if (errorCode != kDNSServiceErr_NoError ||
        rrtype != kDNSServiceType_TXT ||
        rrclass != kDNSServiceClass_IN) {
        resolver->failed = true;
        return;
    }

    QMap<QString, QByteArray> txt;
    const DNSServiceErrorType txtError = parseTxt(rdlen, rdata, txt);
    if (txtError != kDNSServiceErr_NoError) {
        resolver->failed = true;
        return;
    }

    bool recordsChanged = false;
    if ((flags & kDNSServiceFlagsAdd) != 0) {
        if (txt != resolver->record.txt) {
            resolver->record.txt = txt;
            recordsChanged = browser->m_RecordAggregator.update(
                resolver->record, resolver->interfaceIndex);
        }
    }
    else if (txt == resolver->record.txt) {
        // A cache-flush update can add the replacement rdata before it
        // retires the old rdata.  Only removal of the value still selected by
        // this resolver means the TXT RRset is now empty.  Retire this
        // interface's candidate rather than making incomplete metadata the
        // newest logical record; a healthy sibling can then remain selected.
        resolver->record.txt.clear();
        recordsChanged = browser->m_RecordAggregator.remove(
            resolver->record, resolver->interfaceIndex);
    }

    resolver->txtPublishPending =
        resolver->txtPublishPending || recordsChanged;
    if ((flags & kDNSServiceFlagsMoreComing) != 0) {
        return;
    }
    if (resolver->txtPublishPending) {
        resolver->txtPublishPending = false;
        browser->publishRecords();
    }
}
