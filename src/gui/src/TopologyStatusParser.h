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

#include <QString>

namespace barrier {

enum class TopologyStatusState {
    Reconfiguring,
    StableKnown,
    StableUnknown,
    NoDisplayGrace,
    Unavailable
};

struct TopologyStatus {
    TopologyStatusState state{TopologyStatusState::Unavailable};
    QString key;
    DisplayTopology topology;

    bool displayReady() const
    {
        return state == TopologyStatusState::StableKnown ||
               state == TopologyStatusState::StableUnknown;
    }
};

enum class TopologyStatusParseResult {
    NotTopology,
    Valid,
    Invalid
};

class TopologyStatusParser {
public:
    static TopologyStatusParseResult parse(
        const QString& line,
        TopologyStatus& output,
        QString* error = nullptr);
};

} // namespace barrier
