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

#include <QtCore/QMetaType>
#include <QtCore/QByteArray>
#include <QtCore/QMap>
#include <QtCore/QString>

class ZeroconfRecord
{
public:
    ZeroconfRecord() {}
    ZeroconfRecord(const QString& name, const QString& regType,
                   const QString& domain)
        : serviceName(name), registeredType(regType), replyDomain(domain)
    {}
    ZeroconfRecord(const char* name, const char* regType, const char* domain)
    {
        serviceName = QString::fromUtf8(name);
        registeredType = QString::fromUtf8(regType);
        replyDomain = QString::fromUtf8(domain);
    }

    bool operator==(const ZeroconfRecord& other) const {
        return serviceName == other.serviceName
            && registeredType == other.registeredType
            && replyDomain == other.replyDomain;
    }

    bool matchesProximityServer(const QString& proximityId) const
    {
        if (!isValidProximityId(proximityId)) {
            return false;
        }
        const auto found = txt.constFind(QStringLiteral("proximity-id"));
        return found != txt.constEnd() &&
               isValidProximityId(found.value()) &&
               found.value() == proximityId.toLatin1();
    }

    bool matchesPairedClient(const QString& serverProximityId) const
    {
        if (!isValidProximityId(serverProximityId)) {
            return false;
        }
        const auto found = txt.constFind(
            QStringLiteral("paired-server-id"));
        return found != txt.constEnd() &&
               isValidProximityId(found.value()) &&
               found.value() == serverProximityId.toLatin1();
    }

    QString clientRoutingId() const
    {
        const QByteArray value = txt.value(
            QStringLiteral("client-routing-id"));
        return isValidProximityId(value)
            ? QString::fromLatin1(value)
            : QString();
    }

    bool isCurrentLocalWakeRoute(const QString& serverProximityId) const
    {
        return matchesPairedClient(serverProximityId) &&
               hasResolvedService() && interfaceIndex != 0 &&
               replyDomain.compare(
                   QStringLiteral("local."), Qt::CaseInsensitive) == 0 &&
               hostName.endsWith(
                   QStringLiteral(".local."), Qt::CaseInsensitive);
    }

    bool matchesCurrentWakeResolution(
        const QString& resolvedHost,
        const QByteArray& advertisedServerProximityId) const
    {
        const QByteArray expected =
            txt.value(QStringLiteral("paired-server-id"));
        return isValidProximityId(expected) &&
               isValidProximityId(advertisedServerProximityId) &&
               advertisedServerProximityId == expected &&
               resolvedHost.compare(hostName, Qt::CaseInsensitive) == 0 &&
               resolvedHost.endsWith(
                   QStringLiteral(".local."), Qt::CaseInsensitive);
    }

    bool isDisplayReady() const
    {
        return txt.value(QStringLiteral("display-ready")) ==
               QByteArrayLiteral("1");
    }

    bool hasResolvedService() const
    {
        return !hostName.isEmpty() && port != 0;
    }

    QString barrierEndpoint(int dataPort) const
    {
        if (!hasResolvedService() || dataPort <= 0 || dataPort > 65535) {
            return QString();
        }
        return QStringLiteral("[%1]:%2").arg(hostName).arg(dataPort);
    }

private:
    static bool isValidProximityId(const QString& id)
    {
        if (id.size() != 32) {
            return false;
        }
        for (const QChar character : id) {
            const bool digit = character >= QLatin1Char('0') &&
                               character <= QLatin1Char('9');
            const bool hex = character >= QLatin1Char('a') &&
                             character <= QLatin1Char('f');
            if (!digit && !hex) {
                return false;
            }
        }
        return true;
    }

    static bool isValidProximityId(const QByteArray& id)
    {
        if (id.size() != 32) {
            return false;
        }
        for (const char character : id) {
            const bool digit = character >= '0' && character <= '9';
            const bool hex = character >= 'a' && character <= 'f';
            if (!digit && !hex) {
                return false;
            }
        }
        return true;
    }

public:
    QString serviceName;
    QString registeredType;
    QString replyDomain;
    QString hostName;
    // DNS-SD SRV port for the auxiliary discovery service. This is not the
    // Barrier protocol port passed to barrierc.
    quint16 port = 0;
    // Preserve the browse callback's concrete interface. Apple's targeted
    // WakeOnResolve rejects the catch-all interface index.
    quint32 interfaceIndex = 0;
    QMap<QString, QByteArray> txt;
};

Q_DECLARE_METATYPE(ZeroconfRecord)
