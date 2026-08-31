/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2008 Volker Lanz (vl@fidra.de)
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

#ifndef BARRIER_GUI_SRC_TRAYICONACTIVATION_H
#define BARRIER_GUI_SRC_TRAYICONACTIVATION_H

#include <QSystemTrayIcon>

enum class TrayIconSurface
{
    MenuBar,
    SystemTray
};

// macOS menu-bar clicks belong to the menu itself. Only explicit Show and
// Show Log actions may raise windows there. Preserve the historical
// Trigger/DoubleClick toggle for desktop system trays on other platforms.
inline bool trayActivationTogglesWindow(QSystemTrayIcon::ActivationReason reason,
                                        TrayIconSurface surface)
{
    if (surface == TrayIconSurface::MenuBar) {
        return false;
    }

    return reason == QSystemTrayIcon::Trigger ||
           reason == QSystemTrayIcon::DoubleClick;
}

inline TrayIconSurface currentTrayIconSurface()
{
#if defined(Q_OS_MAC)
    return TrayIconSurface::MenuBar;
#else
    return TrayIconSurface::SystemTray;
#endif
}

#endif // BARRIER_GUI_SRC_TRAYICONACTIVATION_H
