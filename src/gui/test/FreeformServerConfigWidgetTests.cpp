/*  barrier -- mouse and keyboard sharing utility
    Copyright (C) 2021 Povilas Kanapickas <povilas@radix.lt>

    This package is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    found in the file LICENSE that should have accompanied this file.

    This package is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
*/

#include "../src/FreeformServerConfigWidget.h"
#include <gtest/gtest.h>

TEST(FreeformServerConfigWidgetTests, prefersProductNameForCanvasLabel)
{
    EXPECT_EQ(FreeformServerConfigWidget::canvasDisplayLabel("target-mac", 1, "LU"), "LU");
}

TEST(FreeformServerConfigWidgetTests, usesBarrierScreenFallbackForMissingProductName)
{
    EXPECT_EQ(FreeformServerConfigWidget::canvasDisplayLabel("target-mac", 2, ""), "target-mac #2");
}

TEST(FreeformServerConfigWidgetTests, padsShortDisplayNameListWithEmptyEntries)
{
    const QStringList aligned =
            FreeformServerConfigWidget::alignDisplayNames(QStringList({"LU"}), 2);
    ASSERT_EQ(aligned.size(), 2);
    EXPECT_EQ(aligned[0], "LU");
    EXPECT_TRUE(aligned[1].isEmpty());
}

TEST(FreeformServerConfigWidgetTests, truncatesLongDisplayNameListToDisplayCount)
{
    const QStringList aligned =
            FreeformServerConfigWidget::alignDisplayNames(QStringList({"LU", "VG", "extra"}), 2);
    ASSERT_EQ(aligned.size(), 2);
    EXPECT_EQ(aligned[0], "LU");
    EXPECT_EQ(aligned[1], "VG");
}

TEST(FreeformServerConfigWidgetTests, clientDisplayNamesRoundTripAndClear)
{
    FreeformServerConfigWidget widget;
    QList<QRect> rects;
    rects.append(QRect(0, 0, 1920, 1080));
    rects.append(QRect(1920, -840, 1080, 1920));
    widget.setClientDisplays("client-mac", rects);
    widget.setClientDisplayNames(QStringList({"LG", "VG"}));
    EXPECT_EQ(widget.clientDisplayNames(), QStringList({"LG", "VG"}));

    // A fresh rect set clears stale names; per-display labels then fall
    // back to the Barrier screen name with a 1-based index.
    widget.setClientDisplays("client-mac", QList<QRect>() << QRect(0, 0, 1920, 1080));
    EXPECT_TRUE(widget.clientDisplayNames().isEmpty());
    EXPECT_EQ(FreeformServerConfigWidget::canvasDisplayLabel("client-mac", 1, ""),
              QStringLiteral("client-mac #1"));
}

TEST(FreeformServerConfigWidgetTests, tracksMultipleConnectedClients)
{
    FreeformServerConfigWidget widget;
    widget.setServerDisplays(QList<QRect>() << QRect(0, 0, 1920, 1080));

    widget.setClientDisplays("landscape-client", QList<QRect>() << QRect(0, 0, 1920, 1080));
    widget.setClientPosition("landscape-client", QPoint(0, -1080));

    widget.setClientDisplays("portrait-client", QList<QRect>() << QRect(0, 0, 1080, 1920));
    widget.setClientPosition("portrait-client", QPoint(1920, -1080));

    const QStringList names = widget.clientNames();
    EXPECT_TRUE(names.contains("landscape-client"));
    EXPECT_TRUE(names.contains("portrait-client"));
    EXPECT_EQ(widget.clientPosition("landscape-client"), QPoint(0, -1080));
    EXPECT_EQ(widget.clientPosition("portrait-client"), QPoint(1920, -1080));
    EXPECT_EQ(widget.clientDisplays("portrait-client"),
              QList<QRect>() << QRect(0, 0, 1080, 1920));
}
