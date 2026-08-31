/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "server/Config.h"

#include "test/global/gtest.h"

#include <sstream>
#include <string>

namespace {

barrier::DisplayTopology topology(const std::string& primaryId,
                                  const std::string& secondaryId,
                                  SInt32 secondaryX,
                                  SInt32 secondaryY)
{
    barrier::DisplayTopology value;
    value.displays = {
        {primaryId, {0, 0, 100, 100}, 0, true},
        {secondaryId, {secondaryX, secondaryY, 100, 100}, 0, false}
    };
    return value.normalized();
}

Config::TopologyProfile horizontalProfile()
{
    Config::TopologyProfile profile;
    profile.topology = topology("internal", "external", 100, 0);
    profile.key = profile.topology.profileKey();
    profile.screenPositions["server"] = {0, 0};
    profile.screenPositions["client"] = {100, 0};
    profile.displayRects["server"] = {{0, 0, 100, 100}};
    profile.displayRects["client"] = {{0, 0, 100, 100}};
    return profile;
}

Config::TopologyProfile verticalProfile()
{
    Config::TopologyProfile profile;
    profile.topology = topology("internal", "external", 0, 100);
    profile.key = profile.topology.profileKey();
    profile.screenPositions["server"] = {0, 0};
    profile.screenPositions["client"] = {0, 100};
    profile.displayRects["server"] = {{0, 0, 100, 100}};
    profile.displayRects["client"] = {{0, 0, 100, 100}};
    return profile;
}

Config configuredScreens()
{
    Config config(nullptr);
    EXPECT_TRUE(config.addScreen("server"));
    EXPECT_TRUE(config.addScreen("client"));
    return config;
}

std::string serialize(const Config& config)
{
    std::ostringstream stream;
    stream << config;
    return stream.str();
}

Config parse(const std::string& text)
{
    Config config(nullptr);
    std::istringstream stream(text);
    stream >> config;
    return config;
}

} // namespace

TEST(TopologyProfileConfigTests, twoProfilesRoundTripThroughParserAndWriter)
{
    Config config = configuredScreens();
    ASSERT_TRUE(config.addTopologyProfile(horizontalProfile()));
    ASSERT_TRUE(config.addTopologyProfile(verticalProfile()));

    const Config parsed = parse(serialize(config));

    EXPECT_EQ(2u, parsed.topologyProfiles().size());
    EXPECT_EQ(config, parsed);
}

TEST(TopologyProfileConfigTests, exactTopologySelectsMatchingProfile)
{
    Config config = configuredScreens();
    const Config::TopologyProfile profile = horizontalProfile();
    ASSERT_TRUE(config.addTopologyProfile(profile));

    EXPECT_EQ(Config::TopologySelectionResult::Known,
              config.selectTopology(profile.topology));
    EXPECT_EQ(profile.key, config.activeTopologyProfileKey());
    EXPECT_EQ("client", config.getNeighbor("server", kRight, 0.5f, nullptr));
}

TEST(TopologyProfileConfigTests,
     multiDisplayServerUsesScreenUnionForDistinctEdgeContacts)
{
    Config config(nullptr);
    ASSERT_TRUE(config.addScreen("server-primary"));
    ASSERT_TRUE(config.addScreen("client-northwest"));
    ASSERT_TRUE(config.addScreen("client-northeast"));

    Config::TopologyProfile profile;
    profile.topology.displays = {
        {"internal", {0, 0, 1920, 1080}, 0, true},
        {"external", {1920, 272, 1352, 878}, 0, false}
    };
    profile.topology = profile.topology.normalized();
    profile.key = profile.topology.profileKey();
    profile.screenPositions["server-primary"] = {0, 0};
    profile.screenPositions["client-northwest"] = {0, -1080};
    profile.screenPositions["client-northeast"] = {1920, -808};
    profile.displayRects["server-primary"] = {
        {0, 0, 1920, 1080},
        {1920, 272, 1352, 878}
    };
    profile.displayRects["client-northwest"] = {{0, 0, 1920, 1080}};
    profile.displayRects["client-northeast"] = {{0, 0, 1920, 1080}};
    ASSERT_TRUE(config.addTopologyProfile(profile));

    Config reloaded = parse(serialize(config));
    ASSERT_EQ(Config::TopologySelectionResult::Known,
              reloaded.selectTopology(profile.topology));
    EXPECT_EQ("client-northwest",
              reloaded.getNeighbor("server-primary", kTop, 0.25f, nullptr));
    EXPECT_EQ("client-northeast",
              reloaded.getNeighbor("server-primary", kTop, 0.75f, nullptr));
}

TEST(TopologyProfileConfigTests, selectedRuntimeLinksAreNotSerializedAsLegacyLinks)
{
    Config config = configuredScreens();
    const Config::TopologyProfile profile = horizontalProfile();
    ASSERT_TRUE(config.addTopologyProfile(profile));
    ASSERT_EQ(Config::TopologySelectionResult::Known,
              config.selectTopology(profile.topology));

    const Config parsed = parse(serialize(config));

    EXPECT_TRUE(parsed.activeTopologyProfileKey().empty());
    EXPECT_TRUE(parsed.getNeighbor("server", kRight, 0.5f, nullptr).empty());
    EXPECT_EQ(1u, parsed.topologyProfiles().size());
}

TEST(TopologyProfileConfigTests, unknownAndEmptyTopologiesClearRuntimeLinks)
{
    Config config = configuredScreens();
    const Config::TopologyProfile profile = horizontalProfile();
    ASSERT_TRUE(config.addTopologyProfile(profile));
    ASSERT_EQ(Config::TopologySelectionResult::Known,
              config.selectTopology(profile.topology));

    const barrier::DisplayTopology unknown =
        topology("different-internal", "external", 100, 0);
    EXPECT_EQ(Config::TopologySelectionResult::Unknown,
              config.selectTopology(unknown));
    EXPECT_TRUE(config.activeTopologyProfileKey().empty());
    EXPECT_TRUE(config.getNeighbor("server", kRight, 0.5f, nullptr).empty());

    EXPECT_EQ(Config::TopologySelectionResult::Unavailable,
              config.selectTopology(barrier::DisplayTopology()));
    EXPECT_TRUE(config.getNeighbor("server", kRight, 0.5f, nullptr).empty());
}

TEST(TopologyProfileConfigTests, switchingProfilesRestoresOnlySelectedLinks)
{
    Config config = configuredScreens();
    const Config::TopologyProfile horizontal = horizontalProfile();
    const Config::TopologyProfile vertical = verticalProfile();
    ASSERT_TRUE(config.addTopologyProfile(horizontal));
    ASSERT_TRUE(config.addTopologyProfile(vertical));

    ASSERT_EQ(Config::TopologySelectionResult::Known,
              config.selectTopology(horizontal.topology));
    EXPECT_EQ("client", config.getNeighbor("server", kRight, 0.5f, nullptr));
    EXPECT_TRUE(config.getNeighbor("server", kBottom, 0.5f, nullptr).empty());

    ASSERT_EQ(Config::TopologySelectionResult::Known,
              config.selectTopology(vertical.topology));
    EXPECT_TRUE(config.getNeighbor("server", kRight, 0.5f, nullptr).empty());
    EXPECT_EQ("client", config.getNeighbor("server", kBottom, 0.5f, nullptr));

    ASSERT_EQ(Config::TopologySelectionResult::Known,
              config.selectTopology(horizontal.topology));
    EXPECT_EQ("client", config.getNeighbor("server", kRight, 0.5f, nullptr));
    EXPECT_TRUE(config.getNeighbor("server", kBottom, 0.5f, nullptr).empty());
}

TEST(TopologyProfileConfigTests, profileActivationPreservesScreenOptions)
{
    Config config = configuredScreens();
    ASSERT_TRUE(config.addOption("server", kOptionScreenSwitchCornerSize, 24));
    const Config::TopologyProfile profile = horizontalProfile();
    ASSERT_TRUE(config.addTopologyProfile(profile));

    ASSERT_EQ(Config::TopologySelectionResult::Known,
              config.selectTopology(profile.topology));

    const Config::ScreenOptions* options = config.getOptions("server");
    ASSERT_NE(nullptr, options);
    ASSERT_EQ(1u, options->count(kOptionScreenSwitchCornerSize));
    EXPECT_EQ(24, options->at(kOptionScreenSwitchCornerSize));
}

TEST(TopologyProfileConfigTests, copyAndAssignmentPreserveProfilesAndSelection)
{
    Config config = configuredScreens();
    const Config::TopologyProfile profile = horizontalProfile();
    ASSERT_TRUE(config.addTopologyProfile(profile));
    ASSERT_EQ(Config::TopologySelectionResult::Known,
              config.selectTopology(profile.topology));

    const Config copied(config);
    EXPECT_EQ(config, copied);
    EXPECT_EQ("client", copied.getNeighbor("server", kRight, 0.5f, nullptr));

    Config assigned(nullptr);
    assigned = config;
    EXPECT_EQ(config, assigned);
    EXPECT_EQ(profile.key, assigned.activeTopologyProfileKey());
}

TEST(TopologyProfileConfigTests, malformedStoredKeyIsRejected)
{
    Config config = configuredScreens();
    ASSERT_TRUE(config.addTopologyProfile(horizontalProfile()));
    std::string text = serialize(config);
    const std::string marker = "profile = ";
    const std::string::size_type key = text.find(marker);
    ASSERT_NE(std::string::npos, key);
    text.replace(key + marker.size(), 64, std::string(64, '0'));

    EXPECT_THROW(parse(text), XConfigRead);
}

TEST(TopologyProfileConfigTests, duplicateStoredProfileKeyIsRejected)
{
    Config config = configuredScreens();
    ASSERT_TRUE(config.addTopologyProfile(horizontalProfile()));
    std::string text = serialize(config);
    const std::string::size_type profileStart = text.find("\tprofile = ");
    const std::string::size_type profileEnd = text.find("\tend-profile\n", profileStart);
    ASSERT_NE(std::string::npos, profileStart);
    ASSERT_NE(std::string::npos, profileEnd);
    const std::string block = text.substr(
        profileStart, profileEnd + std::string("\tend-profile\n").size() - profileStart);
    text.insert(profileEnd + std::string("\tend-profile\n").size(), block);

    EXPECT_THROW(parse(text), XConfigRead);
}

TEST(TopologyProfileConfigTests, identicalMirroredRectanglesRoundTrip)
{
    Config config = configuredScreens();
    Config::TopologyProfile profile = horizontalProfile();
    profile.displayRects["client"].push_back(
        profile.displayRects["client"].front());
    ASSERT_TRUE(config.addTopologyProfile(profile));

    const Config parsed = parse(serialize(config));

    ASSERT_EQ(1u, parsed.topologyProfiles().size());
    EXPECT_EQ(2u, parsed.topologyProfiles()
                      .at(profile.key).displayRects.at("client").size());
}

TEST(TopologyProfileConfigTests, geometryBeforeTopologyVersionIsRejected)
{
    Config config = configuredScreens();
    ASSERT_TRUE(config.addTopologyProfile(horizontalProfile()));
    std::string text = serialize(config);
    const std::string positionMarker = "\t\tposition = ";
    const std::string::size_type positionStart = text.find(positionMarker);
    ASSERT_NE(std::string::npos, positionStart);
    const std::string::size_type positionEnd = text.find('\n', positionStart);
    ASSERT_NE(std::string::npos, positionEnd);
    const std::string position =
        text.substr(positionStart, positionEnd - positionStart + 1);
    text.erase(positionStart, position.size());
    const std::string topologyMarker = "\t\ttopology-version = ";
    const std::string::size_type topologyStart = text.find(topologyMarker);
    ASSERT_NE(std::string::npos, topologyStart);
    text.insert(topologyStart, position);

    EXPECT_THROW(parse(text), XConfigRead);
}

TEST(TopologyProfileConfigTests, invalidProfileReferencesAreRejectedBeforeStorage)
{
    Config config = configuredScreens();
    Config::TopologyProfile profile = horizontalProfile();
    profile.screenPositions["missing"] = {0, 0};
    EXPECT_FALSE(config.addTopologyProfile(profile));

    profile = horizontalProfile();
    profile.displayRects["client"] = {{0, 0, 0, 100}};
    EXPECT_FALSE(config.addTopologyProfile(profile));

    profile = horizontalProfile();
    profile.screenPositions.clear();
    profile.displayRects.clear();
    EXPECT_FALSE(config.addTopologyProfile(profile));

    profile = horizontalProfile();
    profile.screenPositions.erase("client");
    profile.displayRects.erase("client");
    EXPECT_FALSE(config.addTopologyProfile(profile));

}

TEST(TopologyProfileConfigTests, addingConfiguredScreenInvalidatesIncompleteProfiles)
{
    Config config = configuredScreens();
    ASSERT_TRUE(config.addTopologyProfile(horizontalProfile()));

    ASSERT_TRUE(config.addScreen("new-client"));

    EXPECT_TRUE(config.topologyProfiles().empty());
    EXPECT_NO_THROW(parse(serialize(config)));
}

TEST(TopologyProfileConfigTests, removingConfiguredScreenPrunesProfiles)
{
    Config config = configuredScreens();
    const Config::TopologyProfile profile = horizontalProfile();
    ASSERT_TRUE(config.addTopologyProfile(profile));

    config.removeScreen("client");

    ASSERT_EQ(1u, config.topologyProfiles().size());
    EXPECT_EQ(1u, config.topologyProfiles().at(profile.key).screenPositions.size());
    EXPECT_EQ(config, parse(serialize(config)));
}

TEST(TopologyProfileConfigTests, failedGenerationPreservesPreviousLinkMap)
{
    Config config(nullptr);
    ASSERT_TRUE(config.addScreen("server"));
    ASSERT_TRUE(config.addScreen("client-a"));
    ASSERT_TRUE(config.addScreen("client-b"));
    ASSERT_TRUE(config.connect(
        "server", kLeft, 0.0f, 1.0f, "client-a", 0.0f, 1.0f));
    config.setScreenPosition("server", 0, 0);
    config.setScreenPosition("client-a", 100, 0);
    config.setScreenPosition("client-b", 100, 0);
    config.setDisplayRects("server", {{0, 0, 100, 100}});
    config.setDisplayRects("client-a", {{0, 0, 100, 100}});
    config.setDisplayRects("client-b", {{0, 0, 100, 100}});

    EXPECT_FALSE(config.generateFreeformLinks());
    EXPECT_EQ("client-a", config.getNeighbor("server", kLeft, 0.5f, nullptr));
    EXPECT_TRUE(config.getNeighbor("server", kRight, 0.5f, nullptr).empty());
}

TEST(TopologyProfileConfigTests, manualRuntimeMutationClearsActiveProfile)
{
    Config config = configuredScreens();
    const Config::TopologyProfile profile = horizontalProfile();
    ASSERT_TRUE(config.addTopologyProfile(profile));
    ASSERT_EQ(Config::TopologySelectionResult::Known,
              config.selectTopology(profile.topology));

    config.setScreenPosition("client", 200, 0);
    EXPECT_TRUE(config.activeTopologyProfileKey().empty());
    EXPECT_TRUE(config.getNeighbor("server", kRight, 0.5f, nullptr).empty());

    ASSERT_EQ(Config::TopologySelectionResult::Known,
              config.selectTopology(profile.topology));
    ASSERT_TRUE(config.connect(
        "server", kLeft, 0.0f, 1.0f, "client", 0.0f, 1.0f));
    EXPECT_TRUE(config.activeTopologyProfileKey().empty());
    EXPECT_EQ("client", config.getNeighbor("server", kLeft, 0.5f, nullptr));
    EXPECT_TRUE(config.getNeighbor("server", kRight, 0.5f, nullptr).empty());

    ASSERT_EQ(Config::TopologySelectionResult::Known,
              config.selectTopology(profile.topology));
    config.removeScreen("client");
    EXPECT_TRUE(config.activeTopologyProfileKey().empty());
    EXPECT_TRUE(config.getNeighbor("server", kRight, 0.5f, nullptr).empty());
}

TEST(TopologyProfileConfigTests, renamingThroughAliasIsRejected)
{
    Config config = configuredScreens();
    ASSERT_TRUE(config.addAlias("server", "server-alias"));

    EXPECT_FALSE(config.renameScreen("server-alias", "renamed"));
    EXPECT_TRUE(config.isScreen("server"));
    EXPECT_TRUE(config.isScreen("server-alias"));
}

TEST(TopologyProfileConfigTests, invalidatedStoredProfileCannotActivate)
{
    Config config = configuredScreens();
    const Config::TopologyProfile profile = horizontalProfile();
    ASSERT_TRUE(config.addTopologyProfile(profile));
    config.removeScreen("server");
    config.removeScreen("client");

    EXPECT_EQ(Config::TopologySelectionResult::Unknown,
              config.selectTopology(profile.topology));
    EXPECT_TRUE(config.activeTopologyProfileKey().empty());
}

TEST(TopologyProfileConfigTests, conflictingProfileLinksAbortActivation)
{
    Config config(nullptr);
    ASSERT_TRUE(config.addScreen("server"));
    ASSERT_TRUE(config.addScreen("client-a"));
    ASSERT_TRUE(config.addScreen("client-b"));

    Config::TopologyProfile profile;
    profile.topology = topology("internal", "external", 100, 0);
    profile.key = profile.topology.profileKey();
    profile.screenPositions["server"] = {0, 0};
    profile.screenPositions["client-a"] = {100, 0};
    profile.screenPositions["client-b"] = {100, 0};
    profile.displayRects["server"] = {{0, 0, 100, 100}};
    profile.displayRects["client-a"] = {{0, 0, 100, 100}};
    profile.displayRects["client-b"] = {{0, 0, 100, 100}};
    ASSERT_TRUE(config.addTopologyProfile(profile));

    EXPECT_EQ(Config::TopologySelectionResult::Unknown,
              config.selectTopology(profile.topology));
    EXPECT_TRUE(config.activeTopologyProfileKey().empty());
    EXPECT_TRUE(config.getNeighbor("server", kRight, 0.5f, nullptr).empty());
}

TEST(TopologyProfileConfigTests, legacyLinksAreNeverSelectedAsTopologyFallback)
{
    const std::string legacy =
        "section: screens\n"
        "\tserver:\n"
        "\tclient:\n"
        "end\n"
        "section: links\n"
        "\tserver:\n"
        "\t\tright = client\n"
        "\tclient:\n"
        "end\n"
        "section: options\n"
        "end\n";
    Config config = parse(legacy);

    EXPECT_TRUE(config.topologyProfiles().empty());
    EXPECT_TRUE(config.activeTopologyProfileKey().empty());
    EXPECT_EQ("client", config.getNeighbor("server", kRight, 0.5f, nullptr));

    EXPECT_EQ(Config::TopologySelectionResult::Unknown,
              config.selectTopology(horizontalProfile().topology));
    EXPECT_TRUE(config.getNeighbor("server", kRight, 0.5f, nullptr).empty());
}
