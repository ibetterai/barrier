/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include <cstddef>
#include <map>
#include <string>

namespace barrier {

constexpr std::size_t kClientWakeMaxTargetBytes = 256;
constexpr std::size_t kClientWakeMaxLineBytes = 4096;
constexpr double kClientWakeMinimumIntervalSeconds = 15.0;

enum class ClientWakeRequestParseResult {
    NotWake,
    Valid,
    Invalid
};

//! Format a daemon-to-GUI wake request as a canonical log payload.
/*!
The target is encoded byte-for-byte as lower-case hexadecimal so names stored
as UTF-8 cannot inject log fields.  On failure, \p payload is not modified.
*/
bool formatClientWakeRequest(const std::string& target, std::string& payload);

//! Extract a canonical wake request from a bounded raw control log line.
/*!
The marker must start at column zero; this prevents an ordinary or peer-derived
log message from becoming a control request. Lines without the wake marker
return NotWake. Malformed or overlong wake lines return Invalid. The output
target is modified only when Valid is returned.
*/
ClientWakeRequestParseResult parseClientWakeRequest(const std::string& line,
                                                     std::string& target);

//! Per-target rate limiter for wake requests emitted by the server daemon.
class ClientWakeRequestTracker {
public:
    //! Record and allow a request unless the same target was allowed <15s ago.
    bool shouldEmit(const std::string& target, double monotonicSeconds);

    //! Clear a target's cooldown once that client has connected successfully.
    void confirmConnected(const std::string& target);

private:
    std::map<std::string, double> m_lastRequestSeconds;
};

} // namespace barrier
