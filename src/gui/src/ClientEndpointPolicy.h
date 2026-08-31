/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include <QtCore/QString>

namespace barrier {

enum class ClientEndpointStatus {
    Ready,
    WaitingForProximity,
    MissingServerAddress
};

struct ClientEndpointSelection {
    ClientEndpointStatus status{ClientEndpointStatus::MissingServerAddress};
    QString endpoint;
};

ClientEndpointSelection selectClientEndpoint(
    bool proximityGatingEnabled,
    bool autoConfigEnabled,
    const QString& configuredHost,
    const QString& discoveredEndpoint,
    int dataPort);

} // namespace barrier
