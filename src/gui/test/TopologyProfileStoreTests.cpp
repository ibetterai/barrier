/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "../src/TopologyProfileStore.h"

#include <gtest/gtest.h>

#include <QBuffer>
#include <QDataStream>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>

namespace {

barrier::DisplayTopology topology(const std::string& primary,
                                  const std::string& secondary,
                                  int secondaryX,
                                  int secondaryY)
{
    barrier::DisplayTopology result;
    result.displays = {
        {primary, {0, 0, 1920, 1080}, 0, true},
        {secondary, {secondaryX, secondaryY, 1080, 1920}, 90, false}
    };
    return result.normalized();
}

barrier::TopologyProfile profile(const barrier::DisplayTopology& value,
                                 int clientX,
                                 int clientY)
{
    barrier::TopologyProfile result;
    result.topology = value;
    result.positions["server"] = std::make_pair(0, 0);
    result.positions["client"] = std::make_pair(clientX, clientY);
    result.displayRects["server"] = {
        QRect(0, 0, 1920, 1080),
        QRect(1920, -840, 1080, 1920)
    };
    result.displayRects["client"] = {QRect(0, 0, 1920, 1080)};
    return result;
}

QString settingsPath(QTemporaryDir& directory)
{
    return directory.filePath("barrier.ini");
}

} // namespace

TEST(TopologyProfileStoreTests, twoExactProfilesPersistIndependently)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(settingsPath(directory), QSettings::IniFormat);
    const barrier::DisplayTopology a = topology("internal", "left", -1080, 0);
    const barrier::DisplayTopology b = topology("internal", "right", 1920, -840);
    barrier::TopologyProfiles profiles;
    ASSERT_TRUE(barrier::putTopologyProfile(profiles, profile(a, -1920, 0)));
    ASSERT_TRUE(barrier::putTopologyProfile(profiles, profile(b, 1920, 0)));

    QString error;
    ASSERT_EQ(barrier::TopologyProfileStoreResult::Ok,
              barrier::TopologyProfileStore::save(settings, profiles, &error))
        << error.toStdString();

    barrier::TopologyProfiles loaded;
    ASSERT_EQ(barrier::TopologyProfileStoreResult::Ok,
              barrier::TopologyProfileStore::load(settings, loaded, &error))
        << error.toStdString();
    ASSERT_EQ(2u, loaded.size());
    EXPECT_EQ(std::make_pair(-1920, 0),
              loaded.at(a.profileKey()).positions.at("client"));
    EXPECT_EQ(std::make_pair(1920, 0),
              loaded.at(b.profileKey()).positions.at("client"));
}

TEST(TopologyProfileStoreTests, selectingAThenBThenARestoresEachGeometry)
{
    const barrier::DisplayTopology a = topology("internal", "left", -1080, 0);
    const barrier::DisplayTopology b = topology("internal", "right", 1920, -840);
    barrier::TopologyProfiles profiles;
    ASSERT_TRUE(barrier::putTopologyProfile(profiles, profile(a, -1920, 0)));
    ASSERT_TRUE(barrier::putTopologyProfile(profiles, profile(b, 1920, 0)));
    barrier::FreeformPositions legacyPositions;
    legacyPositions["client"] = std::make_pair(7, 9);
    barrier::FreeformDisplayRects legacyRects;

    barrier::TopologyProfileSelection selection = barrier::selectTopologyProfile(
        profiles, a, "server", legacyPositions, legacyRects);
    ASSERT_TRUE(selection.saved);
    EXPECT_EQ(std::make_pair(-1920, 0), selection.positions.at("client"));

    selection = barrier::selectTopologyProfile(
        profiles, b, "server", legacyPositions, legacyRects);
    ASSERT_TRUE(selection.saved);
    EXPECT_EQ(std::make_pair(1920, 0), selection.positions.at("client"));

    selection = barrier::selectTopologyProfile(
        profiles, a, "server", legacyPositions, legacyRects);
    ASSERT_TRUE(selection.saved);
    EXPECT_EQ(std::make_pair(-1920, 0), selection.positions.at("client"));
}

TEST(TopologyProfileStoreTests, savingUnknownTopologyAddsOnlyItsExactKey)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(settingsPath(directory), QSettings::IniFormat);
    const barrier::DisplayTopology a = topology("internal", "left", -1080, 0);
    const barrier::DisplayTopology b = topology("internal", "right", 1920, -840);
    barrier::TopologyProfiles profiles;
    ASSERT_TRUE(barrier::putTopologyProfile(profiles, profile(a, -1920, 0)));
    ASSERT_EQ(barrier::TopologyProfileStoreResult::Ok,
              barrier::TopologyProfileStore::save(settings, profiles));

    ASSERT_TRUE(barrier::putTopologyProfile(profiles, profile(b, 1920, 0)));
    ASSERT_EQ(barrier::TopologyProfileStoreResult::Ok,
              barrier::TopologyProfileStore::save(settings, profiles));

    barrier::TopologyProfiles loaded;
    ASSERT_EQ(barrier::TopologyProfileStoreResult::Ok,
              barrier::TopologyProfileStore::load(settings, loaded));
    ASSERT_EQ(2u, loaded.size());
    EXPECT_NE(loaded.end(), loaded.find(a.profileKey()));
    EXPECT_NE(loaded.end(), loaded.find(b.profileKey()));
}

TEST(TopologyProfileStoreTests, cancelledEditDoesNotWriteProfiles)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(settingsPath(directory), QSettings::IniFormat);
    const barrier::DisplayTopology a = topology("internal", "left", -1080, 0);
    const barrier::DisplayTopology b = topology("internal", "right", 1920, -840);
    barrier::TopologyProfiles profiles;
    ASSERT_TRUE(barrier::putTopologyProfile(profiles, profile(a, -1920, 0)));
    ASSERT_EQ(barrier::TopologyProfileStoreResult::Ok,
              barrier::TopologyProfileStore::save(settings, profiles));
    const QByteArray livePayloadBeforeCancel =
        settings.value("topologyProfiles/payload").toByteArray();

    barrier::TopologyProfiles dialogCopy = profiles;
    ASSERT_TRUE(barrier::putTopologyProfile(dialogCopy, profile(b, 1920, 0)));
    // Simulate Cancel: the edited copy is discarded without save().

    settings.sync();
    EXPECT_EQ(livePayloadBeforeCancel,
              settings.value("topologyProfiles/payload").toByteArray());
    barrier::TopologyProfiles loaded;
    ASSERT_EQ(barrier::TopologyProfileStoreResult::Ok,
              barrier::TopologyProfileStore::load(settings, loaded));
    ASSERT_EQ(1u, loaded.size());
    EXPECT_NE(loaded.end(), loaded.find(a.profileKey()));
    EXPECT_EQ(loaded.end(), loaded.find(b.profileKey()));
}

TEST(TopologyProfileStoreTests, malformedPayloadReturnsErrorWithoutMutation)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(settingsPath(directory), QSettings::IniFormat);
    settings.setValue("topologyProfiles/payload", QByteArray("not-a-profile-store"));
    settings.sync();

    const barrier::DisplayTopology a = topology("internal", "left", -1080, 0);
    barrier::TopologyProfiles profiles;
    ASSERT_TRUE(barrier::putTopologyProfile(profiles, profile(a, -1920, 0)));
    const barrier::TopologyProfiles original = profiles;
    QString error;

    EXPECT_EQ(barrier::TopologyProfileStoreResult::Malformed,
              barrier::TopologyProfileStore::load(settings, profiles, &error));
    EXPECT_FALSE(error.isEmpty());
    ASSERT_EQ(original.size(), profiles.size());
    EXPECT_NE(profiles.end(), profiles.find(a.profileKey()));
}

TEST(TopologyProfileStoreTests, rejectsOversizedFieldBeforeReadingItsBytes)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(settingsPath(directory), QSettings::IniFormat);
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_5_9);
    stream << quint32(0x42545046) << quint32(1) << quint32(1)
           << quint32(0xffffffff);
    settings.setValue("topologyProfiles/payload", payload);
    settings.sync();

    barrier::TopologyProfiles profiles;
    QString error;
    EXPECT_EQ(barrier::TopologyProfileStoreResult::Malformed,
              barrier::TopologyProfileStore::load(settings, profiles, &error));
    EXPECT_TRUE(profiles.empty());
    EXPECT_FALSE(error.isEmpty());
}

TEST(TopologyProfileStoreTests, legacyGeometrySeedsUnknownWithoutBecomingSaved)
{
    const barrier::DisplayTopology unknown =
        topology("internal", "external", 1920, -840);
    barrier::FreeformPositions legacyPositions;
    legacyPositions["client"] = std::make_pair(4000, 20);
    barrier::FreeformDisplayRects legacyRects;
    legacyRects["server"] = {QRect(0, 0, 800, 600)};
    legacyRects["client"] = {QRect(0, 0, 1024, 768)};

    const barrier::TopologyProfileSelection selection =
        barrier::selectTopologyProfile(
            barrier::TopologyProfiles(), unknown, "server",
            legacyPositions, legacyRects);

    EXPECT_FALSE(selection.saved);
    EXPECT_EQ(std::make_pair(4000, 20), selection.positions.at("client"));
    ASSERT_EQ(2, selection.displayRects.at("server").size());
    EXPECT_EQ(QRect(0, 0, 1920, 1080),
              selection.displayRects.at("server")[0]);
    EXPECT_EQ(QRect(1920, -840, 1080, 1920),
              selection.displayRects.at("server")[1]);
}

TEST(TopologyProfileStoreTests, unknownServerRectsAreRelativeToPrimaryDisplay)
{
    barrier::DisplayTopology unknown;
    unknown.displays = {
        {"primary", {100, -50, 1920, 1080}, 0, true},
        {"external", {-980, 100, 1080, 1920}, 90, false}
    };

    const barrier::TopologyProfileSelection selection =
        barrier::selectTopologyProfile(
            barrier::TopologyProfiles(), unknown, "server",
            barrier::FreeformPositions(), barrier::FreeformDisplayRects());

    ASSERT_EQ(2, selection.displayRects.at("server").size());
    EXPECT_EQ(QRect(0, 0, 1920, 1080),
              selection.displayRects.at("server")[0]);
    EXPECT_EQ(QRect(-1080, 150, 1080, 1920),
              selection.displayRects.at("server")[1]);
}


TEST(TopologyProfileStoreTests, knownProfileUsesCurrentExactServerRects)
{
    barrier::DisplayTopology current;
    current.displays = {
        {"primary", {100, -50, 1920, 1080}, 0, true},
        {"external", {-980, 100, 1080, 1920}, 90, false}
    };
    barrier::TopologyProfiles profiles;
    ASSERT_TRUE(barrier::putTopologyProfile(
        profiles, profile(current.normalized(), 1920, 0)));
    profiles.at(current.profileKey()).displayRects["server"] = {
        QRect(0, 0, 800, 600)
    };

    const barrier::TopologyProfileSelection selection =
        barrier::selectTopologyProfile(
            profiles, current, "server",
            barrier::FreeformPositions(), barrier::FreeformDisplayRects());

    ASSERT_TRUE(selection.saved);
    ASSERT_EQ(2, selection.displayRects.at("server").size());
    EXPECT_EQ(QRect(0, 0, 1920, 1080),
              selection.displayRects.at("server")[0]);
    EXPECT_EQ(QRect(-1080, 150, 1080, 1920),
              selection.displayRects.at("server")[1]);
}
TEST(TopologyProfileStoreTests, configSerializationWritesEveryProfile)
{
    const barrier::DisplayTopology a = topology("internal", "left", -1080, 0);
    const barrier::DisplayTopology b = topology("internal", "right", 1920, -840);
    barrier::TopologyProfiles profiles;
    ASSERT_TRUE(barrier::putTopologyProfile(profiles, profile(a, -1920, 0)));
    ASSERT_TRUE(barrier::putTopologyProfile(profiles, profile(b, 1920, 0)));
    QString output;
    QTextStream stream(&output);

    barrier::writeTopologyProfiles(stream, profiles);

    EXPECT_EQ(1, output.count("section: topology-profiles"));
    EXPECT_EQ(2, output.count("\tprofile = "));
    EXPECT_TRUE(output.contains(QString::fromStdString(a.profileKey())));
    EXPECT_TRUE(output.contains(QString::fromStdString(b.profileKey())));
    EXPECT_EQ(4, output.count("\t\tdisplay = "));
    EXPECT_EQ(4, output.count("\t\tposition = "));
    EXPECT_EQ(6, output.count("\t\trect = "));
    EXPECT_TRUE(output.endsWith("end\n\n"));
}

TEST(TopologyProfileStoreTests, restrictsProfileToConfiguredScreens)
{
    barrier::TopologyProfile stored =
        profile(topology("internal", "external", 1920, 0), 1920, 0);
    stored.positions["removed-client"] = std::make_pair(-1920, 0);
    stored.displayRects["removed-client"] = {QRect(0, 0, 1920, 1080)};

    QString error;
    ASSERT_TRUE(barrier::restrictTopologyProfileToScreens(
        stored, QStringList({"server", "client"}), &error))
        << error.toStdString();

    EXPECT_EQ(2u, stored.positions.size());
    EXPECT_EQ(2u, stored.displayRects.size());
    EXPECT_EQ(0u, stored.positions.count("removed-client"));
    EXPECT_EQ(0u, stored.displayRects.count("removed-client"));
}
