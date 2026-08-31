/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "barrier/protocol_types.h"

#include <string>
#include <vector>

namespace barrier {

struct DisplayTopologyEntry {
    std::string stableId;
    ScreenRect logicalBounds;
    int rotationDegrees;
    bool primary;
};

struct DisplayTopology {
    static const int kIdentityVersion = 1;

    std::vector<DisplayTopologyEntry> displays;

    bool empty() const;
    void validate() const;
    DisplayTopology normalized() const;
    std::string canonicalIdentity() const;
    std::string profileKey() const;
};

} // namespace barrier
