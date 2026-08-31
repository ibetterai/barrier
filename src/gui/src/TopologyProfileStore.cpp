/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "TopologyProfileStore.h"

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QRect>
#include <QSettings>
#include <QTextStream>
#include <QVariant>

#include <stdexcept>
#include <utility>

namespace barrier {
namespace {

const quint32 kStoreMagic = 0x42545046; // BTPF
const quint32 kStoreVersion = 1;
const int kMaximumPayloadBytes = 1024 * 1024;
const quint32 kMaximumProfiles = 128;
const quint32 kMaximumDisplays = 32;
const quint32 kMaximumScreens = 128;
const quint32 kMaximumRectsPerScreen = 64;
const quint32 kMaximumProfileKeyBytes = 64;
const quint32 kMaximumStableIdBytes = 256;
const quint32 kMaximumScreenNameBytes = 1024;
const char kLivePayloadKey[] = "topologyProfiles/payload";
const char kStagedPayloadKey[] = "topologyProfiles.pending/payload";

void setError(QString* error, const QString& message)
{
    if (error != nullptr) {
        *error = message;
    }
}

bool writeBoundedBytes(QDataStream& stream,
                       const QByteArray& value,
                       quint32 maximum)
{
    if (value.size() < 0 || quint64(value.size()) > maximum) {
        return false;
    }
    stream << quint32(value.size());
    return value.isEmpty() ||
           stream.writeRawData(value.constData(), value.size()) == value.size();
}

bool readBoundedBytes(QDataStream& stream,
                      quint32 maximum,
                      QByteArray& value)
{
    quint32 size = 0;
    stream >> size;
    if (stream.status() != QDataStream::Ok || size > maximum ||
        stream.device() == nullptr ||
        quint64(stream.device()->bytesAvailable()) < size) {
        return false;
    }
    value.resize(int(size));
    return size == 0 ||
           stream.readRawData(value.data(), int(size)) == int(size);
}

bool writeBoundedString(QDataStream& stream,
                        const QString& value,
                        quint32 maximum)
{
    return writeBoundedBytes(stream, value.toUtf8(), maximum);
}

bool readBoundedString(QDataStream& stream,
                       quint32 maximum,
                       QString& value)
{
    QByteArray encoded;
    if (!readBoundedBytes(stream, maximum, encoded)) {
        return false;
    }
    value = QString::fromUtf8(encoded.constData(), encoded.size());
    return value.toUtf8() == encoded;
}

bool profileIsValid(const TopologyProfile& profile, QString* error)
{
    DisplayTopology normalized;
    try {
        normalized = profile.topology.normalized();
    }
    catch (const std::invalid_argument& exception) {
        setError(error, QString::fromUtf8(exception.what()));
        return false;
    }

    if (normalized.empty()) {
        setError(error, QStringLiteral("topology profile has no displays"));
        return false;
    }
    if (normalized.displays.size() > kMaximumDisplays ||
        profile.positions.size() > kMaximumScreens ||
        profile.displayRects.size() > kMaximumScreens) {
        setError(error, QStringLiteral("topology profile exceeds storage limits"));
        return false;
    }
    for (const DisplayTopologyEntry& display : normalized.displays) {
        if (display.stableId.empty() ||
            display.stableId.size() > kMaximumStableIdBytes) {
            setError(error, QStringLiteral("topology profile display identity is too long"));
            return false;
        }
    }
    if (profile.positions.empty() || profile.displayRects.empty() ||
        profile.positions.size() != profile.displayRects.size()) {
        setError(error, QStringLiteral("topology profile geometry is incomplete"));
        return false;
    }

    for (FreeformPositions::const_iterator position = profile.positions.begin();
         position != profile.positions.end(); ++position) {
        if (position->first.isEmpty() ||
            position->first.toUtf8().size() > int(kMaximumScreenNameBytes) ||
            profile.displayRects.find(position->first) == profile.displayRects.end()) {
            setError(error, QStringLiteral("topology profile screen geometry does not match"));
            return false;
        }
    }
    for (FreeformDisplayRects::const_iterator screen = profile.displayRects.begin();
         screen != profile.displayRects.end(); ++screen) {
        if (screen->first.isEmpty() ||
            screen->first.toUtf8().size() > int(kMaximumScreenNameBytes) ||
            screen->second.isEmpty() ||
            screen->second.size() > int(kMaximumRectsPerScreen) ||
            profile.positions.find(screen->first) == profile.positions.end()) {
            setError(error, QStringLiteral("topology profile display geometry is incomplete"));
            return false;
        }
        for (const QRect& rect : screen->second) {
            if (rect.width() <= 0 || rect.height() <= 0) {
                setError(error, QStringLiteral("topology profile contains an invalid rectangle"));
                return false;
            }
        }
    }
    return true;
}

QByteArray serializeProfiles(const TopologyProfiles& profiles, QString* error)
{
    if (profiles.size() > kMaximumProfiles) {
        setError(error, QStringLiteral("too many topology profiles"));
        return QByteArray();
    }

    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_5_9);
    stream << kStoreMagic << kStoreVersion << quint32(profiles.size());
    for (TopologyProfiles::const_iterator stored = profiles.begin();
         stored != profiles.end(); ++stored) {
        TopologyProfile profile = stored->second;
        if (!profileIsValid(profile, error)) {
            return QByteArray();
        }
        profile.topology = profile.topology.normalized();
        const std::string computedKey = profile.topology.profileKey();
        if (stored->first != computedKey) {
            setError(error, QStringLiteral("topology profile key does not match its displays"));
            return QByteArray();
        }

        if (!writeBoundedBytes(
                stream, QByteArray::fromStdString(stored->first),
                kMaximumProfileKeyBytes)) {
            setError(error, QStringLiteral("topology profile key is too long"));
            return QByteArray();
        }
        stream << quint32(profile.topology.displays.size());
        for (const DisplayTopologyEntry& display : profile.topology.displays) {
            if (!writeBoundedBytes(
                    stream, QByteArray::fromStdString(display.stableId),
                    kMaximumStableIdBytes)) {
                setError(error, QStringLiteral("topology display identity is too long"));
                return QByteArray();
            }
            stream << qint32(display.logicalBounds.x)
                   << qint32(display.logicalBounds.y)
                   << qint32(display.logicalBounds.w)
                   << qint32(display.logicalBounds.h)
                   << qint32(display.rotationDegrees)
                   << quint8(display.primary ? 1 : 0);
        }

        stream << quint32(profile.positions.size());
        for (FreeformPositions::const_iterator position = profile.positions.begin();
             position != profile.positions.end(); ++position) {
            if (!writeBoundedString(
                    stream, position->first, kMaximumScreenNameBytes)) {
                setError(error, QStringLiteral("topology screen name is too long"));
                return QByteArray();
            }
            stream << qint32(position->second.first)
                   << qint32(position->second.second);
        }

        stream << quint32(profile.displayRects.size());
        for (FreeformDisplayRects::const_iterator screen = profile.displayRects.begin();
             screen != profile.displayRects.end(); ++screen) {
            if (!writeBoundedString(
                    stream, screen->first, kMaximumScreenNameBytes)) {
                setError(error, QStringLiteral("topology screen name is too long"));
                return QByteArray();
            }
            stream << quint32(screen->second.size());
            for (const QRect& rect : screen->second) {
                stream << qint32(rect.x()) << qint32(rect.y())
                       << qint32(rect.width()) << qint32(rect.height());
            }
        }
        if (stream.status() != QDataStream::Ok ||
            payload.size() > kMaximumPayloadBytes) {
            setError(error, QStringLiteral("topology profile payload is too large"));
            return QByteArray();
        }
    }

    if (stream.status() != QDataStream::Ok || payload.size() > kMaximumPayloadBytes) {
        setError(error, QStringLiteral("topology profile payload is too large"));
        return QByteArray();
    }
    return payload;
}

bool readCount(QDataStream& stream, quint32 maximum, quint32& count)
{
    stream >> count;
    return stream.status() == QDataStream::Ok && count <= maximum;
}

bool deserializeProfiles(const QByteArray& payload,
                         TopologyProfiles& profiles,
                         QString* error)
{
    if (payload.isEmpty() || payload.size() > kMaximumPayloadBytes) {
        setError(error, QStringLiteral("topology profile payload has an invalid size"));
        return false;
    }

    QDataStream stream(payload);
    stream.setVersion(QDataStream::Qt_5_9);
    quint32 magic = 0;
    quint32 version = 0;
    quint32 profileCount = 0;
    stream >> magic >> version;
    if (stream.status() != QDataStream::Ok || magic != kStoreMagic ||
        version != kStoreVersion ||
        !readCount(stream, kMaximumProfiles, profileCount)) {
        setError(error, QStringLiteral("topology profile payload header is invalid"));
        return false;
    }

    TopologyProfiles parsed;
    for (quint32 profileIndex = 0; profileIndex < profileCount; ++profileIndex) {
        QByteArray encodedKey;
        quint32 displayCount = 0;
        if (!readBoundedBytes(
                stream, kMaximumProfileKeyBytes, encodedKey) ||
            encodedKey.size() != int(kMaximumProfileKeyBytes) ||
            !readCount(stream, kMaximumDisplays, displayCount) ||
            displayCount == 0) {
            setError(error, QStringLiteral("topology profile display list is invalid"));
            return false;
        }

        TopologyProfile profile;
        for (quint32 displayIndex = 0; displayIndex < displayCount; ++displayIndex) {
            QByteArray stableId;
            qint32 x = 0, y = 0, width = 0, height = 0, rotation = 0;
            quint8 primary = 0;
            if (!readBoundedBytes(
                    stream, kMaximumStableIdBytes, stableId) ||
                stableId.isEmpty()) {
                setError(error, QStringLiteral("topology profile display is invalid"));
                return false;
            }
            stream >> x >> y >> width >> height >> rotation >> primary;
            if (stream.status() != QDataStream::Ok || primary > 1) {
                setError(error, QStringLiteral("topology profile display is invalid"));
                return false;
            }
            profile.topology.displays.push_back({
                stableId.toStdString(),
                {x, y, width, height},
                rotation,
                primary == 1
            });
        }

        quint32 positionCount = 0;
        if (!readCount(stream, kMaximumScreens, positionCount) || positionCount == 0) {
            setError(error, QStringLiteral("topology profile positions are invalid"));
            return false;
        }
        for (quint32 positionIndex = 0; positionIndex < positionCount; ++positionIndex) {
            QString name;
            qint32 x = 0, y = 0;
            if (!readBoundedString(
                    stream, kMaximumScreenNameBytes, name) ||
                name.isEmpty() || profile.positions.count(name) != 0) {
                setError(error, QStringLiteral("topology profile position is invalid"));
                return false;
            }
            stream >> x >> y;
            if (stream.status() != QDataStream::Ok) {
                setError(error, QStringLiteral("topology profile position is invalid"));
                return false;
            }
            profile.positions[name] = std::make_pair(int(x), int(y));
        }

        quint32 screenCount = 0;
        if (!readCount(stream, kMaximumScreens, screenCount) ||
            screenCount == 0) {
            setError(error, QStringLiteral("topology profile rectangles are invalid"));
            return false;
        }
        for (quint32 screenIndex = 0; screenIndex < screenCount; ++screenIndex) {
            QString name;
            quint32 rectCount = 0;
            if (!readBoundedString(
                    stream, kMaximumScreenNameBytes, name) ||
                name.isEmpty() || profile.displayRects.count(name) != 0 ||
                !readCount(stream, kMaximumRectsPerScreen, rectCount) ||
                rectCount == 0) {
                setError(error, QStringLiteral("topology profile rectangle list is invalid"));
                return false;
            }
            QList<QRect> rects;
            for (quint32 rectIndex = 0; rectIndex < rectCount; ++rectIndex) {
                qint32 x = 0, y = 0, width = 0, height = 0;
                stream >> x >> y >> width >> height;
                if (stream.status() != QDataStream::Ok || width <= 0 || height <= 0) {
                    setError(error, QStringLiteral("topology profile rectangle is invalid"));
                    return false;
                }
                rects.append(QRect(x, y, width, height));
            }
            profile.displayRects[name] = rects;
        }

        try {
            profile.topology = profile.topology.normalized();
        }
        catch (const std::invalid_argument& exception) {
            setError(error, QString::fromUtf8(exception.what()));
            return false;
        }
        const std::string key = encodedKey.toStdString();
        if (profile.topology.profileKey() != key ||
            parsed.count(key) != 0 || !profileIsValid(profile, error)) {
            setError(error, QStringLiteral("topology profile is inconsistent"));
            return false;
        }
        parsed[key] = profile;
    }

    if (stream.status() != QDataStream::Ok || !stream.atEnd()) {
        setError(error, QStringLiteral("topology profile payload has trailing data"));
        return false;
    }
    profiles.swap(parsed);
    return true;
}

QString hexEncode(const QByteArray& value)
{
    return QString::fromLatin1(value.toHex());
}

} // namespace

bool putTopologyProfile(TopologyProfiles& profiles,
                        const TopologyProfile& profile,
                        QString* error)
{
    TopologyProfile normalized = profile;
    if (!profileIsValid(normalized, error)) {
        return false;
    }
    normalized.topology = normalized.topology.normalized();
    profiles[normalized.topology.profileKey()] = normalized;
    return true;
}

bool restrictTopologyProfileToScreens(
    TopologyProfile& profile,
    const QStringList& screenNames,
    QString* error)
{
    TopologyProfile restricted;
    restricted.topology = profile.topology;

    for (const QString& screenName : screenNames) {
        if (screenName.isEmpty() ||
            restricted.positions.count(screenName) != 0) {
            setError(error, QStringLiteral(
                "configured display names must be unique and non-empty"));
            return false;
        }

        FreeformPositions::const_iterator position =
            profile.positions.find(screenName);
        FreeformDisplayRects::const_iterator displayRects =
            profile.displayRects.find(screenName);
        if (position == profile.positions.end() ||
            displayRects == profile.displayRects.end()) {
            setError(error, QStringLiteral(
                "topology profile is missing geometry for configured display \"%1\"")
                .arg(screenName));
            return false;
        }

        restricted.positions.insert(*position);
        restricted.displayRects.insert(*displayRects);
    }

    if (!profileIsValid(restricted, error)) {
        return false;
    }
    profile = std::move(restricted);
    return true;
}

TopologyProfileSelection selectTopologyProfile(
    const TopologyProfiles& profiles,
    const DisplayTopology& topology,
    const QString& serverName,
    const FreeformPositions& legacyPositions,
    const FreeformDisplayRects& legacyDisplayRects)
{
    TopologyProfileSelection selection;
    DisplayTopology normalized;
    try {
        normalized = topology.normalized();
    }
    catch (const std::invalid_argument&) {
        return selection;
    }
    if (normalized.empty()) {
        return selection;
    }

    const std::string key = normalized.profileKey();
    TopologyProfiles::const_iterator stored = profiles.find(key);
    if (stored != profiles.end() &&
        stored->second.topology.normalized().canonicalIdentity() ==
            normalized.canonicalIdentity()) {
        selection.positions = stored->second.positions;
        selection.displayRects = stored->second.displayRects;
        selection.saved = true;
    }
    else {
        selection.positions = legacyPositions;
        selection.displayRects = legacyDisplayRects;
    }
    selection.positions[serverName] = std::make_pair(0, 0);
    int primaryX = 0;
    int primaryY = 0;
    for (const DisplayTopologyEntry& display : normalized.displays) {
        if (display.primary) {
            primaryX = display.logicalBounds.x;
            primaryY = display.logicalBounds.y;
            break;
        }
    }
    QList<QRect> exactServerDisplays;
    for (const DisplayTopologyEntry& display : normalized.displays) {
        if (display.primary) {
            exactServerDisplays.append(QRect(
                display.logicalBounds.x - primaryX,
                display.logicalBounds.y - primaryY,
                display.logicalBounds.w, display.logicalBounds.h));
        }
    }
    for (const DisplayTopologyEntry& display : normalized.displays) {
        if (!display.primary) {
            exactServerDisplays.append(QRect(
                display.logicalBounds.x - primaryX,
                display.logicalBounds.y - primaryY,
                display.logicalBounds.w, display.logicalBounds.h));
        }
    }
    selection.displayRects[serverName] = exactServerDisplays;
    return selection;
}

void writeTopologyProfiles(QTextStream& stream,
                           const TopologyProfiles& profiles)
{
    if (profiles.empty()) {
        return;
    }

    stream << "section: topology-profiles\n";
    stream << "\tversion = 1\n";
    for (TopologyProfiles::const_iterator stored = profiles.begin();
         stored != profiles.end(); ++stored) {
        const TopologyProfile& profile = stored->second;
        const DisplayTopology topology = profile.topology.normalized();
        stream << "\tprofile = " << QString::fromStdString(stored->first) << "\n";
        stream << "\t\ttopology-version = " << DisplayTopology::kIdentityVersion << "\n";
        for (const DisplayTopologyEntry& display : topology.displays) {
            stream << "\t\tdisplay = "
                   << hexEncode(QByteArray::fromStdString(display.stableId)) << " "
                   << display.logicalBounds.x << " "
                   << display.logicalBounds.y << " "
                   << display.logicalBounds.w << " "
                   << display.logicalBounds.h << " "
                   << display.rotationDegrees << " "
                   << (display.primary ? 1 : 0) << "\n";
        }
        for (FreeformPositions::const_iterator position = profile.positions.begin();
             position != profile.positions.end(); ++position) {
            stream << "\t\tposition = " << hexEncode(position->first.toUtf8())
                   << " " << position->second.first
                   << " " << position->second.second << "\n";
        }
        for (FreeformDisplayRects::const_iterator screen = profile.displayRects.begin();
             screen != profile.displayRects.end(); ++screen) {
            for (const QRect& rect : screen->second) {
                stream << "\t\trect = " << hexEncode(screen->first.toUtf8())
                       << " " << rect.x() << " " << rect.y()
                       << " " << rect.width() << " " << rect.height() << "\n";
            }
        }
        stream << "\tend-profile\n";
    }
    stream << "end\n\n";
}

TopologyProfileStoreResult TopologyProfileStore::load(
    QSettings& settings,
    TopologyProfiles& profiles,
    QString* error)
{
    if (settings.status() != QSettings::NoError) {
        setError(error, QStringLiteral("could not read topology profile settings"));
        return TopologyProfileStoreResult::IoError;
    }
    if (!settings.contains(kLivePayloadKey)) {
        profiles.clear();
        setError(error, QString());
        return TopologyProfileStoreResult::Ok;
    }

    const QByteArray payload = settings.value(kLivePayloadKey).toByteArray();
    if (settings.status() != QSettings::NoError) {
        setError(error, QStringLiteral("could not read topology profile payload"));
        return TopologyProfileStoreResult::IoError;
    }
    TopologyProfiles parsed;
    if (!deserializeProfiles(payload, parsed, error)) {
        return TopologyProfileStoreResult::Malformed;
    }
    profiles.swap(parsed);
    setError(error, QString());
    return TopologyProfileStoreResult::Ok;
}

TopologyProfileStoreResult TopologyProfileStore::save(
    QSettings& settings,
    const TopologyProfiles& profiles,
    QString* error)
{
    const QByteArray payload = serializeProfiles(profiles, error);
    if (payload.isEmpty() && !profiles.empty()) {
        return TopologyProfileStoreResult::Malformed;
    }

    const bool hadLivePayload = settings.contains(kLivePayloadKey);
    const QVariant previousPayload = settings.value(kLivePayloadKey);
    settings.remove("topologyProfiles.pending");
    settings.setValue(kStagedPayloadKey, payload);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        settings.remove("topologyProfiles.pending");
        setError(error, QStringLiteral("could not stage topology profiles"));
        return TopologyProfileStoreResult::IoError;
    }

    const QByteArray stagedPayload =
        settings.value(kStagedPayloadKey).toByteArray();
    if (settings.status() != QSettings::NoError) {
        settings.remove("topologyProfiles.pending");
        setError(error, QStringLiteral("could not validate staged topology profiles"));
        return TopologyProfileStoreResult::IoError;
    }
    TopologyProfiles validated;
    if (!deserializeProfiles(stagedPayload, validated, error)) {
        settings.remove("topologyProfiles.pending");
        return TopologyProfileStoreResult::Malformed;
    }

    settings.setValue(kLivePayloadKey, payload);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (hadLivePayload) {
            settings.setValue(kLivePayloadKey, previousPayload);
        }
        else {
            settings.remove(kLivePayloadKey);
        }
        settings.sync();
        settings.remove("topologyProfiles.pending");
        setError(error, QStringLiteral("could not commit topology profiles"));
        return TopologyProfileStoreResult::IoError;
    }

    settings.remove("topologyProfiles.pending");
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        setError(error, QStringLiteral("could not clean staged topology profiles"));
        return TopologyProfileStoreResult::IoError;
    }
    setError(error, QString());
    return TopologyProfileStoreResult::Ok;
}

} // namespace barrier
