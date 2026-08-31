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

#if !defined(SERVERCONFIG__H)

#define SERVERCONFIG__H

#include <QList>
#include <QStringList>

#include "Screen.h"
#include "BaseConfig.h"
#include "Hotkey.h"

class QTextStream;
class QSettings;
class QString;
class QFile;
class ServerConfigDialog;
class MainWindow;

class ServerConfig : public BaseConfig
{
    friend class ServerConfigDialog;
    friend QTextStream& operator<<(QTextStream& outStream, const ServerConfig& config);

    public:
        ServerConfig(QSettings* settings, int numColumns, int numRows,
            QString serverName, MainWindow* mainWindow);
        ~ServerConfig();

    public:
        const std::vector<Screen>& screens() const { return m_Screens; }
        int numColumns() const { return m_NumColumns; }
        int numRows() const { return m_NumRows; }
        // Freeform accessors
        void setFreeformPosition(const QString& name, int x, int y);
        bool getFreeformPosition(const QString& name, int& x, int& y) const;
        void setFreeformDisplayRects(const QString& name, const QList<QRect>& rects);
        bool getFreeformDisplayRects(const QString& name, QList<QRect>& rects) const;
        // Per-display product names for a screen, ordered identically to
        // the rects stored by setFreeformDisplayRects(). Populated from
        // the daemon's ClientProxy DDNM metadata (MainWindow parses the
        // log line into these); empty entries fall back to
        // "<screen name> #<index>" labels in the canvas.
        void setFreeformDisplayNames(const QString& name, const QStringList& names);
        bool getFreeformDisplayNames(const QString& name, QStringList& names) const;
        bool hasFreeformPositions() const;
        void clearFreeformPositions();
        bool hasHeartbeat() const { return m_HasHeartbeat; }
        int heartbeat() const { return m_Heartbeat; }
        bool relativeMouseMoves() const { return m_RelativeMouseMoves; }
        bool screenSaverSync() const { return m_ScreenSaverSync; }
        bool win32KeepForeground() const { return m_Win32KeepForeground; }
        bool hasSwitchDelay() const { return m_HasSwitchDelay; }
        int switchDelay() const { return m_SwitchDelay; }
        bool hasSwitchDoubleTap() const { return m_HasSwitchDoubleTap; }
        int switchDoubleTap() const { return m_SwitchDoubleTap; }
        bool switchCorner(SwitchCorner c) const { return m_SwitchCorners[static_cast<int>(c)]; }
        int switchCornerSize() const { return m_SwitchCornerSize; }
        const QList<bool>& switchCorners() const { return m_SwitchCorners; }
        const std::vector<Hotkey>& hotkeys() const { return m_Hotkeys; }
        bool ignoreAutoConfigClient() const { return m_IgnoreAutoConfigClient; }
        bool enableDragAndDrop() const { return m_EnableDragAndDrop; }
        bool clipboardSharing() const { return m_ClipboardSharing; }

        void saveSettings();
        void loadSettings();
        bool save(const QString& fileName) const;
        void save(QFile& file) const;
        int numScreens() const;
        int autoAddScreen(const QString name);

    protected:
        QSettings& settings() { return *m_pSettings; }
        std::vector<Screen>& screens() { return m_Screens; }
        void setScreens(const std::vector<Screen>& screens) { m_Screens = screens; }
        void addScreen(const Screen& screen) { m_Screens.push_back(screen); }
        void setNumColumns(int n) { m_NumColumns = n; }
        void setNumRows(int n) { m_NumRows = n; }
        void haveHeartbeat(bool on) { m_HasHeartbeat = on; }
        void setHeartbeat(int val) { m_Heartbeat = val; }
        void setRelativeMouseMoves(bool on) { m_RelativeMouseMoves = on; }
        void setScreenSaverSync(bool on) { m_ScreenSaverSync = on; }
        void setWin32KeepForeground(bool on) { m_Win32KeepForeground = on; }
        void haveSwitchDelay(bool on) { m_HasSwitchDelay = on; }
        void setSwitchDelay(int val) { m_SwitchDelay = val; }
        void haveSwitchDoubleTap(bool on) { m_HasSwitchDoubleTap = on; }
        void setSwitchDoubleTap(int val) { m_SwitchDoubleTap = val; }
        void setSwitchCorner(SwitchCorner c, bool on) { m_SwitchCorners[static_cast<int>(c)] = on; }
        void setSwitchCornerSize(int val) { m_SwitchCornerSize = val; }
        void setIgnoreAutoConfigClient(bool on) { m_IgnoreAutoConfigClient = on; }
        void setEnableDragAndDrop(bool on) { m_EnableDragAndDrop = on; }
        void setClipboardSharing(bool on) { m_ClipboardSharing = on; }
        QList<bool>& switchCorners() { return m_SwitchCorners; }
        std::vector<Hotkey>& hotkeys() { return m_Hotkeys; }

        void init();
        int adjacentScreenIndex(int idx, int deltaColumn, int deltaRow) const;

    private:
        bool findScreenName(const QString& name, int& index);
        bool fixNoServer(const QString& name, int& index);
        int showAddClientDialog(const QString& clientName);
        void addToFirstEmptyGrid(const QString& clientName);

    private:
        QSettings* m_pSettings;
        std::vector<Screen> m_Screens;
        int m_NumColumns;
        int m_NumRows;
        // Freeform layout: when m_freeformPositions is non-empty the grid
        // (m_NumColumns/m_NumRows) is ignored.  Positions are global layout
        // coordinates for each screen's bounding-box origin, and
        // m_freeformDisplayRects are the screen's display rects in
        // screen-local coordinates (main display at 0,0).
        std::map<QString, std::pair<int,int>> m_freeformPositions;
        std::map<QString, QList<QRect>> m_freeformDisplayRects;
        std::map<QString, QStringList> m_freeformDisplayNames;
        bool m_HasHeartbeat;
        int m_Heartbeat;
        bool m_RelativeMouseMoves;
        bool m_ScreenSaverSync;
        bool m_Win32KeepForeground;
        bool m_HasSwitchDelay;
        int m_SwitchDelay;
        bool m_HasSwitchDoubleTap;
        int m_SwitchDoubleTap;
        int m_SwitchCornerSize;
        QList<bool> m_SwitchCorners;
        std::vector<Hotkey> m_Hotkeys;
        QString m_ServerName;
        bool m_IgnoreAutoConfigClient;
        bool m_EnableDragAndDrop;
        bool m_ClipboardSharing;
        MainWindow* m_pMainWindow;
};

QTextStream& operator<<(QTextStream& outStream, const ServerConfig& config);

enum {
    kAutoAddScreenOk,
    kAutoAddScreenManualServer,
    kAutoAddScreenManualClient,
    kAutoAddScreenIgnore
};

#endif
