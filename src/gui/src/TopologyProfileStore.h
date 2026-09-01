/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "FreeformLayoutSettings.h"
#include "barrier/DisplayTopology.h"

#include <QString>
#include <QStringList>

#include <map>
#include <string>

class QSettings;
class QTextStream;

namespace barrier {

struct TopologyProfile {
    DisplayTopology topology;
    FreeformPositions positions;
    FreeformDisplayRects displayRects;
};

using TopologyProfiles = std::map<std::string, TopologyProfile>;

enum class TopologyProfileStoreResult {
    Ok,
    Malformed,
    IoError
};

struct TopologyProfileSelection {
    FreeformPositions positions;
    FreeformDisplayRects displayRects;
    bool saved{false};
};

bool putTopologyProfile(TopologyProfiles& profiles,
                        const TopologyProfile& profile,
                        QString* error = nullptr);

bool restrictTopologyProfileToScreens(
    TopologyProfile& profile,
    const QStringList& screenNames,
    QString* error = nullptr);

bool reconcileTopologyProfilesToScreens(
    TopologyProfiles& profiles,
    const QStringList& screenNames,
    QString* error = nullptr);

TopologyProfileSelection selectTopologyProfile(
    const TopologyProfiles& profiles,
    const DisplayTopology& topology,
    const QString& serverName,
    const FreeformPositions& legacyPositions,
    const FreeformDisplayRects& legacyDisplayRects);

void writeTopologyProfiles(QTextStream& stream,
                           const TopologyProfiles& profiles);

class TopologyProfileStore {
public:
    static TopologyProfileStoreResult load(
        QSettings& settings,
        TopologyProfiles& profiles,
        QString* error = nullptr);
    static TopologyProfileStoreResult save(
        QSettings& settings,
        const TopologyProfiles& profiles,
        QString* error = nullptr);
};

} // namespace barrier
