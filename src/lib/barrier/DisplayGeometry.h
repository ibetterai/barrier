/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2002 Chris Schoeneman
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "barrier/protocol_types.h"
#include "common/basic_types.h"
#include "server/Config.h"

#include <string>
#include <vector>

namespace barrier {

//! Screen origin in the global layout.
/*!
The position of the screen's local coordinate space origin within
the freeform global display layout.  A screen's rectangles are
expressed in this local coordinate space and are placed in the
layout by adding this origin to them.
*/
struct ScreenOrigin {
    SInt32 x;
    SInt32 y;
};

//! A single physical display of a screen.
/*!
\p rect is a screen-local rectangle; \p origin places it in the
freeform global display layout.
*/
struct DisplayPlacement {
    std::string screen;
    ScreenRect rect;
    ScreenOrigin origin;
};

//! An ordered cross-screen contact along one pair of edges.
/*!
Intervals are normalized against the source and target screen unions
along the axis perpendicular to the contact.
*/
struct DisplayLink {
    std::string source;
    EDirection sourceSide;
    Config::Interval sourceInterval;
    std::string target;
    EDirection targetSide;
    Config::Interval targetInterval;
};

bool operator==(const DisplayLink&, const DisplayLink&);
bool operator<(const DisplayLink&, const DisplayLink&);

//! Compute the bounding box of a set of local rectangles.
/*!
\p rects are expressed in a screen's local coordinate space.  The
arithmetic is performed with wider intermediates so it cannot
overflow; the returned rectangle is clamped to the SInt32
representable range.
*/
ScreenRect unionBounds(const std::vector<ScreenRect>& rects);

//! Derive ordered source-to-target links for touching cross-screen edges.
/*!
\p sourceRects and \p targetRects are screen-local rectangles: each
is placed in the freeform global display layout by adding its
screen's \c ScreenOrigin (so callers must not pass already-global
rectangles).  Every ordered cross-screen rectangle pair is then
checked for left, right, top, and bottom edge contacts within
\p adjacencyTolerance.  The overlap perpendicular to the contact is
normalized against each screen's union extent.  Zero-length overlaps
produce no link, equal links are deduplicated, and rects whose
translated coordinates fall outside the SInt32 representable range
are skipped.
*/
std::vector<DisplayLink> deriveDisplayLinks(
    const std::string& sourceName, const std::vector<ScreenRect>& sourceRects,
    ScreenOrigin sourceOrigin, const std::string& targetName,
    const std::vector<ScreenRect>& targetRects, ScreenOrigin targetOrigin,
    SInt32 adjacencyTolerance);

//! Convert an edge-crossing coordinate into the screen-union frame.
/*!
\p screenUnion is the bounding box returned by unionBounds() for the source
screen's display rectangles, \p crossedDisplay is the physical display whose
external edge is being crossed, and \p x/\p y are the cursor coordinates in
the source screen's local coordinate space.  Server::mapToNeighbor() performs
multi-screen traversal in the source screen union frame; this helper preserves
small overshoot from the crossed physical display edge while anchoring left/top
crossings at the union's low edge and right/bottom crossings at the union's
high edge.
*/
SInt32 canonicalEdgeCoordinate(EDirection direction,
                               const ScreenRect& screenUnion,
                               const ScreenRect& crossedDisplay,
                               SInt32 x, SInt32 y, SInt32 zoneSize);

//! Label of the display containing (x, y) in an ordered snapshot.
/*!
\p displays and \p names are the screen's ordered display snapshot as
reported by getDisplays() and getDisplayNames(); (x, y) is expressed in
the screen's local coordinate space, the same frame as \p displays.
The label is the name at the index of the first display whose half-open
bounds [x, x + w) x [y, y + h) contain the point, so an edge shared by
two displays resolves to exactly one of them.

Returns an empty string when no display contains the point, when
\p names does not cover \p displays exactly (a different count, or a
peer that never exchanged names), or when the matching name is itself
empty -- an unknown display carries no label, so a caller appending the
result to a log line never produces a blank parenthesis.
*/
std::string displayLabelAt(const std::vector<ScreenRect>& displays,
                           const std::vector<std::string>& names,
                           SInt32 x, SInt32 y);

//! Parenthesized label suffix for a log line.
/*!
Returns \c " (label)" when \p label is non-empty and \c "" otherwise,
so an unknown display leaves the existing log text byte-identical.
*/
std::string displayLabelSuffix(const std::string& label);

} // namespace barrier
