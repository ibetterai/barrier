/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "ZeroconfRecord.h"

#include <QtCore/QList>
#include <QtCore/QMap>

namespace barrier {

// Tracks the endpoint portion of each complete browse snapshot. Browser
// publications can legitimately change TXT data or the selected interface;
// neither is a new host:port worth repeating in the user log.
class ZeroconfEndpointLogPolicy
{
public:
    QList<ZeroconfRecord> newOrChangedEndpoints(
        const QList<ZeroconfRecord>& snapshot)
    {
        QMap<ServiceIdentity, Endpoint> nextEndpoints;
        QList<ZeroconfRecord> changed;
        for (const ZeroconfRecord& record : snapshot) {
            if (!record.hasResolvedService()) {
                continue;
            }

            const ServiceIdentity identity(record);
            const Endpoint endpoint(record);
            const auto previous = m_ActiveEndpoints.constFind(identity);
            if (previous == m_ActiveEndpoints.constEnd() ||
                !(previous.value() == endpoint)) {
                changed.append(record);
            }
            nextEndpoints.insert(identity, endpoint);
        }
        m_ActiveEndpoints.swap(nextEndpoints);
        return changed;
    }

private:
    struct ServiceIdentity {
        explicit ServiceIdentity(const ZeroconfRecord& record) :
            serviceName(record.serviceName),
            registeredType(record.registeredType),
            replyDomain(record.replyDomain)
        {
        }

        bool operator<(const ServiceIdentity& other) const
        {
            if (serviceName != other.serviceName) {
                return serviceName < other.serviceName;
            }
            if (registeredType != other.registeredType) {
                return registeredType < other.registeredType;
            }
            return replyDomain < other.replyDomain;
        }

        QString serviceName;
        QString registeredType;
        QString replyDomain;
    };

    struct Endpoint {
        explicit Endpoint(const ZeroconfRecord& record) :
            hostName(record.hostName),
            port(record.port)
        {
        }

        bool operator==(const Endpoint& other) const
        {
            return hostName == other.hostName && port == other.port;
        }

        QString hostName;
        quint16 port;
    };

    QMap<ServiceIdentity, Endpoint> m_ActiveEndpoints;
};

} // namespace barrier
