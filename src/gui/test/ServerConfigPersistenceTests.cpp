/*  barrier -- mouse and keyboard sharing utility
    Copyright (C) 2026 William Zhu

    This package is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    found in the file LICENSE that should have accompanied this file.
*/

#include "../src/FreeformLayoutSettings.h"

#include <gtest/gtest.h>

#include <QSettings>
#include <QTemporaryDir>

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
