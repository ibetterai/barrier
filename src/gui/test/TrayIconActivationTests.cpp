/*  barrier -- mouse and keyboard sharing utility
    Copyright (C) 2021 Povilas Kanapickas <povilas@radix.lt>

    This package is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    found in the file LICENSE that should have accompanied this file.

    This package is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "../src/TrayIconActivation.h"

#include <gtest/gtest.h>

TEST(TrayIconActivationTests, MenuBarTriggerDoesNotToggleWindow)
{
    EXPECT_FALSE(trayActivationTogglesWindow(
        QSystemTrayIcon::Trigger, TrayIconSurface::MenuBar));
}

TEST(TrayIconActivationTests, MenuBarDoubleClickDoesNotToggleWindow)
{
    EXPECT_FALSE(trayActivationTogglesWindow(
        QSystemTrayIcon::DoubleClick, TrayIconSurface::MenuBar));
}

TEST(TrayIconActivationTests, SystemTrayTriggerTogglesWindow)
{
    EXPECT_TRUE(trayActivationTogglesWindow(
        QSystemTrayIcon::Trigger, TrayIconSurface::SystemTray));
}

TEST(TrayIconActivationTests, SystemTrayDoubleClickTogglesWindow)
{
    EXPECT_TRUE(trayActivationTogglesWindow(
        QSystemTrayIcon::DoubleClick, TrayIconSurface::SystemTray));
}

TEST(TrayIconActivationTests, OtherActivationReasonsDoNotToggleEitherSurface)
{
    const QSystemTrayIcon::ActivationReason reasons[] = {
        QSystemTrayIcon::Context,
        QSystemTrayIcon::MiddleClick,
        QSystemTrayIcon::Unknown
    };

    for (QSystemTrayIcon::ActivationReason reason : reasons) {
        EXPECT_FALSE(trayActivationTogglesWindow(
            reason, TrayIconSurface::MenuBar));
        EXPECT_FALSE(trayActivationTogglesWindow(
            reason, TrayIconSurface::SystemTray));
    }
}
