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

#include "barrier/DisplayGeometry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace barrier {

namespace {

// Edge bounds kept in 64-bit space so union arithmetic can never
// overflow SInt32 intermediates.
struct Bounds {
    std::int64_t left;
    std::int64_t top;
    std::int64_t right;
    std::int64_t bottom;
};

Bounds boundsOf(const ScreenRect& rect)
{
    Bounds b;
    b.left = static_cast<std::int64_t>(rect.x);
    b.top = static_cast<std::int64_t>(rect.y);
    b.right = b.left + rect.w;
    b.bottom = b.top + rect.h;
    return b;
}

Bounds unionBounds64(const std::vector<ScreenRect>& rects)
{
    Bounds b = boundsOf(rects.front());
    for (std::size_t i = 1; i < rects.size(); ++i) {
        const Bounds r = boundsOf(rects[i]);
        b.left = std::min(b.left, r.left);
        b.top = std::min(b.top, r.top);
        b.right = std::max(b.right, r.right);
        b.bottom = std::max(b.bottom, r.bottom);
    }
    return b;
}

// Union of already-global bounds.  \p bounds must be non-empty.
Bounds unionOf(const std::vector<Bounds>& bounds)
{
    Bounds b = bounds.front();
    for (std::size_t i = 1; i < bounds.size(); ++i) {
        const Bounds& r = bounds[i];
        b.left = std::min(b.left, r.left);
        b.top = std::min(b.top, r.top);
        b.right = std::max(b.right, r.right);
        b.bottom = std::max(b.bottom, r.bottom);
    }
    return b;
}

// Translate a screen-local rect into the global layout using checked
// arithmetic.  Returns false (leaving \p out untouched) when the result
// cannot be represented as SInt32 coordinates; such a rect cannot take
// part in links.
bool translate(const ScreenRect& rect, const ScreenOrigin& origin, ScreenRect& out)
{
    const std::int64_t x = static_cast<std::int64_t>(rect.x) + origin.x;
    const std::int64_t y = static_cast<std::int64_t>(rect.y) + origin.y;
    if (x < std::numeric_limits<SInt32>::min() ||
        x > std::numeric_limits<SInt32>::max() ||
        y < std::numeric_limits<SInt32>::min() ||
        y > std::numeric_limits<SInt32>::max()) {
        return false;
    }
    out = rect;
    out.x = static_cast<SInt32>(x);
    out.y = static_cast<SInt32>(y);
    return true;
}

SInt32 clampSInt32(std::int64_t value)
{
    if (value < std::numeric_limits<SInt32>::min()) {
        return std::numeric_limits<SInt32>::min();
    }
    if (value > std::numeric_limits<SInt32>::max()) {
        return std::numeric_limits<SInt32>::max();
    }
    return static_cast<SInt32>(value);
}

// Normalize [start, end] on an axis against a union position and extent.
Config::Interval normalizeAxis(std::int64_t start, std::int64_t end,
                               std::int64_t unionStart, std::int64_t unionExtent)
{
    if (unionExtent <= 0) {
        return Config::Interval(0.0f, 0.0f);
    }
    return Config::Interval(
        static_cast<float>(start - unionStart) / static_cast<float>(unionExtent),
        static_cast<float>(end - unionStart) / static_cast<float>(unionExtent));
}

// Append a link for a touching edge pair if the perpendicular overlap
// [overlapStart, overlapEnd] is non-empty.  \p vertical is true for
// top/bottom contacts, whose overlap runs along the x axis.
void addLink(std::vector<DisplayLink>& links,
             const std::string& sourceName, EDirection sourceSide,
             const Bounds& sourceUnion,
             const std::string& targetName, EDirection targetSide,
             const Bounds& targetUnion,
             std::int64_t overlapStart, std::int64_t overlapEnd, bool vertical)
{
    if (overlapEnd <= overlapStart) {
        return;
    }

    DisplayLink link;
    link.source = sourceName;
    link.sourceSide = sourceSide;
    link.target = targetName;
    link.targetSide = targetSide;
    if (vertical) {
        link.sourceInterval =
            normalizeAxis(overlapStart, overlapEnd, sourceUnion.left,
                          sourceUnion.right - sourceUnion.left);
        link.targetInterval =
            normalizeAxis(overlapStart, overlapEnd, targetUnion.left,
                          targetUnion.right - targetUnion.left);
    } else {
        link.sourceInterval =
            normalizeAxis(overlapStart, overlapEnd, sourceUnion.top,
                          sourceUnion.bottom - sourceUnion.top);
        link.targetInterval =
            normalizeAxis(overlapStart, overlapEnd, targetUnion.top,
                          targetUnion.bottom - targetUnion.top);
    }
    links.push_back(link);
}

} // namespace

bool operator==(const DisplayLink& lhs, const DisplayLink& rhs)
{
    return lhs.source == rhs.source &&
           lhs.sourceSide == rhs.sourceSide &&
           lhs.sourceInterval == rhs.sourceInterval &&
           lhs.target == rhs.target &&
           lhs.targetSide == rhs.targetSide &&
           lhs.targetInterval == rhs.targetInterval;
}

bool operator<(const DisplayLink& lhs, const DisplayLink& rhs)
{
    if (lhs.source != rhs.source) {
        return lhs.source < rhs.source;
    }
    if (lhs.sourceSide != rhs.sourceSide) {
        return lhs.sourceSide < rhs.sourceSide;
    }
    if (lhs.sourceInterval != rhs.sourceInterval) {
        return lhs.sourceInterval < rhs.sourceInterval;
    }
    if (lhs.target != rhs.target) {
        return lhs.target < rhs.target;
    }
    if (lhs.targetSide != rhs.targetSide) {
        return lhs.targetSide < rhs.targetSide;
    }
    return lhs.targetInterval < rhs.targetInterval;
}

ScreenRect unionBounds(const std::vector<ScreenRect>& rects)
{
    if (rects.empty()) {
        return ScreenRect{0, 0, 0, 0};
    }

    const Bounds b = unionBounds64(rects);
    ScreenRect result;
    result.x = clampSInt32(b.left);
    result.y = clampSInt32(b.top);
    result.w = clampSInt32(b.right - b.left);
    result.h = clampSInt32(b.bottom - b.top);
    return result;
}

std::vector<DisplayLink> deriveDisplayLinks(
    const std::string& sourceName, const std::vector<ScreenRect>& sourceRects,
    ScreenOrigin sourceOrigin, const std::string& targetName,
    const std::vector<ScreenRect>& targetRects, ScreenOrigin targetOrigin,
    SInt32 adjacencyTolerance)
{
    std::vector<DisplayLink> links;

    // Translate each screen's rectangles once, keeping only those whose
    // global coordinates are representable as SInt32.  The union used for
    // interval normalization and the edge-contact checks both derive from
    // this same filtered set, so invalid geometry cannot distort the
    // intervals of valid links.
    std::vector<Bounds> sourceGlobal;
    for (std::vector<ScreenRect>::const_iterator i = sourceRects.begin();
         i != sourceRects.end(); ++i) {
        ScreenRect s;
        if (translate(*i, sourceOrigin, s)) {
            sourceGlobal.push_back(boundsOf(s));
        }
    }

    std::vector<Bounds> targetGlobal;
    for (std::vector<ScreenRect>::const_iterator i = targetRects.begin();
         i != targetRects.end(); ++i) {
        ScreenRect t;
        if (translate(*i, targetOrigin, t)) {
            targetGlobal.push_back(boundsOf(t));
        }
    }

    const Bounds sourceUnion =
        sourceGlobal.empty() ? Bounds{0, 0, 0, 0} : unionOf(sourceGlobal);
    const Bounds targetUnion =
        targetGlobal.empty() ? Bounds{0, 0, 0, 0} : unionOf(targetGlobal);

    for (std::vector<Bounds>::const_iterator si = sourceGlobal.begin();
         si != sourceGlobal.end(); ++si) {
        const Bounds& s = *si;

        for (std::vector<Bounds>::const_iterator ti = targetGlobal.begin();
             ti != targetGlobal.end(); ++ti) {
            const Bounds& t = *ti;

            // source right edge touching target left edge
            if (std::abs(s.right - t.left) <= adjacencyTolerance) {
                addLink(links, sourceName, kRight, sourceUnion,
                        targetName, kLeft, targetUnion,
                        std::max(s.top, t.top), std::min(s.bottom, t.bottom), false);
            }
            // source left edge touching target right edge
            if (std::abs(s.left - t.right) <= adjacencyTolerance) {
                addLink(links, sourceName, kLeft, sourceUnion,
                        targetName, kRight, targetUnion,
                        std::max(s.top, t.top), std::min(s.bottom, t.bottom), false);
            }
            // source bottom edge touching target top edge
            if (std::abs(s.bottom - t.top) <= adjacencyTolerance) {
                addLink(links, sourceName, kBottom, sourceUnion,
                        targetName, kTop, targetUnion,
                        std::max(s.left, t.left), std::min(s.right, t.right), true);
            }
            // source top edge touching target bottom edge
            if (std::abs(s.top - t.bottom) <= adjacencyTolerance) {
                addLink(links, sourceName, kTop, sourceUnion,
                        targetName, kBottom, targetUnion,
                        std::max(s.left, t.left), std::min(s.right, t.right), true);
            }
        }
    }

    std::sort(links.begin(), links.end());
    links.erase(std::unique(links.begin(), links.end()), links.end());
    return links;
}

std::string
displayLabelAt(const std::vector<ScreenRect>& displays,
               const std::vector<std::string>& names,
               SInt32 x, SInt32 y)
{
    // a name set that does not cover the snapshot (or a peer that never
    // exchanged names) can never resolve a label
    if (names.size() != displays.size()) {
        return std::string();
    }
    for (std::size_t i = 0; i < displays.size(); ++i) {
        const ScreenRect& rect = displays[i];
        if (x >= rect.x && x < rect.x + rect.w &&
            y >= rect.y && y < rect.y + rect.h) {
            return names[i];
        }
    }
    return std::string();
}

std::string
displayLabelSuffix(const std::string& label)
{
    if (label.empty()) {
        return std::string();
    }
    return " (" + label + ")";
}

} // namespace barrier
