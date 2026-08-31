/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "server/DisplayTopologyStateMachine.h"

#include <limits>
#include <stdexcept>

namespace {

const std::int64_t kTopologyQuietPeriodMs = 2000;
const std::int64_t kNoDisplayGracePeriodMs = 10000;

} // namespace

DisplayTopologyDecision DisplayTopologyStateMachine::beginReconfiguration(
    std::int64_t)
{
    m_reconfigurationSnapshotPending = true;
    if (m_state != DisplayTopologyState::NoDisplayGrace &&
        m_state != DisplayTopologyState::Unavailable) {
        m_state = DisplayTopologyState::Reconfiguring;
        // Settling cannot begin until a fresh snapshot has been captured.
        m_deadlineMs = -1;
    }
    return decision();
}

DisplayTopologyDecision DisplayTopologyStateMachine::observe(
    const barrier::DisplayTopology& topology,
    bool profileKnown,
    std::int64_t monotonicMs)
{
    return observeSnapshot(topology, profileKnown, monotonicMs, false);
}

DisplayTopologyDecision
DisplayTopologyStateMachine::observeReconfigurationSnapshot(
    const barrier::DisplayTopology& topology,
    bool profileKnown,
    std::int64_t monotonicMs)
{
    return observeSnapshot(topology, profileKnown, monotonicMs, true);
}

DisplayTopologyDecision DisplayTopologyStateMachine::observeSnapshot(
    const barrier::DisplayTopology& topology,
    bool profileKnown,
    std::int64_t monotonicMs,
    bool fromReconfiguration)
{
    // Configuration reloads and secondary DDIS updates can replay the old
    // primary snapshot. They must not complete a pending CoreGraphics capture,
    // including when that old snapshot is empty or invalid.
    if (!fromReconfiguration && m_reconfigurationSnapshotPending) {
        return decision();
    }

    if (topology.empty()) {
        if (fromReconfiguration) {
            m_reconfigurationSnapshotPending = false;
        }
        return observeUnavailable(monotonicMs);
    }

    std::string topologyKey;
    try {
        topologyKey = topology.normalized().profileKey();
    }
    catch (const std::invalid_argument&) {
        if (fromReconfiguration) {
            m_reconfigurationSnapshotPending = false;
        }
        return observeUnavailable(monotonicMs);
    }

    const bool sameTopology = m_hasTopology &&
        !m_lastObservationWasEmpty && topologyKey == m_topologyKey;
    m_profileKnown = profileKnown;

    m_lastObservationWasEmpty = false;
    m_hasTopology = true;

    if (fromReconfiguration) {
        m_reconfigurationSnapshotPending = false;
        m_topologyKey.swap(topologyKey);
        m_state = DisplayTopologyState::Reconfiguring;
        // Every CoreGraphics callback replaces the quiet-period deadline,
        // including callbacks whose normalized topology is unchanged.
        m_deadlineMs = deadlineAfter(monotonicMs, kTopologyQuietPeriodMs);
        return decision();
    }

    if (sameTopology) {
        if (m_state == DisplayTopologyState::Reconfiguring) {
            return decision();
        }
        m_state = profileKnown ? DisplayTopologyState::StableKnown
                               : DisplayTopologyState::StableUnknown;
        m_deadlineMs = -1;
        return decision();
    }

    m_topologyKey.swap(topologyKey);
    m_state = DisplayTopologyState::Reconfiguring;
    m_deadlineMs = deadlineAfter(monotonicMs, kTopologyQuietPeriodMs);
    return decision();
}

DisplayTopologyDecision DisplayTopologyStateMachine::onDeadline(
    std::int64_t monotonicMs)
{
    if (m_state == DisplayTopologyState::Reconfiguring) {
        if (m_reconfigurationSnapshotPending) {
            return decision();
        }
        if (monotonicMs < m_deadlineMs) {
            return decision();
        }
        m_state = m_profileKnown ? DisplayTopologyState::StableKnown
                                 : DisplayTopologyState::StableUnknown;
        m_deadlineMs = -1;
        return decision();
    }

    if (m_state == DisplayTopologyState::NoDisplayGrace) {
        // A CoreGraphics begin callback makes the previous empty snapshot
        // stale.  Do not disconnect on its old grace deadline while capture
        // is pending (query failures are retried by the platform screen).
        if (m_reconfigurationSnapshotPending) {
            return decision();
        }
        if (monotonicMs < m_deadlineMs) {
            return decision();
        }
        m_state = DisplayTopologyState::Unavailable;
        m_deadlineMs = -1;
        return decision(true);
    }

    return decision();
}

DisplayTopologyDecision DisplayTopologyStateMachine::observeUnavailable(
    std::int64_t monotonicMs)
{
    m_reconfigurationSnapshotPending = false;
    if (m_lastObservationWasEmpty) {
        if (m_state == DisplayTopologyState::NoDisplayGrace &&
            m_deadlineMs >= 0 && monotonicMs >= m_deadlineMs) {
            // Capture was pending past the original grace deadline. A fresh
            // empty snapshot now confirms unavailability, so disconnect
            // synchronously rather than returning an already-expired timer.
            m_state = DisplayTopologyState::Unavailable;
            m_deadlineMs = -1;
            return decision(true);
        }
        return decision();
    }

    m_lastObservationWasEmpty = true;
    m_state = DisplayTopologyState::NoDisplayGrace;
    m_deadlineMs = deadlineAfter(monotonicMs, kNoDisplayGracePeriodMs);
    return decision();
}

DisplayTopologyDecision DisplayTopologyStateMachine::decision(
    bool disconnectClients) const
{
    switch (m_state) {
    case DisplayTopologyState::StableKnown:
        return {m_state, true, disconnectClients, true, -1};
    case DisplayTopologyState::StableUnknown:
        return {m_state, false, disconnectClients, true, -1};
    case DisplayTopologyState::Reconfiguring:
        return {m_state, false, disconnectClients, false, m_deadlineMs};
    case DisplayTopologyState::NoDisplayGrace:
        return {m_state, false, disconnectClients, false,
                m_reconfigurationSnapshotPending ? -1 : m_deadlineMs};
    case DisplayTopologyState::Unavailable:
        return {m_state, false, disconnectClients, false, -1};
    }

    return {DisplayTopologyState::Unavailable, false, false, false, -1};
}

std::int64_t DisplayTopologyStateMachine::deadlineAfter(
    std::int64_t monotonicMs,
    std::int64_t delayMs)
{
    const std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
    return monotonicMs > maximum - delayMs ? maximum
                                            : monotonicMs + delayMs;
}
