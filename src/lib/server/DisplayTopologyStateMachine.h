/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "barrier/DisplayTopology.h"

#include <cstdint>
#include <string>

enum class DisplayTopologyState {
    Reconfiguring,
    StableKnown,
    StableUnknown,
    NoDisplayGrace,
    Unavailable
};

struct DisplayTopologyDecision {
    DisplayTopologyState state;
    bool switchingEnabled;
    bool disconnectClients;
    bool displayReady;
    std::int64_t nextDeadlineMs;
};

class DisplayTopologyStateMachine {
public:
    DisplayTopologyDecision beginReconfiguration(std::int64_t monotonicMs);
    DisplayTopologyDecision observe(const barrier::DisplayTopology& topology,
                                    bool profileKnown,
                                    std::int64_t monotonicMs);
    DisplayTopologyDecision observeReconfigurationSnapshot(
                                    const barrier::DisplayTopology& topology,
                                    bool profileKnown,
                                    std::int64_t monotonicMs);
    DisplayTopologyDecision onDeadline(std::int64_t monotonicMs);

private:
    DisplayTopologyDecision observeSnapshot(
                                    const barrier::DisplayTopology& topology,
                                    bool profileKnown,
                                    std::int64_t monotonicMs,
                                    bool fromReconfiguration);
    DisplayTopologyDecision observeUnavailable(std::int64_t monotonicMs);
    DisplayTopologyDecision decision(bool disconnectClients = false) const;
    static std::int64_t deadlineAfter(std::int64_t monotonicMs,
                                      std::int64_t delayMs);

    DisplayTopologyState m_state{DisplayTopologyState::Unavailable};
    std::string m_topologyKey;
    bool m_hasTopology{false};
    bool m_lastObservationWasEmpty{false};
    bool m_profileKnown{false};
    bool m_reconfigurationSnapshotPending{false};
    std::int64_t m_deadlineMs{-1};
};
