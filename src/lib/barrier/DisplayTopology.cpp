/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/DisplayTopology.h"

#include "base/Sha256.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace barrier {
namespace {

bool isQuarterTurn(int rotationDegrees)
{
    return rotationDegrees == 0 || rotationDegrees == 90 ||
           rotationDegrees == 180 || rotationDegrees == 270;
}

SInt32 checkedDifference(SInt32 value, SInt32 origin)
{
    const std::int64_t result = static_cast<std::int64_t>(value) - origin;
    if (result < std::numeric_limits<SInt32>::min() ||
        result > std::numeric_limits<SInt32>::max()) {
        throw std::invalid_argument("display topology coordinate is out of range");
    }
    return static_cast<SInt32>(result);
}

} // namespace

bool DisplayTopology::empty() const
{
    return displays.empty();
}

void DisplayTopology::validate() const
{
    if (empty()) {
        return;
    }

    std::set<std::string> stableIds;
    std::size_t primaryCount = 0;
    for (const DisplayTopologyEntry& display : displays) {
        if (display.stableId.empty()) {
            throw std::invalid_argument("display topology contains an empty stable ID");
        }
        if (!stableIds.insert(display.stableId).second) {
            throw std::invalid_argument("display topology contains a duplicate stable ID");
        }
        if (display.logicalBounds.w <= 0 || display.logicalBounds.h <= 0) {
            throw std::invalid_argument("display topology contains a non-positive size");
        }
        if (!isQuarterTurn(display.rotationDegrees)) {
            throw std::invalid_argument("display topology contains an invalid rotation");
        }
        if (display.primary) {
            ++primaryCount;
        }
    }

    if (primaryCount != 1) {
        throw std::invalid_argument("display topology must contain exactly one primary display");
    }
}

DisplayTopology DisplayTopology::normalized() const
{
    validate();
    if (empty()) {
        return *this;
    }

    const DisplayTopologyEntry* primaryDisplay = nullptr;
    for (const DisplayTopologyEntry& display : displays) {
        if (display.primary) {
            primaryDisplay = &display;
            break;
        }
    }

    DisplayTopology result = *this;
    for (DisplayTopologyEntry& display : result.displays) {
        display.logicalBounds.x = checkedDifference(
            display.logicalBounds.x, primaryDisplay->logicalBounds.x);
        display.logicalBounds.y = checkedDifference(
            display.logicalBounds.y, primaryDisplay->logicalBounds.y);
    }
    std::sort(result.displays.begin(), result.displays.end(),
              [](const DisplayTopologyEntry& left,
                 const DisplayTopologyEntry& right) {
                  return left.stableId < right.stableId;
              });
    return result;
}

std::string DisplayTopology::canonicalIdentity() const
{
    if (empty()) {
        return std::string();
    }

    const DisplayTopology topology = normalized();
    std::ostringstream output;
    output << "display-topology-v" << kIdentityVersion << '|'
           << topology.displays.size();
    static const char kHex[] = "0123456789abcdef";
    for (const DisplayTopologyEntry& display : topology.displays) {
        output << '|' << display.stableId.size() << ':';
        for (unsigned char byte : display.stableId) {
            output << kHex[byte >> 4] << kHex[byte & 0x0f];
        }
        output << ',' << display.logicalBounds.x
               << ',' << display.logicalBounds.y
               << ',' << display.logicalBounds.w
               << ',' << display.logicalBounds.h
               << ',' << display.rotationDegrees
               << ',' << (display.primary ? 1 : 0);
    }
    return output.str();
}

std::string DisplayTopology::profileKey() const
{
    const std::string identity = canonicalIdentity();
    return identity.empty() ? std::string() : sha256Hex(identity);
}

} // namespace barrier
