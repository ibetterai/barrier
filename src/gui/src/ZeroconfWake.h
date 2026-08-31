/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include <QtCore/QString>
#include <QtCore/QtGlobal>

#include <functional>

class QObject;
class ZeroconfRecord;

namespace barrier {

using ZeroconfWakeCompletion =
    std::function<void(bool success, const QString& message)>;

//! Exponential delay between wake attempts for one client.
qint64 zeroconfWakeBackoffMs(int previousAttempts);

//! Schedule a targeted Bonjour wake and auxiliary TCP readiness probe.
/*!
The returned child object can be deleted to cancel the operation. Startup is
queued so the caller can retain the handle before completion is possible.
*/
QObject* scheduleZeroconfWake(const ZeroconfRecord& record,
                              QObject* parent,
                              ZeroconfWakeCompletion completion);

} // namespace barrier
