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

#include "ServerConfig.h"
#include "Hotkey.h"
#include "AddClientDialog.h"
#include "FreeformLayoutSettings.h"
#include "barrier/DisplayGeometry.h"

#include <QtCore>
#include <QMessageBox>
#include <QAbstractButton>
#include <QPushButton>
#include <QWidget>
#include <memory>
#include <stdexcept>

static const struct
{
     int x;
     int y;
     const char* name;
} neighbourDirs[] =
{
    {  1,  0, "right" },
    { -1,  0, "left" },
    {  0, -1, "up" },
    {  0,  1, "down" },

};

const int serverDefaultIndex = 7;

namespace {

bool isAcceptedServerConfigKey(const QString& key)
{
    return key.startsWith(QStringLiteral("internalConfig/")) ||
           key == QStringLiteral("topologyProfiles/payload") ||
           key.startsWith(QStringLiteral("topologyProfiles.pending/"));
}

QMap<QString, QVariant> acceptedServerConfigSnapshot(QSettings& settings)
{
    QMap<QString, QVariant> snapshot;
    const QStringList keys = settings.allKeys();
    for (const QString& key : keys) {
        if (isAcceptedServerConfigKey(key)) {
            snapshot.insert(key, settings.value(key));
        }
    }
    return snapshot;
}

std::unique_ptr<QSettings> freshSettingsHandle(const QSettings& source)
{
    if (!source.organizationName().isEmpty() ||
        !source.applicationName().isEmpty()) {
        return std::unique_ptr<QSettings>(new QSettings(
            source.format(), source.scope(), source.organizationName(),
            source.applicationName()));
    }
    return std::unique_ptr<QSettings>(
        new QSettings(source.fileName(), source.format()));
}

bool restoreAcceptedServerConfigSnapshot(
    QSettings& settings, const QMap<QString, QVariant>& snapshot)
{
    settings.remove(QStringLiteral("internalConfig"));
    settings.remove(QStringLiteral("topologyProfiles"));
    settings.remove(QStringLiteral("topologyProfiles.pending"));
    for (QMap<QString, QVariant>::const_iterator saved = snapshot.begin();
         saved != snapshot.end(); ++saved) {
        settings.setValue(saved.key(), saved.value());
    }
    settings.sync();
    return settings.status() == QSettings::NoError;
}

QStringList configuredScreenNames(const std::vector<Screen>& screens)
{
    QStringList names;
    for (const Screen& screen : screens) {
        if (!screen.isNull()) {
            names.append(screen.name());
        }
    }
    return names;
}

} // namespace

ServerConfig::ServerConfig(QSettings* settings, int numColumns, int numRows ,
                QString serverName, QWidget* mainWindow) :
    m_pSettings(settings),
    m_Screens(),
    m_NumColumns(numColumns),
    m_NumRows(numRows),
    m_ServerName(serverName),
    m_IgnoreAutoConfigClient(false),
    m_EnableDragAndDrop(false),
    m_ClipboardSharing(true),
    m_pMainWindow(mainWindow)
{
    Q_ASSERT(m_pSettings);

    loadSettings();
}

ServerConfig::~ServerConfig()
{
    if (m_persistSettings) {
        QString error;
        if (!saveSettings(&error)) {
            qWarning() << "Could not save server configuration:" << error;
        }
    }
}

void ServerConfig::setFreeformPosition(const QString& name, int x, int y)
{
    m_freeformPositions[name] = std::make_pair(x, y);
}

bool ServerConfig::getFreeformPosition(const QString& name, int& x, int& y) const
{
    std::map<QString, std::pair<int,int>>::const_iterator i = m_freeformPositions.find(name);
    if (i == m_freeformPositions.end()) return false;
    x = i->second.first;
    y = i->second.second;
    return true;
}

void ServerConfig::setFreeformDisplayRects(const QString& name, const QList<QRect>& rects)
{
    m_freeformDisplayRects[name] = rects;
}

bool ServerConfig::getFreeformDisplayRects(const QString& name, QList<QRect>& rects) const
{
    std::map<QString, QList<QRect>>::const_iterator i = m_freeformDisplayRects.find(name);
    if (i == m_freeformDisplayRects.end()) return false;
    rects = i->second;
    return true;
}

void ServerConfig::setFreeformDisplayNames(const QString& name, const QStringList& names)
{
    m_freeformDisplayNames[name] = names;
}

bool ServerConfig::getFreeformDisplayNames(const QString& name, QStringList& names) const
{
    std::map<QString, QStringList>::const_iterator i = m_freeformDisplayNames.find(name);
    if (i == m_freeformDisplayNames.end()) return false;
    names = i->second;
    return true;
}

bool ServerConfig::hasFreeformPositions() const
{
    return !m_freeformPositions.empty();
}

void ServerConfig::clearFreeformPositions()
{
    m_freeformPositions.clear();
}

void ServerConfig::setCurrentTopology(const barrier::DisplayTopology& topology)
{
    barrier::DisplayTopology normalized;
    try {
        normalized = topology.normalized();
    }
    catch (const std::invalid_argument&) {
        clearCurrentTopology();
        return;
    }
    if (normalized.empty()) {
        clearCurrentTopology();
        return;
    }

    const barrier::TopologyProfileSelection selection =
        barrier::selectTopologyProfile(
            m_topologyProfiles, normalized, m_ServerName,
            m_legacyFreeformPositions, m_legacyFreeformDisplayRects);
    m_currentTopology = normalized;
    m_hasCurrentTopology = true;
    m_freeformPositions = selection.positions;
    m_freeformDisplayRects = selection.displayRects;
}

void ServerConfig::clearCurrentTopology()
{
    m_currentTopology = barrier::DisplayTopology();
    m_hasCurrentTopology = false;
    m_freeformPositions = m_legacyFreeformPositions;
    m_freeformDisplayRects = m_legacyFreeformDisplayRects;
}

bool ServerConfig::isCurrentTopologyKnown() const
{
    return m_hasCurrentTopology &&
           m_topologyProfiles.count(m_currentTopology.profileKey()) != 0;
}

QList<QRect> ServerConfig::currentServerDisplayRects() const
{
    if (!m_hasCurrentTopology) {
        return QList<QRect>();
    }
    const barrier::TopologyProfileSelection selection =
        barrier::selectTopologyProfile(
            barrier::TopologyProfiles(), m_currentTopology, m_ServerName,
            barrier::FreeformPositions(), barrier::FreeformDisplayRects());
    barrier::FreeformDisplayRects::const_iterator server =
        selection.displayRects.find(m_ServerName);
    return server == selection.displayRects.end()
        ? QList<QRect>() : server->second;
}

bool ServerConfig::saveCurrentTopologyProfile(QString* error)
{
    if (m_topologyProfileLoadResult !=
        barrier::TopologyProfileStoreResult::Ok) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "saved display profiles must be repaired before editing");
        }
        return false;
    }
    if (!m_hasCurrentTopology) {
        if (error != nullptr) {
            *error = QStringLiteral("no current display topology is available");
        }
        return false;
    }
    barrier::TopologyProfile profile;
    profile.topology = m_currentTopology;
    profile.positions = m_freeformPositions;
    profile.displayRects = m_freeformDisplayRects;
    QStringList configuredScreens;
    for (const Screen& screen : screens()) {
        if (!screen.isNull()) {
            configuredScreens.append(screen.name());
        }
    }
    if (!barrier::restrictTopologyProfileToScreens(
            profile, configuredScreens, error)) {
        return false;
    }
    return barrier::putTopologyProfile(m_topologyProfiles, profile, error);
}

bool ServerConfig::commitAcceptedConfiguration(
    ServerConfig& edited, QString* error)
{
    if (edited.m_pSettings != m_pSettings) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "edited server configuration uses a different settings store");
        }
        return false;
    }
    if (!settings().group().isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "server configuration settings group is not at its root");
        }
        return false;
    }

    if (!barrier::reconcileTopologyProfilesToScreens(
            edited.m_topologyProfiles,
            configuredScreenNames(edited.screens()), error)) {
        return false;
    }

    QString saveError;
    QMap<QString, QVariant> snapshot;
    bool saved = false;
    {
        std::unique_ptr<QSettings> transactionSettings =
            freshSettingsHandle(settings());
        transactionSettings->sync();
        if (transactionSettings->status() != QSettings::NoError) {
            if (error != nullptr) {
                *error = QStringLiteral(
                    "could not read current server settings");
            }
            return false;
        }
        snapshot = acceptedServerConfigSnapshot(*transactionSettings);

        ServerConfig transactionConfig(edited);
        transactionConfig.m_pSettings = transactionSettings.get();
        transactionConfig.m_persistSettings = false;
        saved = transactionConfig.saveSettings(&saveError);
    }

    if (!saved) {
        bool restored = false;
        {
            std::unique_ptr<QSettings> recoverySettings =
                freshSettingsHandle(settings());
            restored = restoreAcceptedServerConfigSnapshot(
                *recoverySettings, snapshot);
        }
        if (restored) {
            std::unique_ptr<QSettings> verificationSettings =
                freshSettingsHandle(settings());
            verificationSettings->sync();
            restored =
                verificationSettings->status() == QSettings::NoError &&
                acceptedServerConfigSnapshot(*verificationSettings) == snapshot;
        }
        if (error != nullptr) {
            *error = restored
                ? saveError
                : QStringLiteral("%1; could not restore previous settings")
                      .arg(saveError);
        }
        return false;
    }

    const bool persistSettings = m_persistSettings;
    *this = edited;
    m_persistSettings = persistSettings;
    return true;
}

bool ServerConfig::save(const QString& fileName) const
{
    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    save(file);
    file.close();

    return true;
}

void ServerConfig::save(QFile& file) const
{
    QTextStream outStream(&file);
    outStream << *this;
}


void ServerConfig::init()
{
    switchCorners().clear();
    screens().clear();

    // m_NumSwitchCorners is used as a fixed size array. See Screen::init()
    for (int i = 0; i < static_cast<int>(SwitchCorner::Count); i++) {
        switchCorners() << false;
    }

    // There must always be screen objects for each cell in the screens QList. Unused screens
    // are identified by having an empty name.
    for (int i = 0; i < numColumns() * numRows(); i++)
        addScreen(Screen());
}

bool ServerConfig::saveSettings(QString* error)
{
    if (m_topologyProfileLoadResult !=
        barrier::TopologyProfileStoreResult::Ok) {
        if (error != nullptr) {
            *error = m_topologyProfileError.isEmpty()
                ? QStringLiteral("saved display profiles are unavailable")
                : m_topologyProfileError;
        }
        return false;
    }

    if (!barrier::reconcileTopologyProfilesToScreens(
            m_topologyProfiles, configuredScreenNames(screens()), error)) {
        return false;
    }

    settings().beginGroup("internalConfig");
    settings().remove("");

    settings().setValue("numColumns", numColumns());
    settings().setValue("numRows", numRows());

    barrier::saveFreeformLayoutSettings(
            settings(),
            m_hasCurrentTopology ? m_legacyFreeformPositions
                                 : m_freeformPositions,
            m_hasCurrentTopology ? m_legacyFreeformDisplayRects
                                 : m_freeformDisplayRects,
            m_freeformDisplayNames);

    settings().setValue("hasHeartbeat", hasHeartbeat());
    settings().setValue("heartbeat", heartbeat());
    settings().setValue("relativeMouseMoves", relativeMouseMoves());
    settings().setValue("screenSaverSync", screenSaverSync());
    settings().setValue("win32KeepForeground", win32KeepForeground());
    settings().setValue("hasSwitchDelay", hasSwitchDelay());
    settings().setValue("switchDelay", switchDelay());
    settings().setValue("hasSwitchDoubleTap", hasSwitchDoubleTap());
    settings().setValue("switchDoubleTap", switchDoubleTap());
    settings().setValue("switchCornerSize", switchCornerSize());
    settings().setValue("ignoreAutoConfigClient", ignoreAutoConfigClient());
    settings().setValue("enableDragAndDrop", enableDragAndDrop());
    settings().setValue("clipboardSharing", clipboardSharing());

    writeSettings<bool>(settings(), switchCorners(), "switchCorner");

    settings().beginWriteArray("screens");
    for (int i = 0; i < screens().size(); i++)
    {
        settings().setArrayIndex(i);
        screens()[i].saveSettings(settings());
    }
    settings().endArray();

    settings().beginWriteArray("hotkeys");
    for (int i = 0; i < hotkeys().size(); i++)
    {
        settings().setArrayIndex(i);
        hotkeys()[i].saveSettings(settings());
    }
    settings().endArray();

    settings().endGroup();

    settings().sync();
    if (settings().status() != QSettings::NoError) {
        if (error != nullptr) {
            *error = QStringLiteral("could not save server settings");
        }
        return false;
    }

    QString topologyStoreError;
    const barrier::TopologyProfileStoreResult topologyStoreResult =
        barrier::TopologyProfileStore::save(
            settings(), m_topologyProfiles, &topologyStoreError);
    if (topologyStoreResult != barrier::TopologyProfileStoreResult::Ok) {
        qWarning() << "Could not save display topology profiles:"
                   << topologyStoreError;
        if (error != nullptr) {
            *error = topologyStoreError;
        }
        return false;
    }

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void ServerConfig::loadSettings()
{
    settings().beginGroup("internalConfig");

    setNumColumns(settings().value("numColumns", 5).toInt());
    setNumRows(settings().value("numRows", 3).toInt());

    // we need to know the number of columns and rows before we can set up ourselves
    init();

    haveHeartbeat(settings().value("hasHeartbeat", false).toBool());
    setHeartbeat(settings().value("heartbeat", 5000).toInt());
    setRelativeMouseMoves(settings().value("relativeMouseMoves", false).toBool());
    setScreenSaverSync(settings().value("screenSaverSync", true).toBool());
    setWin32KeepForeground(settings().value("win32KeepForeground", false).toBool());
    haveSwitchDelay(settings().value("hasSwitchDelay", false).toBool());
    setSwitchDelay(settings().value("switchDelay", 250).toInt());
    haveSwitchDoubleTap(settings().value("hasSwitchDoubleTap", false).toBool());
    setSwitchDoubleTap(settings().value("switchDoubleTap", 250).toInt());
    setSwitchCornerSize(settings().value("switchCornerSize").toInt());
    setIgnoreAutoConfigClient(settings().value("ignoreAutoConfigClient").toBool());
    setEnableDragAndDrop(settings().value("enableDragAndDrop", true).toBool());
    setClipboardSharing(settings().value("clipboardSharing", true).toBool());

    readSettings<bool>(settings(), switchCorners(), "switchCorner", false,
                       static_cast<int>(SwitchCorner::Count));

    int numScreens = settings().beginReadArray("screens");
    Q_ASSERT(numScreens <= screens().size());
    for (int i = 0; i < numScreens; i++)
    {
        settings().setArrayIndex(i);
        screens()[i].loadSettings(settings());
    }
    settings().endArray();

    int numHotkeys = settings().beginReadArray("hotkeys");
    for (int i = 0; i < numHotkeys; i++)
    {
        settings().setArrayIndex(i);
        Hotkey h;
        h.loadSettings(settings());
        hotkeys().push_back(h);
    }
    settings().endArray();

    barrier::loadFreeformLayoutSettings(
            settings(), m_freeformPositions, m_freeformDisplayRects,
            m_freeformDisplayNames);
    settings().endGroup();


    m_legacyFreeformPositions = m_freeformPositions;
    m_legacyFreeformDisplayRects = m_freeformDisplayRects;
    m_topologyProfileLoadResult = barrier::TopologyProfileStore::load(
        settings(), m_topologyProfiles, &m_topologyProfileError);
    if (m_topologyProfileLoadResult !=
        barrier::TopologyProfileStoreResult::Ok) {
        m_topologyProfiles.clear();
        qWarning() << "Ignoring malformed display topology profiles:"
                   << m_topologyProfileError;
    }
    else {
        QString reconciliationError;
        if (!barrier::reconcileTopologyProfilesToScreens(
                m_topologyProfiles, configuredScreenNames(screens()),
                &reconciliationError)) {
            m_topologyProfiles.clear();
            qWarning() << "Ignoring display topology profiles for an invalid "
                          "configured screen list:"
                       << reconciliationError;
        }
    }
}

int ServerConfig::adjacentScreenIndex(int idx, int deltaColumn, int deltaRow) const
{
    if (screens()[idx].isNull())
        return -1;

    // if we're at the left or right end of the table, don't find results going further left or right
    if ((deltaColumn > 0 && (idx+1) % numColumns() == 0)
            || (deltaColumn < 0 && idx % numColumns() == 0))
        return -1;

    int arrayPos = idx + deltaColumn + deltaRow * numColumns();

    if (arrayPos >= screens().size() || arrayPos < 0)
        return -1;

    return arrayPos;
}

QTextStream& operator<<(QTextStream& outStream, const ServerConfig& config)
{
    outStream << "section: screens" << endl;

    for (const Screen& s : config.screens()) {
        if (!s.isNull())
            s.writeScreensSection(outStream);
    }

    outStream << "end" << endl << endl;

    outStream << "section: aliases" << endl;

    for (const Screen& s : config.screens()) {
        if (!s.isNull())
            s.writeAliasesSection(outStream);
    }

    outStream << "end" << endl << endl;

    outStream << "section: links" << endl;

    // A drag that is visually snapped on the scaled freeform canvas can still
    // round to a few dozen layout pixels. Treat that as touching so visually
    // adjacent displays still produce links.
    static const int kFreeformAdjacencyTolerancePx = 32;
    if (config.hasFreeformPositions()) {
        // Freeform layout: generate partial-interval links from the shared
        // display geometry primitives.  Individual physical displays decide
        // which edges touch; the emitted intervals are normalized against
        // each screen's full display union, so L-shape / notch adjacencies
        // keep partial edges without per-rectangle fraction math.
        QMap<QString, std::vector<ScreenRect> > displayRects;
        QMap<QString, barrier::ScreenOrigin> origins;
        QStringList screenNames;
        for (int i = 0; i < config.screens().size(); i++) {
            const Screen& s = config.screens()[i];
            if (s.isNull()) continue;
            screenNames << s.name();
            int px = 0, py = 0;
            config.getFreeformPosition(s.name(), px, py);
            origins[s.name()] = barrier::ScreenOrigin{px, py};
            QList<QRect> stored;
            std::vector<ScreenRect> rects;
            if (config.getFreeformDisplayRects(s.name(), stored) && !stored.isEmpty()) {
                for (int r = 0; r < stored.size(); r++) {
                    const QRect& qr = stored[r];
                    rects.push_back(ScreenRect{qr.x(), qr.y(), qr.width(), qr.height()});
                }
            } else {
                rects.push_back(ScreenRect{0, 0, 1920, 1080});
            }
            displayRects[s.name()] = rects;
        }

        // Serialize an interval as config percentages.  qRound can
        // collapse a narrow positive interval to equal endpoints, which
        // the config parser would reject; widen the upper endpoint so
        // the emitted interval stays non-empty.  Returns false when the
        // interval cannot be represented (start already at 100).
        auto intervalText = [](float first, float second, QString& text) {
            int start = qRound(first * 100.0f);
            int end = qRound(second * 100.0f);
            if (end <= start) {
                if (start >= 100) {
                    return false;
                }
                end = start + 1;
            }
            text = QString("%1,%2").arg(start).arg(end);
            return true;
        };

        for (const QString& name : screenNames) {
            outStream << "\t" << name << ":" << endl;
            // each ordered cross-screen pair is derived once per direction;
            // the primitives dedupe identical links within a pair
            for (const QString& oth : screenNames) {
                if (oth == name) continue;
                const std::vector<barrier::DisplayLink> links = barrier::deriveDisplayLinks(
                    name.toStdString(), displayRects.value(name),
                    origins.value(name), oth.toStdString(),
                    displayRects.value(oth), origins.value(oth),
                    kFreeformAdjacencyTolerancePx);
                for (std::vector<barrier::DisplayLink>::const_iterator it = links.begin();
                     it != links.end(); ++it) {
                    QString sourceInterval;
                    QString targetInterval;
                    if (!intervalText(it->sourceInterval.first, it->sourceInterval.second, sourceInterval) ||
                        !intervalText(it->targetInterval.first, it->targetInterval.second, targetInterval)) {
                        // Narrower than the config format can express;
                        // skip rather than emit invalid (100,100) syntax.
                        continue;
                    }
                    const char* dir;
                    switch (it->sourceSide) {
                        case kLeft:   dir = "left";  break;
                        case kRight:  dir = "right"; break;
                        case kTop:    dir = "up";    break;
                        case kBottom: dir = "down";  break;
                        default:      dir = "";      break;
                    }
                    outStream << "\t\t" << dir << "(" << sourceInterval
                              << ") = " << oth << "(" << targetInterval << ")" << endl;
                }
            }
        }
    }
    else {
        // Grid layout links (backward compatible)
        for (int i = 0; i < config.screens().size(); i++)
            if (!config.screens()[i].isNull())
            {
                outStream << "\t" << config.screens()[i].name() << ":" << endl;

                for (unsigned int j = 0; j < sizeof(neighbourDirs) / sizeof(neighbourDirs[0]); j++)
                {
                    int idx = config.adjacentScreenIndex(i, neighbourDirs[j].x, neighbourDirs[j].y);
                    if (idx != -1 && !config.screens()[idx].isNull())
                        outStream << "\t\t" << neighbourDirs[j].name << " = " << config.screens()[idx].name() << endl;
                }
            }
    }

    outStream << "end" << endl << endl;
    barrier::TopologyProfiles topologyProfiles = config.m_topologyProfiles;
    if (!barrier::reconcileTopologyProfilesToScreens(
            topologyProfiles, configuredScreenNames(config.screens()))) {
        topologyProfiles.clear();
    }
    barrier::writeTopologyProfiles(outStream, topologyProfiles);


    outStream << "section: options" << endl;

    if (config.hasHeartbeat())
        outStream << "\t" << "heartbeat = " << config.heartbeat() << endl;

    outStream << "\t" << "relativeMouseMoves = " << (config.relativeMouseMoves() ? "true" : "false") << endl;
    outStream << "\t" << "screenSaverSync = " << (config.screenSaverSync() ? "true" : "false") << endl;
    outStream << "\t" << "win32KeepForeground = " << (config.win32KeepForeground() ? "true" : "false") << endl;
    outStream << "\t" << "clipboardSharing = " << (config.clipboardSharing() ? "true" : "false") << endl;

    if (config.hasSwitchDelay())
        outStream << "\t" << "switchDelay = " << config.switchDelay() << endl;

    if (config.hasSwitchDoubleTap())
        outStream << "\t" << "switchDoubleTap = " << config.switchDoubleTap() << endl;

    outStream << "\t" << "switchCorners = none ";
    for (int i = 0; i < config.switchCorners().size(); i++) {
        auto corner = static_cast<Screen::SwitchCorner>(i);
        if (config.switchCorners()[i]) {
            outStream << "+" << config.switchCornerName(corner) << " ";
        }
    }
    outStream << endl;

    outStream << "\t" << "switchCornerSize = " << config.switchCornerSize() << endl;

    for (const Hotkey& hotkey : config.hotkeys()) {
        outStream << hotkey;
    }

    outStream << "end" << endl << endl;

    return outStream;
}

int ServerConfig::numScreens() const
{
    int rval = 0;

    for (const Screen& s : screens()) {
        if (!s.isNull())
            rval++;
    }

    return rval;
}

int ServerConfig::autoAddScreen(const QString name)
{
    int serverIndex = -1;
    int targetIndex = -1;
    if (!findScreenName(m_ServerName, serverIndex)) {
        if (!fixNoServer(m_ServerName, serverIndex)) {
            return kAutoAddScreenManualServer;
        }
    }
    if (findScreenName(name, targetIndex)) {
        // already exists.
        return kAutoAddScreenIgnore;
    }

    int result = showAddClientDialog(name);

    if (result == kAddClientIgnore) {
        return kAutoAddScreenIgnore;
    }

    if (result == kAddClientOther) {
        addToFirstEmptyGrid(name);
        return kAutoAddScreenManualClient;
    }

    bool success = false;
    int startIndex = serverIndex;
    int offset = 1;
    int dirIndex = 0;

    if (result == kAddClientLeft) {
        offset = -1;
        dirIndex = 1;
    }
    else if (result == kAddClientUp) {
        offset = -5;
        dirIndex = 2;
    }
    else if (result == kAddClientDown) {
        offset = 5;
        dirIndex = 3;
    }


    int idx = adjacentScreenIndex(startIndex, neighbourDirs[dirIndex].x,
                    neighbourDirs[dirIndex].y);
    while (idx != -1) {
        if (screens()[idx].isNull()) {
            m_Screens[idx].setName(name);
            success = true;
            break;
        }

        startIndex += offset;
        idx = adjacentScreenIndex(startIndex, neighbourDirs[dirIndex].x,
                    neighbourDirs[dirIndex].y);
    }

    if (!success) {
        addToFirstEmptyGrid(name);
        return kAutoAddScreenManualClient;
    }

    saveSettings();
    return kAutoAddScreenOk;
}

bool ServerConfig::findScreenName(const QString& name, int& index)
{
    bool found = false;
    for (int i = 0; i < screens().size(); i++) {
        if (!screens()[i].isNull() &&
            screens()[i].name().compare(name) == 0) {
            index = i;
            found = true;
            break;
        }
    }
    return found;
}

bool ServerConfig::fixNoServer(const QString& name, int& index)
{
    bool fixed = false;
    if (screens()[serverDefaultIndex].isNull()) {
        m_Screens[serverDefaultIndex].setName(name);
        index = serverDefaultIndex;
        fixed = true;
    }

    return fixed;
}

int ServerConfig::showAddClientDialog(const QString& clientName)
{
    int result = kAddClientIgnore;

    if (!m_pMainWindow->isActiveWindow()) {
        m_pMainWindow->showNormal();
        m_pMainWindow->activateWindow();
    }

    AddClientDialog addClientDialog(clientName, m_pMainWindow);
    addClientDialog.exec();
    result = addClientDialog.addResult();
    m_IgnoreAutoConfigClient = addClientDialog.ignoreAutoConfigClient();

    return result;
}

void::ServerConfig::addToFirstEmptyGrid(const QString &clientName)
{
    for (int i = 0; i < screens().size(); i++) {
        if (screens()[i].isNull()) {
            m_Screens[i].setName(clientName);
            break;
        }
    }
}
