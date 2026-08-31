/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2014-2016 Symless Ltd.
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

#include "server/Server.h"

#include "test/global/gtest.h"

#include <cmath>

using namespace barrier;

namespace {

const float kIntervalEpsilon = 1e-5f;

// L-shape layout: the laptop display LU sits above an LG display and to
// the left of a vertical display, both part of the MBP screen.  The MBP
// union spans X [0, 3000] and Y [0, 1920] in the global layout.

// The two MBP displays in MBP-local coordinates: LG below LU on the
// left, VG (the portrait) to LU's right spanning the union height.
std::vector<ScreenRect> lShapeMbpDisplays()
{
    const std::vector<ScreenRect> mbpRects = {{0, 840, 1920, 1080},
                                              {1920, 0, 1080, 1920}};
    return mbpRects;
}

std::vector<DisplayLink> lShapeLinks()
{
    const std::vector<ScreenRect> luRects = {{0, 0, 1920, 1080}};
    return deriveDisplayLinks("LU", luRects, ScreenOrigin{0, 0},
                              "MBP", lShapeMbpDisplays(), ScreenOrigin{0, 240}, 15);
}

// The union-relative intervals of the MBP screen's configured cross-host
// links on \p side, as derived from the L-shape (the target intervals of
// the LU<->MBP links).  This mirrors what the GUI serializes into the
// config for the MBP screen.
std::vector<Config::Interval> mbpLinkIntervals(EDirection side)
{
    const std::vector<DisplayLink> links = lShapeLinks();
    std::vector<Config::Interval> intervals;
    for (std::vector<DisplayLink>::const_iterator it = links.begin();
         it != links.end(); ++it) {
        if (it->target == "MBP" && it->targetSide == side) {
            intervals.push_back(it->targetInterval);
        }
    }
    return intervals;
}

// Locate the unique link between the given edges, or NULL.
const DisplayLink* findLink(const std::vector<DisplayLink>& links,
                            const std::string& source, EDirection sourceSide,
                            const std::string& target, EDirection targetSide)
{
    for (std::vector<DisplayLink>::const_iterator it = links.begin();
         it != links.end(); ++it) {
        if (it->source == source && it->sourceSide == sourceSide &&
            it->target == target && it->targetSide == targetSide) {
            return &*it;
        }
    }
    return NULL;
}

void expectInterval(const Config::Interval& actual, float first, float second)
{
    EXPECT_NEAR(actual.first, first, kIntervalEpsilon);
    EXPECT_NEAR(actual.second, second, kIntervalEpsilon);
}

} // namespace

TEST(DisplayGeometryTests, deriveDisplayLinks_fullTopLink_normalizedToUnions)
{
    const std::vector<DisplayLink> links = lShapeLinks();
    ASSERT_EQ(2u, links.size());

    // LU's bottom edge meets the LG display's top edge across LU's full
    // width: source X [0, 1], target X [0, 1920/3000].
    const DisplayLink* link = findLink(links, "LU", kBottom, "MBP", kTop);
    ASSERT_TRUE(link != NULL);
    expectInterval(link->sourceInterval, 0.0f, 1.0f);
    expectInterval(link->targetInterval, 0.0f, 1920.0f / 3000.0f);

    // The GUI serialization emits each endpoint as qRound(f * 100); the
    // union-normalized target fraction must produce the config text
    // (0,64), not the per-rectangle (0,100).
    EXPECT_EQ(0, static_cast<int>(link->targetInterval.first * 100.0f + 0.5f));
    EXPECT_EQ(64, static_cast<int>(link->targetInterval.second * 100.0f + 0.5f));
}

TEST(DisplayGeometryTests, deriveDisplayLinks_partialLeftLink_normalizedToUnions)
{
    const std::vector<DisplayLink> links = lShapeLinks();
    ASSERT_EQ(2u, links.size());

    // LU's right edge meets the vertical display's left edge only over
    // the lower part of the laptop: source Y [240/1080, 1], target Y
    // [0, 840/1920].
    const DisplayLink* link = findLink(links, "LU", kRight, "MBP", kLeft);
    ASSERT_TRUE(link != NULL);
    expectInterval(link->sourceInterval, 240.0f / 1080.0f, 1.0f);
    expectInterval(link->targetInterval, 0.0f, 840.0f / 1920.0f);
}

TEST(DisplayGeometryTests, deriveDisplayLinks_disjoint_noLink)
{
    const std::vector<DisplayLink> links = deriveDisplayLinks(
        "A", {{0, 0, 100, 100}}, ScreenOrigin{0, 0},
        "B", {{500, 0, 100, 100}}, ScreenOrigin{0, 0}, 15);
    EXPECT_TRUE(links.empty());
}

TEST(DisplayGeometryTests, deriveDisplayLinks_pointContact_noLink)
{
    // B touches A only at the corner point (100, 100); both edge checks
    // find a zero-length overlap, which must not produce a link.
    const std::vector<DisplayLink> links = deriveDisplayLinks(
        "A", {{0, 0, 100, 100}}, ScreenOrigin{0, 0},
        "B", {{100, 100, 100, 100}}, ScreenOrigin{0, 0}, 15);
    EXPECT_TRUE(links.empty());
}

TEST(DisplayGeometryTests, deriveDisplayLinks_tolerance_accepts3pxRejects20px)
{
    // A 3px gap is within the 15px adjacency tolerance.
    std::vector<DisplayLink> links = deriveDisplayLinks(
        "A", {{0, 0, 100, 100}}, ScreenOrigin{0, 0},
        "B", {{103, 0, 100, 100}}, ScreenOrigin{0, 0}, 15);
    ASSERT_EQ(1u, links.size());
    const DisplayLink* link = findLink(links, "A", kRight, "B", kLeft);
    ASSERT_TRUE(link != NULL);
    expectInterval(link->sourceInterval, 0.0f, 1.0f);
    expectInterval(link->targetInterval, 0.0f, 1.0f);

    // A 20px gap is beyond the 15px tolerance.
    links = deriveDisplayLinks(
        "A", {{0, 0, 100, 100}}, ScreenOrigin{0, 0},
        "B", {{120, 0, 100, 100}}, ScreenOrigin{0, 0}, 15);
    EXPECT_TRUE(links.empty());
}

TEST(DisplayGeometryTests, deriveDisplayLinks_outOfRangeRect_notInUnion)
{
    // The second source rect overflows SInt32 when translated by the
    // screen origin, so it must be excluded from the union that
    // normalizes the valid link; otherwise the source interval would
    // shrink to [0, 0.5] and the out-of-range rect would add a link.
    const ScreenOrigin kOverflowOrigin = {2147483647, 0};
    const std::vector<DisplayLink> links = deriveDisplayLinks(
        "A", {{0, 0, 100, 100}, {1, 100, 100, 100}}, kOverflowOrigin,
        "B", {{2147483547, 0, 100, 100}}, ScreenOrigin{0, 0}, 15);
    ASSERT_EQ(1u, links.size());
    const DisplayLink* link = findLink(links, "A", kLeft, "B", kRight);
    ASSERT_TRUE(link != NULL);
    expectInterval(link->sourceInterval, 0.0f, 1.0f);
    expectInterval(link->targetInterval, 0.0f, 1.0f);
}

TEST(DisplayGeometryTests, deriveDisplayLinks_narrowInterval_serializesNonEmpty)
{
    // A 1px contact on a 300px union is a sub-0.5% interval: qRound
    // collapses both endpoints to the same percentage, so the GUI
    // serialization must widen the upper endpoint to keep the emitted
    // config interval non-empty (and skip links it cannot represent).
    const std::vector<DisplayLink> links = deriveDisplayLinks(
        "A", {{0, 0, 300, 100}}, ScreenOrigin{0, 0},
        "B", {{149, 100, 1, 100}}, ScreenOrigin{0, 0}, 15);
    ASSERT_EQ(1u, links.size());
    const DisplayLink* link = findLink(links, "A", kBottom, "B", kTop);
    ASSERT_TRUE(link != NULL);

    const float width = link->sourceInterval.second - link->sourceInterval.first;
    ASSERT_GT(width, 0.0f);
    ASSERT_LT(width, 0.005f);

    // Same rounding as the GUI serialization (qRound(f * 100)).
    int start = static_cast<int>(link->sourceInterval.first * 100.0f + 0.5f);
    int end = static_cast<int>(link->sourceInterval.second * 100.0f + 0.5f);
    if (end <= start) {
        end = start + 1;
    }
    EXPECT_GT(end, start);
    EXPECT_LE(end, 100);
}

TEST(DisplayGeometryTests, localServerDisplayBoundaryIsNotBarrierOwned)
{
    // The MBP screen's own displays: crossing between LG and VG is a
    // same-host boundary the local OS moves the cursor across, so
    // Barrier must neither switch nor clamp.
    const std::vector<ScreenRect> displays = lShapeMbpDisplays();
    const ScreenRect& lg = displays[0];
    const ScreenRect& vg = displays[1];

    // LG's right edge is shared with the portrait display of the same
    // screen.
    EXPECT_EQ(classifyDisplayBoundary(
                  lg, kRight, displays, mbpLinkIntervals(kRight), 1919, 1500),
              BoundaryOwner::LocalOS);

    // The lower part of the portrait's left edge (below the LU link
    // interval, which covers only VG-local y < 840) is the same
    // LG<->VG boundary and is likewise LocalOS.
    EXPECT_EQ(classifyDisplayBoundary(
                  vg, kLeft, displays, mbpLinkIntervals(kLeft), 1920, 1500),
              BoundaryOwner::LocalOS);
}

TEST(DisplayGeometryTests, unmatchedExternalBoundaryClamps)
{
    const std::vector<ScreenRect> displays = lShapeMbpDisplays();
    const ScreenRect& lg = displays[0];
    const ScreenRect& vg = displays[1];

    // LG's left edge faces empty space: external and unlinked -> Clamp.
    EXPECT_EQ(classifyDisplayBoundary(
                  lg, kLeft, displays, mbpLinkIntervals(kLeft), 0, 1500),
              BoundaryOwner::Clamp);

    // The portrait's top edge is external too; the LU top link interval
    // belongs to LG's edge span (X [0, 1920/3000] of the union), so a
    // crossing at x=2500 must not fall through to that link -> Clamp.
    EXPECT_EQ(classifyDisplayBoundary(
                  vg, kTop, displays, mbpLinkIntervals(kTop), 2500, 0),
              BoundaryOwner::Clamp);
}

TEST(DisplayGeometryTests, linkedExternalEdgesStayRemote)
{
    const std::vector<ScreenRect> displays = lShapeMbpDisplays();
    const ScreenRect& lg = displays[0];
    const ScreenRect& vg = displays[1];

    // The portrait's left edge inside the LU link interval -> Remote.
    EXPECT_EQ(classifyDisplayBoundary(
                  vg, kLeft, displays, mbpLinkIntervals(kLeft), 1920, 500),
              BoundaryOwner::Remote);

    // LG's top edge is linked across the whole LU overlap; the interval
    // is encoded against the union (X [0, 1920/3000]), so a crossing at
    // x=1500 must match even though the display itself spans 1920.
    EXPECT_EQ(classifyDisplayBoundary(
                  lg, kTop, displays, mbpLinkIntervals(kTop), 1500, 840),
              BoundaryOwner::Remote);
}

TEST(DisplayGeometryTests, localOSBoundaryIgnoresConfiguredLinkAtCoordinate)
{
    // LocalOS is decided by physical adjacency before any configured
    // link is consulted: even a link covering the cursor coordinate
    // must not classify a same-host boundary as Remote.  The server
    // relies on this to skip neighbor lookup and cancel any pending
    // switch while the OS moves the cursor natively.
    const std::vector<ScreenRect> displays = lShapeMbpDisplays();
    const ScreenRect& vg = displays[1];

    // VG's left edge at y=1500 is shared with LG; a covering link
    // interval must not win.
    const std::vector<Config::Interval> coveringLinks = {
        Config::Interval(0.0f, 1.0f)};
    EXPECT_EQ(classifyDisplayBoundary(
                  vg, kLeft, displays, coveringLinks, 1920, 1500),
              BoundaryOwner::LocalOS);
}

TEST(DisplayGeometryTests, roundedLinkEndpointStillMatchesDisplaySpan)
{
    // A 1440px display on a 2160px union has an edge span ending at
    // exactly 1440/2160 = 0.6667, but the GUI serializes the link
    // endpoint as qRound(0.6667 * 100) = 67 -> 0.67.  One encoded
    // percentage step of rounding must not reject the link as not
    // belonging to this display's edge.
    const std::vector<ScreenRect> displays = {{0, 0, 1440, 1080},
                                              {1440, 0, 720, 1080}};
    const std::vector<Config::Interval> links = {
        Config::Interval(0.0f, 0.67f)};

    EXPECT_EQ(classifyDisplayBoundary(
                  displays[0], kTop, displays, links, 1000, 0),
              BoundaryOwner::Remote);

    // The same quantized interval must still not reach the other
    // display of the union: its edge span starts at 1440/2160, so the
    // interval does not belong to it -> Clamp.
    EXPECT_EQ(classifyDisplayBoundary(
                  displays[1], kTop, displays, links, 1443, 0),
              BoundaryOwner::Clamp);
}

TEST(DisplayGeometryTests, displayLabelAt_resolvesDisplayContainingPoint)
{
    // the L-shape MBP snapshot: LG below-left, VG (portrait) to its
    // right; names line up with the rectangle order
    const std::vector<ScreenRect> displays = {{0, 840, 1920, 1080},
                                              {1920, 0, 1080, 1920}};
    const std::vector<std::string> names = {"LG", "VG"};

    EXPECT_EQ("LG", displayLabelAt(displays, names, 500, 1500));
    EXPECT_EQ("VG", displayLabelAt(displays, names, 2500, 500));

    // half-open bounds: the shared x=1920 edge belongs to the right
    // display only, matching displayIndexAt() on the platform side
    EXPECT_EQ("LG", displayLabelAt(displays, names, 1919, 1500));
    EXPECT_EQ("VG", displayLabelAt(displays, names, 1920, 500));
}

TEST(DisplayGeometryTests, displayLabelAt_returnsEmptyWhenNamesDoNotCoverSnapshot)
{
    const std::vector<ScreenRect> displays = {{0, 0, 1920, 1080},
                                              {1920, 0, 1080, 1920}};

    // a peer that never exchanged names reports an empty name set
    const std::vector<std::string> noNames;
    EXPECT_EQ("", displayLabelAt(displays, noNames, 500, 500));

    // a mismatched count can never be matched to the rectangles
    const std::vector<std::string> shortNames = {"LU"};
    EXPECT_EQ("", displayLabelAt(displays, shortNames, 500, 500));
    const std::vector<std::string> longNames = {"LU", "VG", "LG"};
    EXPECT_EQ("", displayLabelAt(displays, longNames, 500, 500));
}

TEST(DisplayGeometryTests, displayLabelAt_returnsEmptyForPointOutsideAllDisplays)
{
    const std::vector<ScreenRect> displays = {{0, 0, 1920, 1080},
                                              {1920, -840, 1080, 1920}};
    const std::vector<std::string> names = {"LU", "VG"};

    EXPECT_EQ("", displayLabelAt(displays, names, -1, 0));
    EXPECT_EQ("", displayLabelAt(displays, names, 5000, 5000));
}

TEST(DisplayGeometryTests, displayLabelAt_returnsEmptyForEmptyMatchingName)
{
    // an empty product name is a valid per-display DDNM entry; it must
    // never render as a blank parenthesis in a log line
    const std::vector<ScreenRect> displays = {{0, 0, 1920, 1080}};
    const std::vector<std::string> names = {""};
    EXPECT_EQ("", displayLabelAt(displays, names, 500, 500));
}

TEST(DisplayGeometryTests, displayLabelSuffix_keepsUnknownLabelsBare)
{
    EXPECT_EQ(" (VG)", displayLabelSuffix("VG"));
    EXPECT_EQ(" (LU)", displayLabelSuffix("LU"));
    EXPECT_EQ("", displayLabelSuffix(""));
}
