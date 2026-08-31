/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "server/ClientWakeRequest.h"

#include <cmath>

namespace barrier {

namespace {

const char kWakeMarker[] = "BARRIER_WAKE";
const char kWakePrefix[] = "BARRIER_WAKE\tv=1\ttarget=";

bool isValidTarget(const std::string& target)
{
    return !target.empty() && target.size() <= kClientWakeMaxTargetBytes;
}

int lowerHexValue(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

} // namespace

bool formatClientWakeRequest(const std::string& target, std::string& payload)
{
    if (!isValidTarget(target)) {
        return false;
    }

    static const char kHexDigits[] = "0123456789abcdef";
    std::string formatted(kWakePrefix);
    formatted.reserve(formatted.size() + target.size() * 2);
    for (const unsigned char value : target) {
        formatted.push_back(kHexDigits[value >> 4]);
        formatted.push_back(kHexDigits[value & 0x0f]);
    }

    payload.swap(formatted);
    return true;
}

ClientWakeRequestParseResult parseClientWakeRequest(const std::string& line,
                                                     std::string& target)
{
    // Bound work and allocations before searching attacker-controlled logs.
    if (line.size() > kClientWakeMaxLineBytes) {
        return ClientWakeRequestParseResult::Invalid;
    }

    const std::size_t markerPosition = line.find(kWakeMarker);
    if (markerPosition == std::string::npos) {
        return ClientWakeRequestParseResult::NotWake;
    }
    if (markerPosition != 0 ||
        line.compare(0, sizeof(kWakePrefix) - 1, kWakePrefix) != 0 ||
        line.find(kWakeMarker, markerPosition + 1) != std::string::npos) {
        return ClientWakeRequestParseResult::Invalid;
    }

    const std::size_t encodedStart = sizeof(kWakePrefix) - 1;
    std::size_t encodedEnd = line.size();
    if (encodedEnd > encodedStart && line[encodedEnd - 1] == '\n') {
        --encodedEnd;
        if (encodedEnd > encodedStart && line[encodedEnd - 1] == '\r') {
            --encodedEnd;
        }
    }

    const std::size_t encodedSize = encodedEnd - encodedStart;
    if (encodedSize == 0 || encodedSize % 2 != 0 ||
        encodedSize / 2 > kClientWakeMaxTargetBytes) {
        return ClientWakeRequestParseResult::Invalid;
    }

    std::string decoded;
    decoded.reserve(encodedSize / 2);
    for (std::size_t offset = encodedStart; offset < encodedEnd; offset += 2) {
        const int high = lowerHexValue(line[offset]);
        const int low = lowerHexValue(line[offset + 1]);
        if (high < 0 || low < 0) {
            return ClientWakeRequestParseResult::Invalid;
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
    }

    target.swap(decoded);
    return ClientWakeRequestParseResult::Valid;
}

bool ClientWakeRequestTracker::shouldEmit(const std::string& target,
                                          double monotonicSeconds)
{
    if (!isValidTarget(target) || !std::isfinite(monotonicSeconds) ||
        monotonicSeconds < 0.0) {
        return false;
    }

    const auto existing = m_lastRequestSeconds.find(target);
    if (existing == m_lastRequestSeconds.end()) {
        m_lastRequestSeconds.emplace(target, monotonicSeconds);
        return true;
    }

    const double previous = existing->second;
    if (monotonicSeconds < previous ||
        monotonicSeconds - previous < kClientWakeMinimumIntervalSeconds) {
        return false;
    }

    existing->second = monotonicSeconds;
    return true;
}

void ClientWakeRequestTracker::confirmConnected(const std::string& target)
{
    m_lastRequestSeconds.erase(target);
}

} // namespace barrier
