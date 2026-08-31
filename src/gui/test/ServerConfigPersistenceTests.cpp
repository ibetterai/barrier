/*  barrier -- mouse and keyboard sharing utility
    Copyright (C) 2026 William Zhu

    This package is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    found in the file LICENSE that should have accompanied this file.
*/

#include "../src/FreeformLayoutSettings.h"
#include "../src/ServerConfig.h"

#include <gtest/gtest.h>

#include <QSettings>
#include <QTemporaryDir>

namespace {

class TestServerConfig : public ServerConfig
{
public:
    using ServerConfig::ServerConfig;

    void setClipboardSharingForTest(bool enabled)
    {
        setClipboardSharing(enabled);
    }
};

} // namespace

TEST(ServerConfigPersistenceTests, freeformGeometrySurvivesRelaunch)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString settingsPath = directory.filePath("barrier.ini");

    const QList<QRect> serverDisplays = {
        QRect(0, 0, 1920, 1080),
        QRect(1920, -1090, 1080, 1920),
    };
    const QList<QRect> clientDisplays = {
        QRect(0, 0, 1920, 1080),
    };

    {
        QSettings settings(settingsPath, QSettings::IniFormat);
        barrier::FreeformPositions positions;
        positions["server"] = std::make_pair(0, 0);
        positions["client"] = std::make_pair(0, -1080);
        barrier::FreeformDisplayRects displayRects;
        displayRects["server"] = serverDisplays;
        displayRects["client"] = clientDisplays;
        barrier::FreeformDisplayNames displayNames;
        displayNames["client"] = QStringList({"Studio Display"});

        settings.beginGroup("internalConfig");
        barrier::saveFreeformLayoutSettings(
                settings, positions, displayRects, displayNames);
        settings.endGroup();
        settings.sync();
        ASSERT_EQ(settings.status(), QSettings::NoError);
    }

    {
        QSettings settings(settingsPath, QSettings::IniFormat);
        barrier::FreeformPositions positions;
        barrier::FreeformDisplayRects displayRects;
        barrier::FreeformDisplayNames displayNames;
        settings.beginGroup("internalConfig");
        barrier::loadFreeformLayoutSettings(
                settings, positions, displayRects, displayNames);
        settings.endGroup();

        ASSERT_EQ(positions.count("client"), 1u);
        EXPECT_EQ(positions["client"], std::make_pair(0, -1080));
        EXPECT_EQ(displayRects["server"], serverDisplays);
        EXPECT_EQ(displayRects["client"], clientDisplays);
        EXPECT_EQ(displayNames["client"], QStringList({"Studio Display"}));
    }
}

TEST(ServerConfigPersistenceTests,
     acceptedTopologyProfileSurvivesImmediateRelaunch)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString settingsPath = directory.filePath("barrier.ini");

    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.beginGroup("internalConfig");
    settings.setValue("numColumns", 5);
    settings.setValue("numRows", 3);
    settings.beginWriteArray("screens", 15);
    settings.setArrayIndex(7);
    Screen("server").saveSettings(settings);
    settings.setArrayIndex(8);
    Screen("client").saveSettings(settings);
    settings.endArray();
    settings.endGroup();
    settings.sync();
    ASSERT_EQ(QSettings::NoError, settings.status());

    ServerConfig liveConfig(&settings, 5, 3, "server", nullptr);
    ServerConfig editedConfig(liveConfig);

    barrier::DisplayTopology topology;
    topology.displays = {
        {"internal-display", {0, 0, 1920, 1080}, 0, true}
    };
    topology = topology.normalized();
    editedConfig.setCurrentTopology(topology);
    editedConfig.setFreeformPosition("server", 0, 0);
    editedConfig.setFreeformPosition("client", 1920, -878);
    editedConfig.setFreeformDisplayRects(
        "server", {QRect(0, 0, 1920, 1080)});
    editedConfig.setFreeformDisplayRects(
        "client", {QRect(0, 0, 1920, 1080)});

    QString error;
    ASSERT_TRUE(editedConfig.saveCurrentTopologyProfile(&error))
        << error.toStdString();
    ASSERT_TRUE(liveConfig.commitAcceptedConfiguration(
        editedConfig, &error)) << error.toStdString();

    // Reopen through a fresh QSettings and ServerConfig while both original
    // objects are still alive. This proves acceptance, not destruction,
    // made the profile durable.
    QSettings relaunchedSettings(settingsPath, QSettings::IniFormat);
    ServerConfig relaunchedConfig(
        &relaunchedSettings, 5, 3, "server", nullptr);
    relaunchedConfig.setCurrentTopology(topology);
    EXPECT_TRUE(relaunchedConfig.isCurrentTopologyKnown());
    EXPECT_NE(relaunchedConfig.topologyProfiles().end(),
              relaunchedConfig.topologyProfiles().find(topology.profileKey()));
}

TEST(ServerConfigPersistenceTests,
     rejectedAcceptedConfigurationDoesNotMutateDurableSettings)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString settingsPath = directory.filePath("barrier.ini");

    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.beginGroup("internalConfig");
    settings.setValue("numColumns", 5);
    settings.setValue("numRows", 3);
    settings.setValue("sentinel", QStringLiteral("original"));
    settings.setValue("clipboardSharing", true);
    settings.beginWriteArray("screens", 15);
    settings.setArrayIndex(7);
    Screen("server").saveSettings(settings);
    settings.setArrayIndex(8);
    Screen("client").saveSettings(settings);
    settings.endArray();
    settings.endGroup();
    settings.sync();
    ASSERT_EQ(QSettings::NoError, settings.status());

    ServerConfig liveConfig(&settings, 5, 3, "server", nullptr);
    TestServerConfig editedConfig(&settings, 5, 3, "server", nullptr);
    editedConfig.setClipboardSharingForTest(false);

    barrier::DisplayTopology topology;
    topology.displays = {
        {"internal-display", {0, 0, 1920, 1080}, 0, true}
    };
    topology = topology.normalized();
    editedConfig.setCurrentTopology(topology);
    editedConfig.setFreeformPosition("server", 0, 0);
    editedConfig.setFreeformPosition("client", 1920, 0);
    editedConfig.setFreeformDisplayRects(
        "server", {QRect(0, 0, 1920, 1080)});
    editedConfig.setFreeformDisplayRects(
        "client", {QRect(0, 0, 1920, 1080)});

    QString error;
    ASSERT_TRUE(editedConfig.saveCurrentTopologyProfile(&error))
        << error.toStdString();
    barrier::TopologyProfiles& corruptedProfiles =
        const_cast<barrier::TopologyProfiles&>(
            editedConfig.topologyProfiles());
    ASSERT_EQ(1u, corruptedProfiles.size());
    const barrier::TopologyProfile validProfile =
        corruptedProfiles.begin()->second;
    corruptedProfiles.clear();
    corruptedProfiles["invalid-profile-key"] = validProfile;

    EXPECT_FALSE(liveConfig.commitAcceptedConfiguration(
        editedConfig, &error));
    EXPECT_FALSE(error.isEmpty());

    QSettings relaunchedSettings(settingsPath, QSettings::IniFormat);
    EXPECT_EQ(QString("original"),
              relaunchedSettings.value("internalConfig/sentinel").toString());
    EXPECT_TRUE(relaunchedSettings.value(
        "internalConfig/clipboardSharing").toBool());
    EXPECT_FALSE(relaunchedSettings.contains("topologyProfiles/payload"));
    EXPECT_FALSE(relaunchedSettings.contains(
        "topologyProfiles.pending/payload"));
}

TEST(ServerConfigPersistenceTests, parsesClientDisplayRectsLogLine)
{
    QString clientName;
    QList<QRect> rects;

    ASSERT_TRUE(barrier::parseClientDisplayRectsLogLine(
            "INFO: client \"portrait-client\" display rects: [0,0 1080x1920]",
            clientName, rects));

    EXPECT_EQ(clientName, QString("portrait-client"));
    ASSERT_EQ(rects.size(), 1);
    EXPECT_EQ(rects[0], QRect(0, 0, 1080, 1920));
}

TEST(ServerConfigPersistenceTests, parsesMultipleDisplayRectsLogLine)
{
    QString clientName;
    QList<QRect> rects;

    ASSERT_TRUE(barrier::parseClientDisplayRectsLogLine(
            "INFO: client \"client\" display rects: [0,0 1920x1080; 1920,-840 1080x1920]",
            clientName, rects));

    EXPECT_EQ(clientName, QString("client"));
    ASSERT_EQ(rects.size(), 2);
    EXPECT_EQ(rects[0], QRect(0, 0, 1920, 1080));
    EXPECT_EQ(rects[1], QRect(1920, -840, 1080, 1920));
}
