/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/DisplayTopology.h"

#include <gtest/gtest.h>
#include <stdexcept>
#include <vector>

namespace {

barrier::DisplayTopology makeTopology(SInt32 offsetX = 0, SInt32 offsetY = 0)
{
    barrier::DisplayTopology topology;
    topology.displays.push_back({"internal", {offsetX, offsetY, 1512, 982}, 0, true});
    topology.displays.push_back({"external", {offsetX + 1512, offsetY - 98, 1920, 1080}, 90, false});
    return topology;
}

} // namespace

TEST(DisplayTopologyTests, emptyTopologyHasNoProfileKey)
{
    barrier::DisplayTopology topology;
    EXPECT_TRUE(topology.empty());
    EXPECT_TRUE(topology.profileKey().empty());
}

TEST(DisplayTopologyTests, translationDoesNotChangeIdentity)
{
    EXPECT_EQ(makeTopology().profileKey(), makeTopology(400, -250).profileKey());
}

TEST(DisplayTopologyTests, inputOrderDoesNotChangeIdentity)
{
    barrier::DisplayTopology reversed = makeTopology();
    std::swap(reversed.displays[0], reversed.displays[1]);
    EXPECT_EQ(makeTopology().profileKey(), reversed.profileKey());
}

TEST(DisplayTopologyTests, normalizedCoordinatesRemainSigned)
{
    const barrier::DisplayTopology normalized = makeTopology().normalized();
    ASSERT_EQ(2u, normalized.displays.size());
    EXPECT_EQ(-98, normalized.displays[0].logicalBounds.y);
    EXPECT_EQ(0, normalized.displays[1].logicalBounds.x);
}

TEST(DisplayTopologyTests, everyIdentityFieldChangesTheKey)
{
    const std::string base = makeTopology().profileKey();

    barrier::DisplayTopology geometry = makeTopology();
    geometry.displays[1].logicalBounds.x += 1;
    EXPECT_NE(base, geometry.profileKey());

    barrier::DisplayTopology size = makeTopology();
    size.displays[1].logicalBounds.w += 1;
    EXPECT_NE(base, size.profileKey());


    barrier::DisplayTopology rotation = makeTopology();
    rotation.displays[1].rotationDegrees = 180;
    EXPECT_NE(base, rotation.profileKey());

    barrier::DisplayTopology stableId = makeTopology();
    stableId.displays[1].stableId = "other-external";
    EXPECT_NE(base, stableId.profileKey());

    barrier::DisplayTopology primary = makeTopology();
    primary.displays[0].primary = false;
    primary.displays[1].primary = true;
    EXPECT_NE(base, primary.profileKey());
}
TEST(DisplayTopologyTests, canonicalIdentityHexEncodesStableIdBytes)
{
    barrier::DisplayTopology topology = makeTopology();
    topology.displays[0].stableId = std::string("\xc3\xa9", 2);
    const std::string identity = topology.canonicalIdentity();
    EXPECT_NE(std::string::npos, identity.find("c3a9"));
    for (unsigned char byte : identity) {
        EXPECT_LT(byte, 0x80);
    }
}

TEST(DisplayTopologyTests, canonicalIdentityIsVersionedAndDeterministic)
{
    const std::string identity = makeTopology().canonicalIdentity();
    EXPECT_EQ(0u, identity.find("display-topology-v1|"));
    EXPECT_EQ(identity, makeTopology(20, 30).canonicalIdentity());
}

TEST(DisplayTopologyTests, rejectsInvalidNonEmptyTopologies)
{
    const auto expectInvalid = [](barrier::DisplayTopology topology) {
        EXPECT_THROW(topology.profileKey(), std::invalid_argument);
    };

    barrier::DisplayTopology missingPrimary = makeTopology();
    missingPrimary.displays[0].primary = false;
    expectInvalid(missingPrimary);

    barrier::DisplayTopology twoPrimaries = makeTopology();
    twoPrimaries.displays[1].primary = true;
    expectInvalid(twoPrimaries);

    barrier::DisplayTopology duplicateId = makeTopology();
    duplicateId.displays[1].stableId = duplicateId.displays[0].stableId;
    expectInvalid(duplicateId);

    barrier::DisplayTopology emptyId = makeTopology();
    emptyId.displays[0].stableId.clear();
    expectInvalid(emptyId);

    barrier::DisplayTopology zeroWidth = makeTopology();
    zeroWidth.displays[0].logicalBounds.w = 0;
    expectInvalid(zeroWidth);

    barrier::DisplayTopology negativeHeight = makeTopology();
    negativeHeight.displays[0].logicalBounds.h = -1;
    expectInvalid(negativeHeight);

    barrier::DisplayTopology invalidRotation = makeTopology();
    invalidRotation.displays[0].rotationDegrees = 45;
    expectInvalid(invalidRotation);
}
