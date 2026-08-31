/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "ClientEndpointPolicy.h"

namespace barrier {

ClientEndpointSelection selectClientEndpoint(
    bool proximityGatingEnabled,
    bool autoConfigEnabled,
    const QString& configuredHost,
    const QString& discoveredEndpoint,
    int dataPort)
{
    if (proximityGatingEnabled && discoveredEndpoint.isEmpty()) {
        return {ClientEndpointStatus::WaitingForProximity, QString()};
    }
    if (autoConfigEnabled && !discoveredEndpoint.isEmpty()) {
        return {ClientEndpointStatus::Ready, discoveredEndpoint};
    }
    if (configuredHost.isEmpty()) {
        return {ClientEndpointStatus::MissingServerAddress, QString()};
    }
    return {
        ClientEndpointStatus::Ready,
        QStringLiteral("[%1]:%2").arg(configuredHost).arg(dataPort)
    };
}

} // namespace barrier
