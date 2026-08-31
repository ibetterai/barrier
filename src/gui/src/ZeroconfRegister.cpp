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

#include "ZeroconfRegister.h"

#include <QtCore/QPointer>
#include <QtCore/QSocketNotifier>

namespace {

bool encodeTxt(const QMap<QString, QByteArray>& txt,
               QMap<QString, QByteArray>& normalizedTxt,
               QByteArray& encoded,
               DNSServiceErrorType& error)
{
    normalizedTxt.clear();
    for (auto it = txt.constBegin(); it != txt.constEnd(); ++it) {
        const QString normalizedKey = it.key().toLower();
        bool validKey = !normalizedKey.isEmpty();
        for (const QChar character : it.key()) {
            validKey = validKey &&
                       character.unicode() >= 0x20 &&
                       character.unicode() <= 0x7e &&
                       character != QLatin1Char('=');
        }
        if (!validKey || normalizedTxt.contains(normalizedKey) ||
            it.value().size() > 255) {
            error = kDNSServiceErr_BadParam;
            return false;
        }
        normalizedTxt.insert(normalizedKey, it.value());
    }

    TXTRecordRef txtRecord;
    TXTRecordCreate(&txtRecord, 0, nullptr);
    for (auto it = normalizedTxt.constBegin();
         it != normalizedTxt.constEnd(); ++it) {
        const QByteArray key = it.key().toLatin1();
        const QByteArray value = it.value();
        error = TXTRecordSetValue(
            &txtRecord, key.constData(), static_cast<quint8>(value.size()),
            value.constData());
        if (error != kDNSServiceErr_NoError) {
            TXTRecordDeallocate(&txtRecord);
            return false;
        }
    }
    encoded = QByteArray(
        static_cast<const char*>(TXTRecordGetBytesPtr(&txtRecord)),
        TXTRecordGetLength(&txtRecord));
    TXTRecordDeallocate(&txtRecord);
    error = kDNSServiceErr_NoError;
    return true;
}

} // namespace

ZeroconfRegister::ZeroconfRegister(QObject* parent) :
    QObject(parent),
    m_DnsServiceRef(nullptr),
    m_pSocket(nullptr),
    m_registrationFailed(false)
{
}

ZeroconfRegister::~ZeroconfRegister()
{
    if (m_pSocket) {
        delete m_pSocket;
    }

    if (m_DnsServiceRef) {
        DNSServiceRefDeallocate(m_DnsServiceRef);
        m_DnsServiceRef = 0;
    }
}

void ZeroconfRegister::registerService(
    const ZeroconfRecord& record,
    quint16 servicePort,
    const QMap<QString, QByteArray>& txt)
{
    if (m_DnsServiceRef) {
        qWarning("Warning: Already registered a service for this object");
        return;
    }
    m_registrationFailed = false;

    QMap<QString, QByteArray> normalizedTxt;
    QByteArray encodedTxt;
    DNSServiceErrorType txtError = kDNSServiceErr_NoError;
    if (!encodeTxt(txt, normalizedTxt, encodedTxt, txtError)) {
        emit error(txtError);
        return;
    }

    quint16 bigEndianPort = servicePort;
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    bigEndianPort = static_cast<quint16>(
        ((servicePort & 0x00ff) << 8) |
        ((servicePort & 0xff00) >> 8));
#endif

    const DNSServiceErrorType err = DNSServiceRegister(
        &m_DnsServiceRef, kDNSServiceFlagsNoAutoRename, 0,
        record.serviceName.toUtf8().constData(),
        record.registeredType.toUtf8().constData(),
        record.replyDomain.isEmpty()
            ? nullptr
            : record.replyDomain.toUtf8().constData(),
        nullptr, bigEndianPort, static_cast<quint16>(encodedTxt.size()),
        encodedTxt.constData(), registerService, this);

    if (err != kDNSServiceErr_NoError) {
        if (m_DnsServiceRef != nullptr) {
            DNSServiceRefDeallocate(m_DnsServiceRef);
            m_DnsServiceRef = nullptr;
        }
        emit error(err);
        return;
    }

    finalRecord = record;
    finalRecord.port = servicePort;
    finalRecord.txt = normalizedTxt;
    const int sockfd = DNSServiceRefSockFD(m_DnsServiceRef);
    if (sockfd == -1) {
        DNSServiceRefDeallocate(m_DnsServiceRef);
        m_DnsServiceRef = nullptr;
        emit error(kDNSServiceErr_Invalid);
        return;
    }
    m_pSocket = new QSocketNotifier(sockfd, QSocketNotifier::Read, this);
    connect(m_pSocket, SIGNAL(activated(int)), this, SLOT(socketReadyRead()));
}

bool ZeroconfRegister::updateTxt(
    const QMap<QString, QByteArray>& txt)
{
    QMap<QString, QByteArray> normalizedTxt;
    QByteArray encodedTxt;
    DNSServiceErrorType txtError = kDNSServiceErr_NoError;
    if (!encodeTxt(txt, normalizedTxt, encodedTxt, txtError)) {
        emit error(txtError);
        return false;
    }
    if (m_DnsServiceRef != nullptr) {
        txtError = DNSServiceUpdateRecord(
            m_DnsServiceRef, nullptr, 0,
            static_cast<quint16>(encodedTxt.size()),
            encodedTxt.constData(), 0);
        if (txtError != kDNSServiceErr_NoError) {
            emit error(txtError);
            return false;
        }
    }
    finalRecord.txt = normalizedTxt;
    return true;
}

void ZeroconfRegister::socketReadyRead()
{
    QPointer<ZeroconfRegister> guard(this);
    const DNSServiceErrorType err =
        DNSServiceProcessResult(m_DnsServiceRef);
    if (guard.isNull() ||
        (!m_registrationFailed && err == kDNSServiceErr_NoError)) {
        return;
    }

    delete m_pSocket;
    m_pSocket = nullptr;
    DNSServiceRefDeallocate(m_DnsServiceRef);
    m_DnsServiceRef = nullptr;
    if (!m_registrationFailed) {
        emit error(err);
    }
}

void ZeroconfRegister::registerService(
    DNSServiceRef, DNSServiceFlags, DNSServiceErrorType errorCode,
    const char* name, const char* regtype, const char* domain, void* data)
{
    ZeroconfRegister* serviceRegister =
        static_cast<ZeroconfRegister*>(data);
    if (errorCode != kDNSServiceErr_NoError) {
        serviceRegister->m_registrationFailed = true;
        emit serviceRegister->error(errorCode);
        return;
    }

    serviceRegister->finalRecord.serviceName = QString::fromUtf8(name);
    serviceRegister->finalRecord.registeredType = QString::fromUtf8(regtype);
    serviceRegister->finalRecord.replyDomain = QString::fromUtf8(domain);
    emit serviceRegister->serviceRegistered(serviceRegister->finalRecord);
}
