/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "TopologyStatusParser.h"

#include <QByteArray>
#include <QMap>
#include <QRegularExpression>
#include <QStringList>

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace barrier {
namespace {

const QString kStatusPrefix = QStringLiteral("BARRIER_TOPOLOGY\t");
const int kMaximumStatusCharacters = 16384;

bool parseUnsigned(const QString& value, qulonglong& output)
{
    static const QRegularExpression pattern(QStringLiteral("^(0|[1-9][0-9]*)$"));
    if (!pattern.match(value).hasMatch()) {
        return false;
    }
    bool ok = false;
    output = value.toULongLong(&ok, 10);
    return ok;
}

bool parseCoordinate(const QString& value, SInt32& output)
{
    static const QRegularExpression pattern(
        QStringLiteral("^(0|-?[1-9][0-9]*)$"));
    if (!pattern.match(value).hasMatch()) {
        return false;
    }
    bool ok = false;
    const qlonglong parsed = value.toLongLong(&ok, 10);
    if (!ok || parsed < std::numeric_limits<SInt32>::min() ||
        parsed > std::numeric_limits<SInt32>::max()) {
        return false;
    }
    output = static_cast<SInt32>(parsed);
    return true;
}

bool decodeTopology(const QString& identity,
                    DisplayTopology& topology,
                    QString& error)
{
    const QStringList parts = identity.split(QLatin1Char('|'), Qt::KeepEmptyParts);
    if (parts.size() < 3 || parts[0] != QStringLiteral("display-topology-v1")) {
        error = QStringLiteral("unsupported or malformed topology identity");
        return false;
    }

    qulonglong displayCount = 0;
    if (!parseUnsigned(parts[1], displayCount) || displayCount == 0 ||
        displayCount > 256 || parts.size() != static_cast<int>(displayCount) + 2) {
        error = QStringLiteral("invalid topology display count");
        return false;
    }

    DisplayTopology decoded;
    decoded.displays.reserve(static_cast<std::size_t>(displayCount));
    static const QRegularExpression lowerHex(QStringLiteral("^[0-9a-f]+$"));
    for (int index = 2; index < parts.size(); ++index) {
        const QStringList fields = parts[index].split(
            QLatin1Char(','), Qt::KeepEmptyParts);
        if (fields.size() != 7) {
            error = QStringLiteral("invalid topology display field count");
            return false;
        }

        const int separator = fields[0].indexOf(QLatin1Char(':'));
        if (separator <= 0 || fields[0].indexOf(QLatin1Char(':'), separator + 1) >= 0) {
            error = QStringLiteral("invalid topology display identity");
            return false;
        }
        qulonglong stableIdLength = 0;
        const QString encodedId = fields[0].mid(separator + 1);
        if (!parseUnsigned(fields[0].left(separator), stableIdLength) ||
            stableIdLength == 0 || stableIdLength > 4096 ||
            encodedId.size() != static_cast<int>(stableIdLength * 2) ||
            !lowerHex.match(encodedId).hasMatch()) {
            error = QStringLiteral("invalid topology stable display ID");
            return false;
        }
        const QByteArray stableIdBytes = QByteArray::fromHex(encodedId.toLatin1());
        if (stableIdBytes.size() != static_cast<int>(stableIdLength)) {
            error = QStringLiteral("invalid topology stable display ID encoding");
            return false;
        }

        DisplayTopologyEntry display;
        display.stableId.assign(stableIdBytes.constData(),
                                static_cast<std::size_t>(stableIdBytes.size()));
        if (!parseCoordinate(fields[1], display.logicalBounds.x) ||
            !parseCoordinate(fields[2], display.logicalBounds.y) ||
            !parseCoordinate(fields[3], display.logicalBounds.w) ||
            !parseCoordinate(fields[4], display.logicalBounds.h)) {
            error = QStringLiteral("invalid topology display geometry");
            return false;
        }

        qulonglong rotation = 0;
        qulonglong primary = 0;
        if (!parseUnsigned(fields[5], rotation) ||
            !parseUnsigned(fields[6], primary) || primary > 1 ||
            rotation > static_cast<qulonglong>(std::numeric_limits<int>::max())) {
            error = QStringLiteral("invalid topology display attributes");
            return false;
        }
        display.rotationDegrees = static_cast<int>(rotation);
        display.primary = primary == 1;
        decoded.displays.push_back(display);
    }

    try {
        decoded.validate();
        if (QString::fromStdString(decoded.canonicalIdentity()) != identity) {
            error = QStringLiteral("topology identity is not canonical");
            return false;
        }
    }
    catch (const std::invalid_argument&) {
        error = QStringLiteral("invalid topology display collection");
        return false;
    }

    topology = decoded;
    return true;
}

bool parseState(const QString& value, TopologyStatusState& state)
{
    if (value == QStringLiteral("Reconfiguring")) {
        state = TopologyStatusState::Reconfiguring;
    }
    else if (value == QStringLiteral("StableKnown")) {
        state = TopologyStatusState::StableKnown;
    }
    else if (value == QStringLiteral("StableUnknown")) {
        state = TopologyStatusState::StableUnknown;
    }
    else if (value == QStringLiteral("NoDisplayGrace")) {
        state = TopologyStatusState::NoDisplayGrace;
    }
    else if (value == QStringLiteral("Unavailable")) {
        state = TopologyStatusState::Unavailable;
    }
    else {
        return false;
    }
    return true;
}

} // namespace

TopologyStatusParseResult TopologyStatusParser::parse(
    const QString& line,
    TopologyStatus& output,
    QString* error)
{
    const int prefixIndex = line.indexOf(kStatusPrefix);
    if (prefixIndex < 0) {
        return TopologyStatusParseResult::NotTopology;
    }

    auto invalid = [error](const QString& message) {
        if (error != nullptr) {
            *error = message;
        }
        return TopologyStatusParseResult::Invalid;
    };
    if (line.size() > kMaximumStatusCharacters) {
        return invalid(QStringLiteral("topology status line is too large"));
    }

    const QString payload = line.mid(prefixIndex + kStatusPrefix.size());
    const QStringList encodedFields = payload.split(
        QLatin1Char('\t'), Qt::KeepEmptyParts);
    QMap<QString, QString> fields;
    for (const QString& encodedField : encodedFields) {
        const int separator = encodedField.indexOf(QLatin1Char('='));
        if (separator <= 0 ||
            encodedField.indexOf(QLatin1Char('='), separator + 1) >= 0) {
            return invalid(QStringLiteral("malformed topology status field"));
        }
        const QString name = encodedField.left(separator);
        if ((name != QStringLiteral("state") &&
             name != QStringLiteral("key") &&
             name != QStringLiteral("displays")) ||
            fields.contains(name)) {
            return invalid(QStringLiteral("unknown or duplicate topology status field"));
        }
        fields.insert(name, encodedField.mid(separator + 1));
    }
    if (fields.size() != 3 || !fields.contains(QStringLiteral("state")) ||
        !fields.contains(QStringLiteral("key")) ||
        !fields.contains(QStringLiteral("displays"))) {
        return invalid(QStringLiteral("missing topology status field"));
    }

    TopologyStatus parsed;
    if (!parseState(fields.value(QStringLiteral("state")), parsed.state)) {
        return invalid(QStringLiteral("invalid topology state"));
    }
    parsed.key = fields.value(QStringLiteral("key"));
    const QString identity = fields.value(QStringLiteral("displays"));
    const bool requiresTopology =
        parsed.state == TopologyStatusState::Reconfiguring ||
        parsed.state == TopologyStatusState::StableKnown ||
        parsed.state == TopologyStatusState::StableUnknown;
    if (requiresTopology) {
        static const QRegularExpression keyPattern(QStringLiteral("^[0-9a-f]{64}$"));
        QString topologyError;
        if (!keyPattern.match(parsed.key).hasMatch() ||
            !decodeTopology(identity, parsed.topology, topologyError)) {
            return invalid(topologyError.isEmpty()
                ? QStringLiteral("invalid topology key") : topologyError);
        }
        if (QString::fromStdString(parsed.topology.profileKey()) != parsed.key) {
            return invalid(QStringLiteral("topology key does not match identity"));
        }
    }
    else if (parsed.key != QStringLiteral("-") || !identity.isEmpty()) {
        return invalid(QStringLiteral("unavailable topology must be empty"));
    }

    output = parsed;
    if (error != nullptr) {
        error->clear();
    }
    return TopologyStatusParseResult::Valid;
}

} // namespace barrier
