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

#include <iostream>

#include "MainWindow.h"

#include "AboutDialog.h"
#include "ServerConfigDialog.h"
#include "SettingsDialog.h"
#include "ZeroconfService.h"
#include "DataDownloader.h"
#include "CommandProcess.h"
#include "FingerprintAcceptDialog.h"
#include "FreeformLayoutSettings.h"
#include "QUtility.h"
#include "ProcessorArch.h"
#include "SslCertificate.h"
#include "ShutdownCh.h"
#include "base/String.h"
#include "common/DataDirectories.h"
#include "net/FingerprintDatabase.h"
#include "net/SecureUtils.h"
#include "server/ClientWakeRequest.h"
#include "TrayIconActivation.h"

#include <QtCore>
#include <QtGui>
#include <QtNetwork>
#include <QNetworkAccessManager>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QFileDialog>
#include <QDesktopServices>
#include <QDesktopWidget>
#include <QApplication>
#include <QWindow>

#if defined(Q_OS_MAC)
#include "MacWindowActivation.h"
#include "MacProximityController.h"
#include "ProximitySettingsDialog.h"
#include <ApplicationServices/ApplicationServices.h>
#endif

#if defined(Q_OS_UNIX)
#include <signal.h>
#include <sys/types.h>
#endif

#if defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

static const QString allFilesFilter(QObject::tr("All files (*.*)"));
#if defined(Q_OS_WIN)
static const char barrierConfigName[] = "barrier.sgc";
static const QString barrierConfigFilter(QObject::tr("Barrier Configurations (*.sgc)"));
static QString bonjourBaseUrl = "http://binaries.symless.com/bonjour/";
static const char bonjourFilename32[] = "Bonjour.msi";
static const char bonjourFilename64[] = "Bonjour64.msi";
static const char bonjourTargetFilename[] = "Bonjour.msi";
#else
static const char barrierConfigName[] = "barrier.conf";
static const QString barrierConfigFilter(QObject::tr("Barrier Configurations (*.conf)"));
#endif
static const QString barrierConfigOpenFilter(barrierConfigFilter + ";;" + allFilesFilter);
constexpr int barrierRestartDelayMilliseconds = 1000;
static const QString barrierConfigSaveFilter(barrierConfigFilter);

namespace {

#if defined(Q_OS_MAC)
const char kProximityChildGenerationProperty[] =
    "barrier.proximityChildGeneration";

quint64 proximityChildGeneration(const QProcess* process)
{
    return process == NULL
        ? 0
        : process->property(kProximityChildGenerationProperty).toULongLong();
}
#endif

void showAndActivate(QWidget* window)
{
    window->showNormal();
#if defined(Q_OS_MAC)
    // Qt can show a parentless QDialog without activating the application.
    // Restore foreground eligibility before issuing the Qt focus requests.
    activateCurrentApplication();
#endif
    window->raise();
    window->activateWindow();
    QApplication::setActiveWindow(window);
    if (window->windowHandle() != NULL) {
        window->windowHandle()->requestActivate();
    }
#if defined(Q_OS_MAC)
    // Make the requested native window authoritative after Qt has finished
    // creating and activating it. Otherwise a later Qt focus request can
    // leave the parentless Log dialog behind the main window or another app.
    bringWindowToFront(window);
#endif
}

}

static const char* barrierIconFiles[] =
{
#if defined(Q_OS_MAC)
    ":/res/icons/32x32/barrier-disconnected-mask.png",
    ":/res/icons/32x32/barrier-disconnected-mask.png",
    ":/res/icons/32x32/barrier-connected-mask.png",
    ":/res/icons/32x32/barrier-transfering-mask.png"
#else
    ":/res/icons/16x16/barrier-disconnected.png",
    ":/res/icons/16x16/barrier-disconnected.png",
    ":/res/icons/16x16/barrier-connected.png",
    ":/res/icons/16x16/barrier-transfering.png"
#endif
};

static const char* barrierIconNames[] =
{
    "barrier-disconnected",
    "barrier-disconnected",
    "barrier-connected",
    "barrier-transfering"
};

static const char* barrierLargeIcon = ":/res/icons/256x256/barrier.ico";

MainWindow::MainWindow(QSettings& settings, AppConfig& appConfig) :
    m_Settings(settings),
    m_ProximityConfig(settings),
    m_AppConfig(&appConfig),
    m_pBarrier(NULL),
    m_BarrierState(barrierDisconnected),
    m_ServerConfig(&m_Settings, 5, 3, m_AppConfig->screenName(), this),
    m_pTempConfigFile(NULL),
    m_pTrayIcon(NULL),
    m_pTrayIconMenu(NULL),
    m_AlreadyHidden(false),
    m_pMenuBar(NULL),
    m_pMenuBarrier(NULL),
    m_pMenuHelp(NULL),
    m_pTopologyStatusAction(NULL),
    m_pConfigureTopologyAction(NULL),
    m_pProximitySettingsAction(NULL),
    m_pProximityOverrideAction(NULL),
    m_pProximityStatusAction(NULL),
    m_pZeroconfService(NULL),
    m_pDataDownloader(NULL),
    m_DownloadMessageBox(NULL),
    m_pCancelButton(NULL),
    m_SuppressAutoConfigWarning(false),
    m_BonjourInstall(NULL),
    m_SuppressEmptyServerWarning(false),
    m_ExpectedRunningState(kStopped),
    m_pSslCertificate(NULL),
    m_pLogWindow(new LogWindow(nullptr)),
    m_HasTopologyStatus(false),
    m_ProximityManualOverride(false)
#if defined(Q_OS_MAC)
    , m_pProximityController(NULL)
    , m_ProximityBluetoothPermissionPending(true)
    , m_ProximityBluetoothAuthorized(false)
    , m_ProximityBluetoothAvailable(true)
    , m_ProximityChildRunning(false)
    , m_ProximityProtocolConnected(false)
    , m_ProximityScanning(false)
    , m_ProximityAdvertising(false)
    , m_ClientPresenceAdvertising(false)
    , m_ProximityStoppingChild(false)
    , m_ProximityRuntimeGatingEnabled(false)
    , m_ProximityRestartRequired(false)
#endif
{
    // explicitly unset DeleteOnClose so the window can be show and hidden
    // repeatedly until Barrier is finished
    setAttribute(Qt::WA_DeleteOnClose, false);
    // mark the windows as sort of "dialog" window so that tiling window
    // managers will float it by default (X11)
    setAttribute(Qt::WA_X11NetWmWindowTypeDialog, true);

    setupUi(this);
    m_ProcessRestartTimer.setInterval(barrierRestartDelayMilliseconds);
    m_ProcessRestartTimer.setSingleShot(true);
    connect(
        &m_ProcessRestartTimer, &QTimer::timeout,
        this,
        [this]() {
            if (m_ExpectedRunningState == kStarted &&
                m_ProcessRestartPolicy.takeScheduledRetry()) {
#if defined(Q_OS_MAC)
                if (proximityClientEnabled()) {
                    reconcileProximityClient();
                    return;
                }
#endif
                startBarrierChild();
            }
        });
#if defined(Q_OS_MAC)
    m_pProximityController = new MacProximityController(this);
    m_ProximityClock.start();
    m_ProximityTimer.setInterval(1000);
    connect(&m_ProximityTimer, &QTimer::timeout,
            this, &MainWindow::reconcileProximityClient);
    m_ProximityRestartTimer.setInterval(1000);
    m_ProximityRestartTimer.setSingleShot(true);
    connect(
        &m_ProximityRestartTimer, &QTimer::timeout,
        this,
        [this]() {
            if (m_ProximityRestartPolicy.takeScheduledRetry()) {
                reconcileProximityClient();
            }
        });
    connect(
        m_pProximityController,
        &MacProximityController::bluetoothStateChanged,
        this,
        [this](MacProximityController::BluetoothState state) {
            switch (state) {
            case MacProximityController::BluetoothState::Unknown:
                m_ProximityBluetoothPermissionPending = true;
                m_ProximityBluetoothAuthorized = false;
                m_ProximityBluetoothAvailable = true;
                break;
            case MacProximityController::BluetoothState::Unauthorized:
                m_ProximityBluetoothPermissionPending = false;
                m_ProximityBluetoothAuthorized = false;
                m_ProximityBluetoothAvailable = true;
                break;
            case MacProximityController::BluetoothState::PoweredOn:
                m_ProximityBluetoothPermissionPending = false;
                m_ProximityBluetoothAuthorized = true;
                m_ProximityBluetoothAvailable = true;
                break;
            case MacProximityController::BluetoothState::PoweredOff:
            case MacProximityController::BluetoothState::Failed:
                m_ProximityBluetoothPermissionPending = false;
                m_ProximityBluetoothAuthorized = true;
                m_ProximityBluetoothAvailable = false;
                break;
            }
            reconcileProximityClient();
        });
    connect(
        m_pProximityController,
        &MacProximityController::peripheralObserved,
        this,
        [this](QUuid peripheralId, QString, int rssiDbm) {
            barrier::ProximityPairing pairing;
            if (m_ProximitySignalFilter &&
                m_ProximityConfig.pairing(pairing) &&
                pairing.peripheralId == peripheralId) {
                m_ProximitySignalFilter->addSample(
                    rssiDbm, m_ProximityClock.elapsed());
                reconcileProximityClient();
            }
        });
    connect(
        m_pProximityController,
        &MacProximityController::operationFailed,
        this,
        [this](const QString& operation, const QString& userMessage) {
            if (operation == QStringLiteral("advertising")) {
                appendLogError(userMessage);
                m_ProximityAdvertising = false;
                m_ProximityAdvertisingId.clear();
            }
            else if (operation ==
                     QStringLiteral("client-presence-advertising")) {
                appendLogError(userMessage);
                m_ClientPresenceAdvertising = false;
                m_ClientPresenceAdvertisingId.clear();
            }
        });
#endif
    setWindowIcon(QIcon(barrierLargeIcon));
    createMenuBar();
    loadSettings();
    initConnections();
#if defined(Q_OS_MAC)
    configureProximityController();
#endif

    m_pLabelScreenName->setText(getScreenName());
    m_pLabelIpAddresses->setText(getIPAddresses());

#if defined(Q_OS_WIN)
    // ipc must always be enabled, so that we can disable command when switching to desktop mode.
    connect(&m_IpcClient, SIGNAL(readLogLine(const QString&)), this, SLOT(appendLogRaw(const QString&)));
    connect(&m_IpcClient, SIGNAL(errorMessage(const QString&)), this, SLOT(appendLogError(const QString&)));
    connect(&m_IpcClient, SIGNAL(infoMessage(const QString&)), this, SLOT(appendLogInfo(const QString&)));
    m_IpcClient.connectToHost();
#endif

    m_SuppressAutoConfigWarning = true;
    m_pCheckBoxAutoConfig->setChecked(appConfig.autoConfig());
    m_SuppressAutoConfigWarning = false;

    m_pComboServerList->hide();
    m_pLabelPadlock->hide();
    frame_fingerprint_details->hide();

    updateSSLFingerprint();

    connect(toolbutton_show_fingerprint, &QToolButton::clicked, [this](bool checked)
    {
        m_fingerprint_expanded = !m_fingerprint_expanded;
        if (m_fingerprint_expanded) {
            frame_fingerprint_details->show();
            toolbutton_show_fingerprint->setArrowType(Qt::ArrowType::UpArrow);
        } else {
            frame_fingerprint_details->hide();
            toolbutton_show_fingerprint->setArrowType(Qt::ArrowType::DownArrow);
        }
    });

}

MainWindow::~MainWindow()
{
    if (appConfig().processMode() == Desktop) {
        m_ExpectedRunningState = kStopped;
        m_ProcessRestartPolicy.requestStop();
        m_ProcessRestartTimer.stop();
        stopDesktop();
    }

    saveSettings();

    delete m_pZeroconfService;
    delete m_DownloadMessageBox;
    delete m_BonjourInstall;
    delete m_pSslCertificate;

    // LogWindow is created as a sibling of the MainWindow rather than a child
    // so that the main window can be hidden without hiding the log. because of
    // this it does not get properly cleaned up by the QObject system. also by
    // the time this destructor is called the event loop will no longer be able
    // to clean up the LogWindow so ->deleteLater() will not work
    delete m_pLogWindow;
}

void MainWindow::open()
{
    createTrayIcon();

    if (appConfig().getAutoHide()) {
        hide();
    } else {
        showNormal();
    }

    if (!appConfig().autoConfigPrompted()) {
        promptAutoConfig();
    }

    // Loading persisted widget state does not necessarily emit a toggle.
    // Refresh explicitly so proximity clients browse on every launch.
    updateZeroconfService();

    // only start if user has previously started. this stops the gui from
    // auto hiding before the user has configured barrier (which of course
    // confuses first time users, who think barrier has crashed).
    if (appConfig().startedBefore() && appConfig().getAutoStart()) {
        m_SuppressEmptyServerWarning = true;
        startBarrier();
        m_SuppressEmptyServerWarning = false;
    }
}

void MainWindow::setStatus(const QString &status)
{
    m_pStatusLabel->setText(status);
}

void MainWindow::resetTopologyStatusSession()
{
    m_TopologyStatus = barrier::TopologyStatus();
    m_HasTopologyStatus = false;
    m_NotifiedUnknownTopologyKeys.clear();
    m_ClickableUnknownTopologyKey.clear();
    m_ServerConfig.clearCurrentTopology();
    if (m_pTopologyStatusAction != NULL) {
        m_pTopologyStatusAction->setText(
            tr("Display arrangement: unavailable"));
    }
    if (m_pConfigureTopologyAction != NULL) {
        m_pConfigureTopologyAction->setVisible(false);
    }
    if (m_pZeroconfService != NULL &&
        barrier_type() == BarrierType::Server) {
        m_pZeroconfService->setServerDisplayReady(false);
    }
}

void MainWindow::createTrayIcon()
{
    m_pTrayIconMenu = new QMenu(this);

    m_pTrayIconMenu->addAction(m_pActionStartBarrier);
    m_pTrayIconMenu->addAction(m_pActionStopBarrier);
    m_pTrayIconMenu->addAction(m_pActionShowLog);
    m_pTrayIconMenu->addAction(m_pTopologyStatusAction);
    m_pTrayIconMenu->addAction(m_pConfigureTopologyAction);
#if defined(Q_OS_MAC)
    m_pTrayIconMenu->addAction(m_pProximityStatusAction);
    m_pTrayIconMenu->addAction(m_pProximitySettingsAction);
    m_pTrayIconMenu->addAction(m_pProximityOverrideAction);
#endif
    m_pTrayIconMenu->addSeparator();

    m_pTrayIconMenu->addAction(m_pActionMinimize);
    m_pTrayIconMenu->addAction(m_pActionRestore);
    m_pTrayIconMenu->addSeparator();
    m_pTrayIconMenu->addAction(m_pActionQuit);

    m_pTrayIcon = new QSystemTrayIcon(this);
    m_pTrayIcon->setContextMenu(m_pTrayIconMenu);
    m_pTrayIcon->setToolTip("Barrier");

    connect(m_pTrayIcon, SIGNAL(activated(QSystemTrayIcon::ActivationReason)),
            this, SLOT(trayActivated(QSystemTrayIcon::ActivationReason)));
    connect(m_pTrayIcon, &QSystemTrayIcon::messageClicked, this, [this]() {
        if (!m_ClickableUnknownTopologyKey.isEmpty() &&
            m_HasTopologyStatus &&
            m_TopologyStatus.state ==
                barrier::TopologyStatusState::StableUnknown &&
            m_TopologyStatus.key == m_ClickableUnknownTopologyKey) {
            m_ClickableUnknownTopologyKey.clear();
            showAndActivate(this);
            showConfigureServer(
                tr("Configure this display arrangement before switching "
                   "between screens."));
        }
    });

    setIcon(barrierDisconnected);

    m_pTrayIcon->show();
    if (m_HasTopologyStatus &&
        m_TopologyStatus.state ==
            barrier::TopologyStatusState::StableUnknown &&
        !m_NotifiedUnknownTopologyKeys.contains(m_TopologyStatus.key)) {
        m_pTrayIcon->showMessage(
            tr("New display arrangement"),
            tr("Configure this display arrangement before switching between screens."));
        m_ClickableUnknownTopologyKey = m_TopologyStatus.key;
        m_NotifiedUnknownTopologyKeys.insert(m_TopologyStatus.key);
    }
}

void MainWindow::retranslateMenuBar()
{
    m_pMenuBarrier->setTitle(tr("&Barrier"));
    m_pMenuHelp->setTitle(tr("&Help"));
#if defined(Q_OS_MAC)
    if (m_pProximitySettingsAction != NULL) {
        m_pProximitySettingsAction->setText(tr("Proximity Settings…"));
    }
    if (m_pProximityOverrideAction != NULL) {
        m_pProximityOverrideAction->setText(
            m_ProximityManualOverride ? tr("Resume Proximity Gating")
                                      : tr("Connect Anyway"));
    }
#endif
}

void MainWindow::createMenuBar()
{
    m_pMenuBar = new QMenuBar(this);
    m_pMenuBarrier = new QMenu("", m_pMenuBar);
    m_pMenuHelp = new QMenu("", m_pMenuBar);
    retranslateMenuBar();

    m_pMenuBar->addAction(m_pMenuBarrier->menuAction());
    m_pMenuBar->addAction(m_pMenuHelp->menuAction());

    m_pMenuBarrier->addAction(m_pActionShowLog);
    m_pMenuBarrier->addAction(m_pActionSettings);
    m_pTopologyStatusAction =
        new QAction(tr("Display arrangement: unavailable"), this);
    m_pTopologyStatusAction->setEnabled(false);
    m_pConfigureTopologyAction =
        new QAction(tr("Configure this display arrangement"), this);
    m_pConfigureTopologyAction->setVisible(false);
    connect(m_pConfigureTopologyAction, &QAction::triggered,
            this, [this]() { showConfigureServer(); });
    m_pMenuBarrier->addAction(m_pTopologyStatusAction);
    m_pMenuBarrier->addAction(m_pConfigureTopologyAction);
#if defined(Q_OS_MAC)
    m_pProximityStatusAction =
        new QAction(tr("Proximity: Off"), this);
    m_pProximityStatusAction->setEnabled(false);
    m_pMenuBarrier->addAction(m_pProximityStatusAction);
    m_pProximitySettingsAction =
        new QAction(tr("Proximity Settings…"), this);
    connect(m_pProximitySettingsAction, &QAction::triggered,
            this, &MainWindow::showProximitySettings);
    m_pProximityOverrideAction =
        new QAction(tr("Connect Anyway"), this);
    m_pProximityOverrideAction->setVisible(false);
    connect(m_pProximityOverrideAction, &QAction::triggered,
            this, &MainWindow::toggleProximityOverride);
    m_pMenuBarrier->addAction(m_pProximitySettingsAction);
    m_pMenuBarrier->addAction(m_pProximityOverrideAction);
#endif
    m_pMenuBarrier->addAction(m_pActionMinimize);
    m_pMenuBarrier->addSeparator();
    m_pMenuBarrier->addAction(m_pActionSave);
    m_pMenuBarrier->addSeparator();
    m_pMenuBarrier->addAction(m_pActionQuit);
    m_pMenuHelp->addAction(m_pActionAbout);

    setMenuBar(m_pMenuBar);
}

void MainWindow::showProximitySettings()
{
#if defined(Q_OS_MAC)
    const bool gatingWasEnabled = m_ProximityConfig.clientGatingEnabled();
    barrier::ProximityPairing pairingBefore;
    const bool wasPaired = m_ProximityConfig.pairing(pairingBefore);
    ProximitySettingsDialog dialog(
        this, barrier_type() == BarrierType::Server,
        m_ProximityConfig, *m_pProximityController, m_ZeroconfRecords);
    bool changed = false;
    connect(&dialog, &ProximitySettingsDialog::configurationChanged,
            this, [&changed]() { changed = true; });
    connect(this, &MainWindow::zeroconfRecordsChanged,
            &dialog, &ProximitySettingsDialog::bonjourServersChanged);
    dialog.exec();
    if (changed) {
        barrier::ProximityPairing pairingAfter;
        const bool isPaired = m_ProximityConfig.pairing(pairingAfter);
        const bool samePairing = wasPaired && isPaired &&
            pairingBefore.proximityId == pairingAfter.proximityId &&
            pairingBefore.peripheralId == pairingAfter.peripheralId;
        const bool preserveClientRuntime = samePairing &&
            gatingWasEnabled == m_ProximityConfig.clientGatingEnabled();
        const bool boundedThresholdChange = preserveClientRuntime &&
            gatingWasEnabled &&
            pairingBefore.thresholdPolicy != pairingAfter.thresholdPolicy &&
            m_ProximitySignalFilter;
        updateZeroconfService();
        m_ProximityManualOverride = false;
        if (preserveClientRuntime) {
            if (boundedThresholdChange) {
                m_ProximitySignalFilter->reconfigure(
                    pairingAfter.thresholdPolicy,
                    m_ProximityClock.elapsed());
            }
        }
        else {
            m_ProximitySignalFilter.reset();
            m_ProximityPolicy.reset();
            m_ProximityRestartPolicy.updateEligibility(false);
            m_ProximityRestartTimer.stop();
        }
        configureProximityController();
    }
#endif
}

void MainWindow::toggleProximityOverride()
{
    m_ProximityManualOverride = !m_ProximityManualOverride;
    if (m_pProximityOverrideAction != NULL) {
        m_pProximityOverrideAction->setText(
            m_ProximityManualOverride ? tr("Resume Proximity Gating")
                                      : tr("Connect Anyway"));
    }
#if defined(Q_OS_MAC)
    reconcileProximityClient();
#endif
}

#if defined(Q_OS_MAC)
bool MainWindow::proximityClientEnabled() const
{
    return barrier_type() == BarrierType::Client &&
           m_ProximityConfig.clientGatingEnabled();
}

QString MainWindow::pairedProximityEndpoint() const
{
    barrier::ProximityPairing pairing;
    if (!m_ProximityConfig.pairing(pairing)) {
        return QString();
    }
    for (const ZeroconfRecord& record : m_ZeroconfRecords) {
        if (record.matchesProximityServer(pairing.proximityId) &&
            record.isDisplayReady() && record.hasResolvedService()) {
            return record.barrierEndpoint(m_AppConfig->port());
        }
    }
    return QString();
}

const ZeroconfRecord* MainWindow::pairedProximityServer() const
{
    barrier::ProximityPairing pairing;
    if (!m_ProximityConfig.pairing(pairing)) {
        return nullptr;
    }
    for (const ZeroconfRecord& record : m_ZeroconfRecords) {
        if (record.matchesProximityServer(pairing.proximityId) &&
            record.hasResolvedService()) {
            return &record;
        }
    }
    return nullptr;
}

barrier::ProximityInputs MainWindow::currentProximityInputs() const
{
    const ZeroconfRecord* server = pairedProximityServer();
    const qint64 nowMs = m_ProximityClock.elapsed();
    barrier::ProximityInputs inputs;
    inputs.enabled = proximityClientEnabled();
    inputs.userWantsBarrier = m_ExpectedRunningState == kStarted;
    inputs.bluetoothPermissionPending =
        m_ProximityBluetoothPermissionPending;
    inputs.bluetoothAuthorized = m_ProximityBluetoothAuthorized;
    inputs.bluetoothAvailable = m_ProximityBluetoothAvailable;
    inputs.pairedPeerNear = m_ProximitySignalFilter &&
        m_ProximitySignalFilter->isNear(nowMs);
    inputs.pairedPeerReconfigurationGrace = m_ProximitySignalFilter &&
        m_ProximitySignalFilter->isReconfigurationGrace(nowMs);
    inputs.pairedPeerDepartureGrace = m_ProximitySignalFilter &&
        m_ProximitySignalFilter->isDepartureGrace(nowMs);
    inputs.pairedPeerDepartureWarning = m_ProximitySignalFilter &&
        m_ProximitySignalFilter->isDepartureWarning(nowMs);
    inputs.pairedBonjourPresent = server != nullptr;
    inputs.serverDisplayReady = !pairedProximityEndpoint().isEmpty();
    inputs.manualOverride = m_ProximityManualOverride;
    inputs.childRunning = isBarrierChildRunning();
    inputs.protocolConnected = m_ProximityProtocolConnected;
    inputs.monotonicMs = nowMs;
    return inputs;
}

void MainWindow::configureProximityController()
{
    const bool shouldAdvertise = barrier::shouldAdvertiseProximityServer(
        barrier_type() == BarrierType::Server,
        m_ProximityConfig.serverAdvertiserEnabled(),
        m_ExpectedRunningState == kStarted);
    if (shouldAdvertise) {
        QString error;
        const QString proximityId =
            m_ProximityConfig.serverProximityId(&error);
        if (proximityId.isEmpty()) {
            appendLogError(
                tr("Unable to enable proximity advertising: %1").arg(error));
            if (m_ProximityAdvertising) {
                m_pProximityController->stopAdvertising();
                m_ProximityAdvertising = false;
                m_ProximityAdvertisingId.clear();
            }
        }
        else if (!m_ProximityAdvertising ||
                 m_ProximityAdvertisingId != proximityId) {
            m_pProximityController->startAdvertising(proximityId);
            m_ProximityAdvertising = true;
            m_ProximityAdvertisingId = proximityId;
        }
    }
    else {
        // stopAdvertising() is intentionally idempotent. The controller may
        // still have an asynchronous advertising request after reporting a
        // failure even when the GUI's m_ProximityAdvertising flag is false.
        m_pProximityController->stopAdvertising();
        m_ProximityAdvertising = false;
        m_ProximityAdvertisingId.clear();
    }

    barrier::ProximityPairing presencePairing;
    const bool shouldShareClientPresence =
        barrier_type() == BarrierType::Client &&
        m_ExpectedRunningState == kStarted &&
        m_ProximityConfig.pairing(presencePairing) &&
        presencePairing.signalSharingEnabled &&
        !presencePairing.clientRoutingId.isEmpty();
    if (shouldShareClientPresence) {
        if (!m_ClientPresenceAdvertising ||
            m_ClientPresenceAdvertisingId !=
                presencePairing.clientRoutingId) {
            // Mark the request before calling into CoreBluetooth so a
            // synchronous validation failure can clear it through
            // operationFailed without being overwritten on return.
            m_ClientPresenceAdvertising = true;
            m_ClientPresenceAdvertisingId =
                presencePairing.clientRoutingId;
            m_pProximityController->startClientPresenceAdvertising(
                presencePairing.clientRoutingId);
        }
    }
    else {
        m_pProximityController->stopClientPresenceAdvertising();
        m_ClientPresenceAdvertising = false;
        m_ClientPresenceAdvertisingId.clear();
    }

    const bool wasGating = m_ProximityRuntimeGatingEnabled;
    const bool shouldGate = proximityClientEnabled();
    if (shouldGate) {
        barrier::ProximityPairing pairing;
        if (!m_ProximitySignalFilter &&
            m_ProximityConfig.pairing(pairing)) {
            m_ProximitySignalFilter.reset(
                new barrier::ProximitySignalFilter(
                    pairing.thresholdPolicy));
        }
        if (!m_ProximityScanning) {
            m_pProximityController->startScanning();
            m_ProximityScanning = true;
        }
        if (!m_ProximityTimer.isActive()) {
            m_ProximityTimer.start();
        }
    }
    else {
        if (m_ProximityScanning) {
            m_pProximityController->stopScanning();
            m_ProximityScanning = false;
        }
        m_ProximityTimer.stop();
        m_ProximitySignalFilter.reset();
        m_ProximityManualOverride = false;
    }

    if (wasGating != shouldGate) {
        m_ProximityRestartRequired = true;
    }
    m_ProximityRuntimeGatingEnabled = shouldGate;
    reconcileProximityClient();
}

void MainWindow::reconcileProximityClient()
{
    if (m_ProximityStoppingChild) {
        return;
    }
    const bool enabled = proximityClientEnabled();
    if (!enabled) {
        m_ProximityPolicy.reset();
    }
    barrier::ProximityDecision decision = enabled
        ? m_ProximityPolicy.evaluate(currentProximityInputs())
        : barrier::ProximityDecision{
              barrier::ProximityPolicyState::Disabled, false, false};
    m_ProximityRestartPolicy.updateEligibility(
        enabled && decision.canStartChild);
    if (!decision.canStartChild && m_ProximityRestartTimer.isActive()) {
        m_ProximityRestartTimer.stop();
    }
    const QString desiredEndpoint = decision.canStartChild
        ? clientEndpointSelection().endpoint
        : QString();
    if (enabled && decision.canStartChild &&
        isBarrierChildRunning() &&
        desiredEndpoint != m_ProximityRunningEndpoint) {
        m_ProximityRestartRequired = true;
    }

    if (m_ProximityRestartRequired) {
        if (isBarrierChildRunning()) {
            stopBarrierChild(true);
            QTimer::singleShot(
                0, this, &MainWindow::reconcileProximityClient);
            updateProximityStatus(decision.state);
            return;
        }
        m_ProximityRestartRequired = false;
        if (!enabled && m_ExpectedRunningState == kStarted) {
            startBarrierChild();
            updateProximityStatus(
                barrier::ProximityPolicyState::Disabled);
            return;
        }
    }

    if (!enabled) {
        updateProximityStatus(barrier::ProximityPolicyState::Disabled);
        return;
    }

    if (decision.shouldRunChild && decision.canStartChild &&
        !isBarrierChildRunning() &&
        m_ProximityRestartPolicy.automaticLaunchAllowed()) {
        startBarrierChild();
    }
    else if (!decision.shouldRunChild && isBarrierChildRunning()) {
        stopBarrierChild(true);
    }
    if (m_ExpectedRunningState == kStarted &&
        !isBarrierChildRunning()) {
        setBarrierState(barrierConnecting);
    }
    else if (m_ExpectedRunningState == kStopped) {
        setBarrierState(barrierDisconnected);
    }
    decision = m_ProximityPolicy.evaluate(currentProximityInputs());
    updateProximityStatus(
        decision.canStartChild && m_ProximityRestartPolicy.suppressed()
            ? barrier::ProximityPolicyState::RetrySuppressed
            : decision.state);
}

void MainWindow::updateProximityStatus(
    barrier::ProximityPolicyState state)
{
    QString text;
    switch (state) {
    case barrier::ProximityPolicyState::Disabled:
        text = tr("Proximity: Off");
        break;
    case barrier::ProximityPolicyState::WaitingForPermission:
        text = tr("Proximity: Waiting for Bluetooth permission");
        break;
    case barrier::ProximityPolicyState::SensorUnavailable:
        text = tr("Proximity paused: Bluetooth denied or unavailable");
        break;
    case barrier::ProximityPolicyState::WaitingForPeer:
        text = tr("Proximity: Waiting for paired server");
        break;
    case barrier::ProximityPolicyState::WaitingForNetwork:
        text = tr("Proximity: Waiting for network-ready paired server");
        break;
    case barrier::ProximityPolicyState::Starting:
        text = tr("Proximity: Starting client");
        break;
    case barrier::ProximityPolicyState::Connected:
        text = tr("Proximity: Connected");
        break;
    case barrier::ProximityPolicyState::DepartureGrace:
        text = tr("Proximity: Server signal lost; disconnecting soon");
        break;
    case barrier::ProximityPolicyState::ReconfigurationGrace:
        text = tr("Proximity: Applying new signal range");
        break;
    case barrier::ProximityPolicyState::ManualOverride:
        text = m_ProximityProtocolConnected
            ? tr("Proximity override: Connected")
            : (isBarrierChildRunning()
                ? tr("Proximity override: Starting client")
                : tr("Proximity override: Waiting for network-ready server"));
        break;
    case barrier::ProximityPolicyState::RetrySuppressed:
        text = tr("Proximity paused: Client exited repeatedly");
        break;
    }

    if (m_pProximityStatusAction != NULL) {
        m_pProximityStatusAction->setText(text);
        m_pProximityStatusAction->setVisible(
            barrier_type() == BarrierType::Client);
    }
    if (m_pProximityOverrideAction != NULL) {
        const bool overrideAvailable =
            proximityClientEnabled() &&
            (state == barrier::ProximityPolicyState::SensorUnavailable ||
             m_ProximityManualOverride);
        m_pProximityOverrideAction->setVisible(overrideAvailable);
        m_pProximityOverrideAction->setText(
            m_ProximityManualOverride ? tr("Resume Proximity Gating")
                                      : tr("Connect Anyway"));
    }
    if (proximityClientEnabled()) {
        setStatus(text);
    }
}
#endif

void MainWindow::loadSettings()
{
    // the next two must come BEFORE loading groupServerChecked and groupClientChecked or
    // disabling and/or enabling the right widgets won't automatically work
    m_pRadioExternalConfig->setChecked(settings().value("useExternalConfig", false).toBool());
    m_pRadioInternalConfig->setChecked(settings().value("useInternalConfig", true).toBool());

    m_pGroupServer->setChecked(settings().value("groupServerChecked", false).toBool());
    m_pLineEditConfigFile->setText(settings().value("configFile", QDir::homePath() + "/" + barrierConfigName).toString());
    m_pGroupClient->setChecked(settings().value("groupClientChecked", true).toBool());
    m_pLineEditHostname->setText(settings().value("serverHostname").toString());
}

void MainWindow::initConnections()
{
    connect(m_pActionMinimize, SIGNAL(triggered()), this, SLOT(hide()));
    connect(m_pActionRestore, &QAction::triggered, this, [this]() {
        showAndActivate(this);
    });
    connect(m_pActionStartBarrier, SIGNAL(triggered()), this, SLOT(startBarrier()));
    connect(m_pActionStopBarrier, SIGNAL(triggered()), this, SLOT(stopBarrier()));
    connect(m_pActionShowLog, SIGNAL(triggered()), this, SLOT(showLogWindow()));
    connect(m_pActionQuit, SIGNAL(triggered()), qApp, SLOT(quit()));
}

void MainWindow::saveSettings()
{
    // program settings
    settings().setValue("groupServerChecked", m_pGroupServer->isChecked());
    settings().setValue("useExternalConfig", m_pRadioExternalConfig->isChecked());
    settings().setValue("configFile", m_pLineEditConfigFile->text());
    settings().setValue("useInternalConfig", m_pRadioInternalConfig->isChecked());
    settings().setValue("groupClientChecked", m_pGroupClient->isChecked());
    settings().setValue("serverHostname", m_pLineEditHostname->text());

    settings().sync();
}

void MainWindow::setIcon(qBarrierState state)
{
    if (m_pTrayIcon) {
        QIcon icon = QIcon::fromTheme(barrierIconNames[state], QIcon(barrierIconFiles[state]));
#if defined(Q_OS_MAC)
        icon.setIsMask(true);
#endif
        m_pTrayIcon->setIcon(icon);
    }
}

void MainWindow::trayActivated(QSystemTrayIcon::ActivationReason reason)
{
    if (!trayActivationTogglesWindow(reason, currentTrayIconSurface()))
    {
        // macOS status-item clicks belong to the menu. Context, MiddleClick
        // and Unknown keep their existing behavior on other platforms.
        return;
    }

    if (isVisible())
    {
        hide();
    }
    else
    {
        showAndActivate(this);
    }
}

void MainWindow::logOutput()
{
    if (m_pBarrier != NULL) {
        processLogChunk(
            QString::fromLocal8Bit(m_pBarrier->readAllStandardOutput()),
            m_StdoutLogBuffer);
    }
}

void MainWindow::logError()
{
    if (m_pBarrier != NULL) {
        processLogChunk(
            QString::fromLocal8Bit(m_pBarrier->readAllStandardError()),
            m_StderrLogBuffer);
    }
}

void MainWindow::appendLogInfo(const QString& text)
{
    if (appConfig().logLevel() >= 3) {
        m_pLogWindow->appendInfo(text);
    }
}

void MainWindow::appendLogDebug(const QString& text) {
    if (appConfig().logLevel() >= 4) {
        m_pLogWindow->appendDebug(text);
    }
}

void MainWindow::appendLogError(const QString& text)
{
    m_pLogWindow->appendError(text);
}

void MainWindow::processLogChunk(const QString& text, QString& buffer)
{
    buffer.append(text);
    while (true) {
        const int carriageReturn = buffer.indexOf(QLatin1Char('\r'));
        const int lineFeed = buffer.indexOf(QLatin1Char('\n'));
        int separator = carriageReturn;
        if (separator < 0 || (lineFeed >= 0 && lineFeed < separator)) {
            separator = lineFeed;
        }
        if (separator < 0) {
            break;
        }

        const QString line = buffer.left(separator);
        int consumed = separator + 1;
        if (buffer.at(separator) == QLatin1Char('\r') &&
            consumed < buffer.size() &&
            buffer.at(consumed) == QLatin1Char('\n')) {
            ++consumed;
        }
        buffer.remove(0, consumed);
        if (!line.isEmpty()) {
            updateFromLogLine(line);
        }
    }
}

void MainWindow::flushPendingLogChunks()
{
    if (!m_StdoutLogBuffer.isEmpty()) {
        updateFromLogLine(m_StdoutLogBuffer);
        m_StdoutLogBuffer.clear();
    }
    if (!m_StderrLogBuffer.isEmpty()) {
        updateFromLogLine(m_StderrLogBuffer);
        m_StderrLogBuffer.clear();
    }
}

void MainWindow::appendLogRaw(const QString& text)
{
    for (QString line : text.split(QRegExp("\r|\n|\r\n"))) {
        if (!line.isEmpty()) {
            updateFromLogLine(line);
        }
    }
}

void MainWindow::updateFromLogLine(const QString &line)
{
    const QByteArray utf8Line = line.toUtf8();
    std::string wakeTarget;
    const barrier::ClientWakeRequestParseResult wakeResult =
        barrier::parseClientWakeRequest(
            std::string(utf8Line.constData(),
                        static_cast<std::size_t>(utf8Line.size())),
            wakeTarget);
    if (wakeResult == barrier::ClientWakeRequestParseResult::Valid &&
        m_pZeroconfService != NULL &&
        barrier_type() == BarrierType::Server) {
        m_pZeroconfService->wakeClient(QString::fromUtf8(
            wakeTarget.data(), static_cast<int>(wakeTarget.size())));
    }

    QRegExp connectedClient(
        QStringLiteral(".*client \"([^\"]+)\" has connected$"));
    if (m_pZeroconfService != NULL &&
        barrier_type() == BarrierType::Server &&
        connectedClient.exactMatch(line)) {
        m_pZeroconfService->clientConnected(connectedClient.cap(1));
    }

    barrier::TopologyStatus parsed;
    const barrier::TopologyStatusParseResult result =
        barrier::TopologyStatusParser::parse(line, parsed);
    if (result == barrier::TopologyStatusParseResult::Valid) {
        m_TopologyStatus = parsed;
        m_HasTopologyStatus = true;
        if (parsed.topology.empty()) {
            m_ServerConfig.clearCurrentTopology();
        }
        else {
            m_ServerConfig.setCurrentTopology(parsed.topology);
        }

        QString statusText;
        bool configureVisible = false;
        switch (parsed.state) {
        case barrier::TopologyStatusState::Reconfiguring:
            statusText = tr("Display arrangement: changing");
            break;
        case barrier::TopologyStatusState::StableKnown:
            statusText = tr("Display arrangement: configured");
            break;
        case barrier::TopologyStatusState::StableUnknown:
            statusText = tr("Display arrangement: configuration required");
            configureVisible = true;
            if (m_pTrayIcon != NULL &&
                !m_NotifiedUnknownTopologyKeys.contains(parsed.key)) {
                m_pTrayIcon->showMessage(
                    tr("New display arrangement"),
                    tr("Configure this display arrangement before switching between screens."));
                m_ClickableUnknownTopologyKey = parsed.key;
                m_NotifiedUnknownTopologyKeys.insert(parsed.key);
            }
            break;
        case barrier::TopologyStatusState::NoDisplayGrace:
            statusText = tr("Display arrangement: waiting for displays");
            break;
        case barrier::TopologyStatusState::Unavailable:
            statusText = tr("Display arrangement: unavailable");
            break;
        }
        if (parsed.state != barrier::TopologyStatusState::StableUnknown ||
            parsed.key != m_ClickableUnknownTopologyKey) {
            m_ClickableUnknownTopologyKey.clear();
        }
        if (m_pTopologyStatusAction != NULL) {
            m_pTopologyStatusAction->setText(statusText);
        }
        if (m_pConfigureTopologyAction != NULL) {
            m_pConfigureTopologyAction->setVisible(configureVisible);
        }
        if (m_pZeroconfService != NULL &&
            barrier_type() == BarrierType::Server) {
            m_pZeroconfService->setServerDisplayReady(
                parsed.displayReady());
        }
    }

    m_pLogWindow->appendRaw(line);
    // TODO: this code makes Andrew cry
    checkConnected(line);
    checkFingerprint(line);
    checkClientDisplayRects(line);
    checkClientDisplayNames(line);
}

void MainWindow::checkClientDisplayRects(const QString& line)
{
    // The daemon logs this when DDIS metadata arrives:
    //   client "client-mac" display rects: [0,0 1080x1920]
    // Store it before opening the freeform dialog so a portrait client
    // renders from the reported geometry instead of the default landscape
    // fallback.
    QString clientName;
    QList<QRect> rects;
    if (barrier::parseClientDisplayRectsLogLine(line, clientName, rects)) {
        serverConfig().setFreeformDisplayRects(clientName, rects);
    }
}

void MainWindow::checkClientDisplayNames(const QString& line)
{
    // The daemon is the only process with the live ClientProxy; log lines
    // are the established daemon -> GUI metadata channel (see
    // checkFingerprint()). Parses the line emitted by
    // ClientProxy1_0::recvDisplayNames() when DDNM metadata arrives:
    //   client "client-mac" display names: ["LG", "", "VG"]
    // The ordered names (empty entries allowed) are stored per client
    // screen so ServerConfigDialog can label the canvas displays.
    QRegExp namesRegex(".*client \"([^\"]+)\" display names: \\[(.*)\\]");
    if (!namesRegex.exactMatch(line)) {
        return;
    }
    const QString clientName = namesRegex.cap(1);
    QStringList names;
    const QString content = namesRegex.cap(2);
    if (!content.isEmpty()) {
        for (const QString& part : content.split(QStringLiteral("\", \""))) {
            QString name = part;
            if (name.startsWith(QLatin1Char('"')) && name.endsWith(QLatin1Char('"'))) {
                name = name.mid(1, name.size() - 2);
            }
            names.append(name);
        }
    }
    serverConfig().setFreeformDisplayNames(clientName, names);
}

void MainWindow::checkConnected(const QString& line)
{
    // TODO: implement ipc connection state messages to replace this hack.
    const bool clientProtocolConnected = line.contains("connected to server");
    if (line.contains("started server") ||
        clientProtocolConnected ||
        line.contains("server status: active"))
    {
        setBarrierState(barrierConnected);
#if defined(Q_OS_MAC)
        if (clientProtocolConnected && proximityClientEnabled()) {
            m_ProximityProtocolConnected = true;
            reconcileProximityClient();
        }
#endif

        if (!appConfig().startedBefore() && isVisible()) {
                QMessageBox::information(
                    this, "Barrier",
                    tr("Barrier is now connected. You can close the "
                    "config window and Barrier will remain connected in "
                    "the background."));

            appConfig().setStartedBefore(true);
            appConfig().saveSettings();
        }
    }
}

void MainWindow::checkFingerprint(const QString& line)
{
    QRegExp fingerprintRegex(".*peer fingerprint \\(SHA1\\): ([A-F0-9:]+) \\(SHA256\\): ([A-F0-9:]+)");
    if (!fingerprintRegex.exactMatch(line)) {
        return;
    }

    barrier::FingerprintData fingerprint_sha1 = {
        barrier::fingerprint_type_to_string(barrier::FingerprintType::SHA1),
        barrier::string::from_hex(fingerprintRegex.cap(1).toStdString())
    };

    barrier::FingerprintData fingerprint_sha256 = {
        barrier::fingerprint_type_to_string(barrier::FingerprintType::SHA256),
        barrier::string::from_hex(fingerprintRegex.cap(2).toStdString())
    };

    bool is_client = barrier_type() == BarrierType::Client;

    auto db_path = is_client
            ? barrier::DataDirectories::trusted_servers_ssl_fingerprints_path()
            : barrier::DataDirectories::trusted_clients_ssl_fingerprints_path();

    auto db_dir = db_path.parent_path();
    if (!barrier::fs::exists(db_dir)) {
        barrier::fs::create_directories(db_dir);
    }

    // We compare only SHA256 fingerprints, but show both SHA1 and SHA256 so that the users can
    // still verify fingerprints on old Barrier servers. This way the only time when we are exposed
    // to SHA1 vulnerabilities is when the user is reconnecting again.
    barrier::FingerprintDatabase db;
    db.read(db_path);
    if (db.is_trusted(fingerprint_sha256)) {
        return;
    }

    static bool messageBoxAlreadyShown = false;

    if (!messageBoxAlreadyShown) {
        if (is_client) {
            stopBarrier();
        }

        messageBoxAlreadyShown = true;
        FingerprintAcceptDialog dialog{this, barrier_type(), fingerprint_sha1, fingerprint_sha256};
        if (dialog.exec() == QDialog::Accepted) {
            // restart core process after trusting fingerprint.
            db.add_trusted(fingerprint_sha256);
            db.write(db_path);
            if (is_client) {
                startBarrier();
            }
        }

        messageBoxAlreadyShown = false;
    }
}

void MainWindow::restartBarrier()
{
    stopBarrier();
    startBarrier();
}

void MainWindow::proofreadInfo()
{
    int oldState = m_BarrierState;
    m_BarrierState = barrierDisconnected;
    setBarrierState((qBarrierState)oldState);
}

void MainWindow::startBarrier()
{
    m_ExpectedRunningState = kStarted;
    m_ProcessRestartPolicy.requestStart();
    m_ProcessRestartTimer.stop();
#if defined(Q_OS_MAC)
    barrier::ProximityPairing presencePairing;
    if (m_ProximityConfig.pairing(presencePairing) &&
        presencePairing.signalSharingEnabled) {
        updateZeroconfService();
    }
    configureProximityController();
    if (proximityClientEnabled()) {
        reconcileProximityClient();
        return;
    }
#endif
    startBarrierChild();
}
void MainWindow::startBarrierChild()
{
#if defined(Q_OS_MAC)
    if (m_ProximityStoppingChild) {
        return;
    }
#endif
    if (isBarrierChildRunning()) {
        return;
    }
    if (barrierProcess() != NULL &&
        barrierProcess()->state() == QProcess::NotRunning) {
#if defined(Q_OS_MAC)
        m_ProximityProcessExitTracker.forgetProcess(
            proximityChildGeneration(barrierProcess()));
#endif
        delete barrierProcess();
        setBarrierProcess(NULL);
    }
    resetTopologyStatusSession();
    m_StdoutLogBuffer.clear();
    m_StderrLogBuffer.clear();
    const bool desktopMode = appConfig().processMode() == Desktop;
    const bool serviceMode = appConfig().processMode() == Service;

    appendLogDebug("starting process");
#if defined(Q_OS_MAC)
    m_ProximityProtocolConnected = false;
#endif
    setBarrierState(barrierConnecting);

    QString app;
    QStringList args;

    args << "-f" << "--no-tray" << "--debug" << appConfig().logLevelText();


    args << "--name" << getScreenName();

    if (desktopMode)
    {
        QProcess* process = new QProcess(this);
#if defined(Q_OS_MAC)
        const quint64 generation =
            m_ProximityProcessExitTracker.beginProcess();
        process->setProperty(
            kProximityChildGenerationProperty,
            QVariant::fromValue<qulonglong>(generation));
#endif
        setBarrierProcess(process);
    }
    else
    {
        // tell client/server to talk to daemon through ipc.
        args << "--ipc";

#if defined(Q_OS_WIN)
        // tell the client/server to shut down when a ms windows desk
        // is switched; this is because we may need to elevate or not
        // based on which desk the user is in (login always needs
        // elevation, where as default desk does not).
        // Note that this is only enabled when barrier is set to elevate
        // 'as needed' (e.g. on a UAC dialog popup) in order to prevent
        // unnecessary restarts when barrier was started elevated or
        // when it is not allowed to elevate. In these cases restarting
        // the server is fruitless.
        if (appConfig().elevateMode() == ElevateAsNeeded) {
                args << "--stop-on-desk-switch";
        }
#endif
    }

#ifndef Q_OS_LINUX

    if (m_ServerConfig.enableDragAndDrop()) {
        args << "--enable-drag-drop";
    }

#endif

    if (!m_AppConfig->getCryptoEnabled()) {
        args << "--disable-crypto";
    }

#if defined(Q_OS_WIN)
    // on windows, the profile directory changes depending on the user that
    // launched the process (e.g. when launched with elevation). setting the
    // profile dir on launch ensures it uses the same profile dir is used
    // no matter how its relaunched.
    args << "--profile-dir" << QString::fromStdString("\"" + barrier::DataDirectories::profile().u8string() + "\"");
#endif

    if ((barrier_type() == BarrierType::Client && !clientArgs(args, app))
        || (barrier_type() == BarrierType::Server && !serverArgs(args, app)))
    {
        stopBarrier();
        return;
    }

    if (desktopMode)
    {
        connect(barrierProcess(), SIGNAL(finished(int, QProcess::ExitStatus)), this, SLOT(barrierFinished(int, QProcess::ExitStatus)));
        connect(barrierProcess(), SIGNAL(readyReadStandardOutput()), this, SLOT(logOutput()));
        connect(barrierProcess(), SIGNAL(readyReadStandardError()), this, SLOT(logError()));
    }

    m_pLogWindow->startNewInstance();

    appendLogInfo("starting " + QString(barrier_type() == BarrierType::Server ? "server" : "client"));

    qDebug() << args;

    appendLogDebug(QString("command: %1 %2").arg(app, args.join(" ")));

    appendLogInfo("config file: " + configFilename());
    appendLogInfo("log level: " + appConfig().logLevelText());

    if (appConfig().logToFile())
        appendLogInfo("log file: " + appConfig().logFilename());

    if (desktopMode)
    {
        barrierProcess()->start(app, args);
        if (!barrierProcess()->waitForStarted())
        {
            enterStoppedIntent();
            setBarrierState(barrierDisconnected);
            show();
            QMessageBox::warning(this, tr("Program can not be started"), QString(tr("The executable<br><br>%1<br><br>could not be successfully started, although it does exist. Please check if you have sufficient permissions to run this program.").arg(app)));
            return;
        }
#if defined(Q_OS_MAC)
        m_ProximityChildRunning = true;
#endif
    }

    if (serviceMode)
    {
        QString command(app + " " + args.join(" "));
        m_IpcClient.sendCommand(command, appConfig().elevateMode());
#if defined(Q_OS_MAC)
        m_ProximityChildRunning = true;
#endif
    }
#if defined(Q_OS_MAC)
    m_ProximityRunningEndpoint = proximityClientEnabled()
        ? clientEndpointSelection().endpoint
        : QString();
#endif
}

barrier::ClientEndpointSelection MainWindow::clientEndpointSelection() const
{
    const bool autoConfigEnabled = m_pCheckBoxAutoConfig->isChecked();
    QString discoveredEndpoint;
    bool proximityGatingEnabled = false;
#if defined(Q_OS_MAC)
    proximityGatingEnabled = proximityClientEnabled();
    if (proximityGatingEnabled) {
        discoveredEndpoint = pairedProximityEndpoint();
    }
#endif
    if (!proximityGatingEnabled && autoConfigEnabled &&
        m_pComboServerList->count() != 0) {
        const ZeroconfRecord record =
            m_pComboServerList->currentData(Qt::UserRole + 3)
                .value<ZeroconfRecord>();
        discoveredEndpoint = record.barrierEndpoint(m_AppConfig->port());
    }

    return barrier::selectClientEndpoint(
        proximityGatingEnabled, autoConfigEnabled,
        m_pLineEditHostname->text(), discoveredEndpoint,
        m_AppConfig->port());
}

bool MainWindow::clientArgs(QStringList& args, QString& app)
{
    app = appPath(appConfig().barriercName());

    if (!QFile::exists(app))
    {
        show();
        QMessageBox::warning(this, tr("Barrier client not found"),
                             tr("The executable for the barrier client does not exist."));
        return false;
    }

#if defined(Q_OS_WIN)
    // wrap in quotes so a malicious user can't start \Program.exe as admin.
    app = QString("\"%1\"").arg(app);
#endif

    if (appConfig().logToFile())
    {
        appConfig().persistLogDir();
        args << "--log" << appConfig().logFilenameCmd();
    }
    const barrier::ClientEndpointSelection selection =
        clientEndpointSelection();
    if (selection.status ==
        barrier::ClientEndpointStatus::WaitingForProximity) {
        return false;
    }
    if (selection.status ==
        barrier::ClientEndpointStatus::MissingServerAddress) {
        show();
        if (!m_SuppressEmptyServerWarning) {
            QMessageBox::warning(
                this, tr("Hostname is empty"),
                tr("Please fill in a hostname for the barrier client to connect to."));
        }
        return false;
    }
#if defined(Q_OS_MAC)
    if (proximityClientEnabled()) {
        args << "--no-restart" << selection.endpoint;
        return true;
    }
#endif
    args << selection.endpoint;
    return true;
}

QString MainWindow::configFilename()
{
    QString filename;
    if (m_pRadioInternalConfig->isChecked())
    {
        // TODO: no need to use a temporary file, since we need it to
        // be permanent (since it'll be used for Windows services, etc).
        m_pTempConfigFile = new QTemporaryFile();
        if (!m_pTempConfigFile->open())
        {
            QMessageBox::critical(this, tr("Cannot write configuration file"), tr("The temporary configuration file required to start barrier can not be written."));
            return "";
        }

        serverConfig().save(*m_pTempConfigFile);
        filename = m_pTempConfigFile->fileName();

        m_pTempConfigFile->close();
    }
    else
    {
        if (!QFile::exists(m_pLineEditConfigFile->text()))
        {
            if (QMessageBox::warning(this, tr("Configuration filename invalid"),
                tr("You have not filled in a valid configuration file for the barrier server. "
                        "Do you want to browse for the configuration file now?"), QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes
                    || !on_m_pButtonBrowseConfigFile_clicked())
                return "";
        }

        filename = m_pLineEditConfigFile->text();
    }
    return filename;
}

BarrierType MainWindow::barrier_type() const
{
    return m_pGroupClient->isChecked() ? BarrierType::Client : BarrierType::Server;
}

QString MainWindow::address()
{
    QString address = appConfig().networkInterface();
    if (!address.isEmpty())
        address = "[" + address + "]";
    return address + ":" + QString::number(appConfig().port());
}

QString MainWindow::appPath(const QString& name)
{
    return appConfig().barrierProgramDir() + name;
}

bool MainWindow::serverArgs(QStringList& args, QString& app)
{
    app = appPath(appConfig().barriersName());

    if (!QFile::exists(app))
    {
        QMessageBox::warning(this, tr("Barrier server not found"),
                             tr("The executable for the barrier server does not exist."));
        return false;
    }

#if defined(Q_OS_WIN)
    // wrap in quotes so a malicious user can't start \Program.exe as admin.
    app = QString("\"%1\"").arg(app);
#endif

    if (appConfig().logToFile())
    {
        appConfig().persistLogDir();

        args << "--log" << appConfig().logFilenameCmd();
    }

    if (!appConfig().getRequireClientCertificate()) {
        args << "--disable-client-cert-checking";
    }

    QString configFilename = this->configFilename();
#if defined(Q_OS_WIN)
    // wrap in quotes in case username contains spaces.
    configFilename = QString("\"%1\"").arg(configFilename);
#endif
    args << "-c" << configFilename << "--address" << address();

    return true;
}

void MainWindow::stopBarrier()
{
    enterStoppedIntent();
#if defined(Q_OS_MAC)
    if (proximityClientEnabled()) {
        return;
    }
#endif
    stopBarrierChild();
}

void MainWindow::enterStoppedIntent()
{
    m_ExpectedRunningState = kStopped;
    m_ProcessRestartPolicy.requestStop();
    m_ProcessRestartTimer.stop();
#if defined(Q_OS_MAC)
    barrier::ProximityPairing presencePairing;
    if (m_ProximityConfig.pairing(presencePairing) &&
        presencePairing.signalSharingEnabled) {
        updateZeroconfService();
    }
    m_ProximityManualOverride = false;
    m_ProximityProtocolConnected = false;
    m_ProximityRestartTimer.stop();
    m_ProximityRestartPolicy.reset();
    if (m_pProximityOverrideAction != NULL) {
        m_pProximityOverrideAction->setText(tr("Connect Anyway"));
    }
    configureProximityController();
#endif
}

void MainWindow::stopBarrierChild(bool proximityPolicyStop)
{
    appendLogDebug("stopping process");
    resetTopologyStatusSession();
#if defined(Q_OS_MAC)
    if (proximityPolicyStop &&
        appConfig().processMode() == Desktop &&
        barrierProcess() != NULL &&
        barrierProcess()->state() != QProcess::NotRunning) {
        m_ProximityProcessExitTracker.expectPolicyStop(
            proximityChildGeneration(barrierProcess()));
    }
    m_ProximityStoppingChild = true;
#else
    Q_UNUSED(proximityPolicyStop);
#endif

    if (appConfig().processMode() == Service)
    {
        stopService();
    }
    else if (appConfig().processMode() == Desktop)
    {
        stopDesktop();
    }
#if defined(Q_OS_MAC)
    m_ProximityStoppingChild = false;
    m_ProximityChildRunning = false;
    m_ProximityProtocolConnected = false;
    m_ProximityRunningEndpoint.clear();
#endif

    setBarrierState(barrierDisconnected);

    // HACK: deleting the object deletes the physical file, which is
    // bad, since it could be in use by the Windows service!
#if !defined(Q_OS_WIN)
    delete m_pTempConfigFile;
#endif
    m_pTempConfigFile = NULL;

    // reset so that new connects cause auto-hide.
    m_AlreadyHidden = false;
}

bool MainWindow::isBarrierChildRunning() const
{
#if defined(Q_OS_MAC)
    return m_ProximityChildRunning;
#else
    return m_pBarrier != NULL &&
           m_pBarrier->state() != QProcess::NotRunning;
#endif
}

void MainWindow::stopService()
{
    // send empty command to stop service from launching anything.
    m_IpcClient.sendCommand("", appConfig().elevateMode());
}

void MainWindow::stopDesktop()
{
    QMutexLocker locker(&m_StopDesktopMutex);
    if (!barrierProcess()) {
        return;
    }
#if defined(Q_OS_MAC)
    const quint64 generation = proximityChildGeneration(barrierProcess());
#endif

    appendLogInfo("stopping barrier desktop process");

    if (barrierProcess()->isOpen()) {
        // try to shutdown child gracefully
        barrierProcess()->write(&ShutdownCh, 1);
        barrierProcess()->waitForFinished(5000);
    }
    processLogChunk(
        QString::fromLocal8Bit(barrierProcess()->readAllStandardOutput()),
        m_StdoutLogBuffer);
    processLogChunk(
        QString::fromLocal8Bit(barrierProcess()->readAllStandardError()),
        m_StderrLogBuffer);
    flushPendingLogChunks();
    barrierProcess()->close();

    delete barrierProcess();
    setBarrierProcess(NULL);
#if defined(Q_OS_MAC)
    m_ProximityProcessExitTracker.forgetProcess(generation);
#endif
}

void MainWindow::barrierFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    logOutput();
    logError();
    flushPendingLogChunks();

#if defined(Q_OS_MAC)
    const barrier::ProximityProcessExitClassification exitClassification =
        m_ProximityProcessExitTracker.classifyAndConsume(
            proximityChildGeneration(qobject_cast<QProcess*>(sender())),
            exitCode, exitStatus == QProcess::CrashExit, SIGKILL);
    const bool stoppedByReconciliation =
        exitClassification.expectedPolicyStop;
    const bool expectedForcedStop =
        exitClassification.logDisposition ==
        barrier::ProximityProcessExitLogDisposition::ExpectedPolicyStop;
#else
    const bool expectedForcedStop = false;
#endif

    if (expectedForcedStop) {
        appendLogInfo(QString("process was force-stopped by proximity policy"));
    }
    else if (exitCode == 0) {
        appendLogInfo(QString("process exited normally"));
    }
    else {
        appendLogError(QString("process exited with error code: %1").arg(exitCode));
    }
    resetTopologyStatusSession();
#if defined(Q_OS_MAC)
    m_ProximityChildRunning = false;
    m_ProximityProtocolConnected = false;
    m_ProximityRunningEndpoint.clear();
    if (stoppedByReconciliation) {
        return;
    }
    if (exitStatus == QProcess::NormalExit && exitCode == kExitConfig) {
        enterStoppedIntent();
        setBarrierState(barrierDisconnected);
        setStatus(tr("Configuration error. Repair the Barrier screen layout, then click Start."));
        appendLogError(tr("Barrier stopped because its configuration could not be read. "
                          "Open Configure Server, repair the screen layout, then click Start."));
        return;
    }
    if (proximityClientEnabled()) {
        barrier::ProximityDecision decision =
            m_ProximityPolicy.evaluate(currentProximityInputs());
        m_ProximityRestartPolicy.updateEligibility(
            decision.canStartChild);
        switch (m_ProximityRestartPolicy.childExitedUnexpectedly()) {
        case barrier::ProximityChildExitAction::RetryAfterDelay:
            appendLogInfo(
                QString("proximity client exited; retrying once in 1 second"));
            setBarrierState(barrierConnecting);
            updateProximityStatus(barrier::ProximityPolicyState::Starting);
            m_ProximityRestartTimer.start();
            break;
        case barrier::ProximityChildExitAction::Suppress:
            appendLogError(
                QString("proximity client exited again; suppressing retries "
                        "until the proximity gates or user intent reset"));
            setBarrierState(barrierConnecting);
            updateProximityStatus(
                barrier::ProximityPolicyState::RetrySuppressed);
            break;
        case barrier::ProximityChildExitAction::Ignore:
            reconcileProximityClient();
            break;
        }
        return;
    }
#endif

    switch (m_ProcessRestartPolicy.childExited(
        exitCode, exitStatus == QProcess::NormalExit)) {
    case barrier::ProcessExitAction::RetryAfterDelay:
        m_ProcessRestartTimer.start();
        appendLogInfo(QString("detected process not running, auto restarting"));
        break;
    case barrier::ProcessExitAction::StopForConfigurationError:
        enterStoppedIntent();
        setBarrierState(barrierDisconnected);
        setStatus(tr("Configuration error. Repair the Barrier screen layout, then click Start."));
        appendLogError(tr("Barrier stopped because its configuration could not be read. "
                          "Open Configure Server, repair the screen layout, then click Start."));
        break;
    case barrier::ProcessExitAction::Ignore:
        setBarrierState(barrierDisconnected);
        break;
    }
}

void MainWindow::setBarrierState(qBarrierState state)
{
    if (barrierState() == state)
        return;

    if (state == barrierConnected || state == barrierConnecting)
    {
        disconnect (m_pButtonToggleStart, SIGNAL(clicked()), m_pActionStartBarrier, SLOT(trigger()));
        connect (m_pButtonToggleStart, SIGNAL(clicked()), m_pActionStopBarrier, SLOT(trigger()));
        m_pButtonToggleStart->setText(tr("&Stop"));
        m_pButtonReload->setEnabled(true);
    }
    else if (state == barrierDisconnected)
    {
        disconnect (m_pButtonToggleStart, SIGNAL(clicked()), m_pActionStopBarrier, SLOT(trigger()));
        connect (m_pButtonToggleStart, SIGNAL(clicked()), m_pActionStartBarrier, SLOT(trigger()));
        m_pButtonToggleStart->setText(tr("&Start"));
        m_pButtonReload->setEnabled(false);
    }

    const bool running = state != barrierDisconnected;
    m_pActionStartBarrier->setEnabled(!running);
    m_pActionStopBarrier->setEnabled(running);

    switch (state)
    {
    case barrierConnected: {
        if (m_AppConfig->getCryptoEnabled()) {
            m_pLabelPadlock->show();
        }
        else {
            m_pLabelPadlock->hide();
        }

        setStatus(tr("Barrier is running."));

        break;
    }
    case barrierConnecting:
        m_pLabelPadlock->hide();
        setStatus(tr("Barrier is starting."));
        break;
    case barrierDisconnected:
        m_pLabelPadlock->hide();
        setStatus(tr("Barrier is not running."));
        break;
    case barrierTransfering:
        break;
    }

    setIcon(state);

    m_BarrierState = state;
}

void MainWindow::setVisible(bool visible)
{
    QMainWindow::setVisible(visible);
    m_pActionMinimize->setEnabled(visible);
    m_pActionRestore->setEnabled(!visible);

#if __MAC_OS_X_VERSION_MIN_REQUIRED >= 1070 // lion
    // dock hide only supported on lion :(
    ProcessSerialNumber psn = { 0, kCurrentProcess };
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    GetCurrentProcess(&psn);
#pragma GCC diagnostic pop
    if (visible)
        TransformProcessType(&psn, kProcessTransformToForegroundApplication);
    else
        TransformProcessType(&psn, kProcessTransformToBackgroundApplication);
#endif
}

QString MainWindow::getIPAddresses()
{
    QList<QHostAddress> addresses = QNetworkInterface::allAddresses();

    bool hinted = false;
    QString result;
    for (int i = 0; i < addresses.size(); i++) {
        if (addresses[i].protocol() == QAbstractSocket::IPv4Protocol &&
            addresses[i] != QHostAddress(QHostAddress::LocalHost)) {

            QString address = addresses[i].toString();
            QString format = "%1, ";

            // usually 192.168.x.x is a useful ip for the user, so indicate
            // this by making it bold.
            if (!hinted && address.startsWith("192.168")) {
                hinted = true;
                format = "<b>%1</b>, ";
            }

            result += format.arg(address);
        }
    }

    if (result == "") {
        return tr("Unknown");
    }

    // remove trailing comma.
    result.chop(2);

    return result;
}

QString MainWindow::getScreenName()
{
    if (appConfig().screenName() == "") {
        return QHostInfo::localHostName();
    }
    else {
        return appConfig().screenName();
    }
}

void MainWindow::changeEvent(QEvent* event)
{
    if (event != 0)
    {
        switch (event->type())
        {
        case QEvent::LanguageChange:
        {
            retranslateUi(this);
            retranslateMenuBar();

            proofreadInfo();
#if defined(Q_OS_MAC)
            reconcileProximityClient();
#endif

            break;
        }
        case QEvent::WindowStateChange:
        {
            windowStateChanged();
            break;
        }
        default:
        {
            break;
        }
        }
    }
    // all that do not return are allowing the event to propagate
    QMainWindow::changeEvent(event);
}

bool MainWindow::event(QEvent* event)
{
    if (event->type() == QEvent::LayoutRequest) {
        setFixedSize(sizeHint());
    }
    return QMainWindow::event(event);
}

void MainWindow::updateZeroconfService()
{
    QMutexLocker locker(&m_UpdateZeroconfMutex);

    if (isBonjourRunning()) {
        if (!m_AppConfig->wizardShouldRun()) {
            if (m_pZeroconfService) {
                delete m_pZeroconfService;
                m_pZeroconfService = NULL;
            }

            bool proximityDiscoveryRequired = false;
            bool clientSignalSharingRequired = false;
#if defined(Q_OS_MAC)
            proximityDiscoveryRequired = proximityClientEnabled();
            barrier::ProximityPairing pairing;
            clientSignalSharingRequired =
                barrier_type() == BarrierType::Client &&
                m_ProximityConfig.pairing(pairing) &&
                pairing.signalSharingEnabled;
#endif
            const ZeroconfService::Requirements requirements =
                ZeroconfService::requirements(
                    m_AppConfig->autoConfig(),
                    barrier_type() == BarrierType::Server,
                    proximityDiscoveryRequired,
                    clientSignalSharingRequired);
            if (requirements.active) {
                m_pZeroconfService = new ZeroconfService(
                    this, requirements.publishClientService);
            }
        }
    }
}

void MainWindow::serverDetected(const QList<ZeroconfRecord>& records)
{
    m_ZeroconfRecords = records;
    emit zeroconfRecordsChanged();
    QList<ZeroconfRecord> visibleRecords;
    if (barrier_type() == BarrierType::Client &&
        m_ProximityConfig.clientGatingEnabled()) {
        barrier::ProximityPairing pairing;
        if (m_ProximityConfig.pairing(pairing)) {
            for (const ZeroconfRecord& record : records) {
                if (record.matchesProximityServer(pairing.proximityId)) {
                    visibleRecords.append(record);
                }
            }
        }
    }
    else {
        visibleRecords = records;
    }

    const QString previousKey =
        m_pComboServerList->currentData(Qt::UserRole + 2).toString();
    const ZeroconfRecord previousRecord =
        m_pComboServerList->currentData(Qt::UserRole + 3)
            .value<ZeroconfRecord>();
    const QString previousEndpoint =
        previousRecord.barrierEndpoint(m_AppConfig->port());

    QSignalBlocker blocker(m_pComboServerList);
    m_pComboServerList->clear();
    int selectedIndex = -1;
    for (const ZeroconfRecord& record : visibleRecords) {
        if (!record.hasResolvedService()) {
            continue;
        }
        const QString key = record.serviceName + QChar(0x1f) +
                            record.registeredType + QChar(0x1f) +
                            record.replyDomain;
        const QString label = record.serviceName.isEmpty()
                                  ? record.hostName
                                  : record.serviceName;
        const int index = m_pComboServerList->count();
        m_pComboServerList->addItem(label, record.hostName);
        m_pComboServerList->setItemData(index, key, Qt::UserRole + 2);
        m_pComboServerList->setItemData(
            index, QVariant::fromValue(record), Qt::UserRole + 3);
        if (key == previousKey) {
            selectedIndex = index;
        }
    }
    if (selectedIndex >= 0) {
        m_pComboServerList->setCurrentIndex(selectedIndex);
    }

    m_pComboServerList->setVisible(m_pComboServerList->count() > 1);
    const ZeroconfRecord currentRecord =
        m_pComboServerList->currentData(Qt::UserRole + 3)
            .value<ZeroconfRecord>();
    const QString currentEndpoint =
        currentRecord.barrierEndpoint(m_AppConfig->port());
#if defined(Q_OS_MAC)
    if (proximityClientEnabled()) {
        reconcileProximityClient();
        return;
    }
#endif
    if (!currentEndpoint.isEmpty() && currentEndpoint != previousEndpoint &&
        m_pCheckBoxAutoConfig->isChecked()) {
        restartBarrier();
    }
}

void MainWindow::updateSSLFingerprint()
{
    if (m_AppConfig->getCryptoEnabled() && m_pSslCertificate == nullptr) {
        m_pSslCertificate = new SslCertificate(this);
        connect(m_pSslCertificate, &SslCertificate::info, [&](QString info)
        {
            appendLogInfo(info);
        });
        m_pSslCertificate->generateCertificate();
    }

    toolbutton_show_fingerprint->setEnabled(false);
    m_pLabelLocalFingerprint->setText("Disabled");

    if (!m_AppConfig->getCryptoEnabled()) {
        return;
    }

    auto local_path = barrier::DataDirectories::local_ssl_fingerprints_path();
    if (!barrier::fs::exists(local_path)) {
        return;
    }

    barrier::FingerprintDatabase db;
    db.read(local_path);
    if (db.fingerprints().size() != 2) {
        return;
    }

    for (const auto& fingerprint : db.fingerprints()) {
        if (fingerprint.algorithm == "sha1") {
            auto fingerprint_str = barrier::format_ssl_fingerprint(fingerprint.data);
            label_sha1_fingerprint_full->setText(QString::fromStdString(fingerprint_str));
            continue;
        }

        if (fingerprint.algorithm == "sha256") {
            auto fingerprint_str = barrier::format_ssl_fingerprint(fingerprint.data);
            fingerprint_str.resize(40);
            fingerprint_str += " ...";

            auto fingerprint_str_cols = barrier::format_ssl_fingerprint_columns(fingerprint.data);
            auto fingerprint_randomart = barrier::create_fingerprint_randomart(fingerprint.data);

            m_pLabelLocalFingerprint->setText(QString::fromStdString(fingerprint_str));
            label_sha256_fingerprint_full->setText(QString::fromStdString(fingerprint_str_cols));
            label_sha256_randomart->setText(QString::fromStdString(fingerprint_randomart));
        }
    }

    toolbutton_show_fingerprint->setEnabled(true);
}

void MainWindow::on_m_pGroupClient_toggled(bool on)
{
    m_pGroupServer->setChecked(!on);
    if (on) {
        updateZeroconfService();
#if defined(Q_OS_MAC)
        configureProximityController();
#endif
    }
}

void MainWindow::on_m_pGroupServer_toggled(bool on)
{
    m_pGroupClient->setChecked(!on);
    if (on) {
        updateZeroconfService();
#if defined(Q_OS_MAC)
        configureProximityController();
#endif
    }
}

bool MainWindow::on_m_pButtonBrowseConfigFile_clicked()
{
    QString fileName = QFileDialog::getOpenFileName(this, tr("Browse for a barriers config file"), QString(), barrierConfigOpenFilter);

    if (!fileName.isEmpty())
    {
        m_pLineEditConfigFile->setText(fileName);
        return true;
    }

    return false;
}

bool MainWindow::on_m_pActionSave_triggered()
{
    QString fileName = QFileDialog::getSaveFileName(this, tr("Save configuration as..."), QString(), barrierConfigSaveFilter);

    if (!fileName.isEmpty() && !serverConfig().save(fileName))
    {
        QMessageBox::warning(this, tr("Save failed"), tr("Could not save configuration to file."));
        return true;
    }

    return false;
}

void MainWindow::on_m_pActionAbout_triggered()
{
    AboutDialog(this, appPath(appConfig().barriercName())).exec();
}

void MainWindow::on_m_pActionSettings_triggered()
{
    if (SettingsDialog(this, appConfig()).exec() == QDialog::Accepted)
        updateSSLFingerprint();
}

void MainWindow::autoAddScreen(const QString name)
{
    if (!m_ServerConfig.ignoreAutoConfigClient()) {
        int r = m_ServerConfig.autoAddScreen(name);
        if (r != kAutoAddScreenOk) {
            switch (r) {
            case kAutoAddScreenManualServer:
                showConfigureServer(
                    tr("Please add the server (%1) to the grid.")
                        .arg(appConfig().screenName()));
                break;

            case kAutoAddScreenManualClient:
                showConfigureServer(
                    tr("Please drag the new client screen (%1) "
                        "to the desired position on the grid.")
                        .arg(name));
                break;
            }
        }
        else {
            restartBarrier();
        }
    }
}

bool MainWindow::reloadRunningServerConfig(QString& error)
{
    if (m_ExpectedRunningState != kStarted ||
        barrier_type() != BarrierType::Server) {
        return true;
    }
    if (!m_pRadioInternalConfig->isChecked() ||
        m_pTempConfigFile == NULL) {
        error = tr("The display profile was saved. Select the internal "
                   "configuration and save again to apply it live.");
        return false;
    }

    QFile configFile(m_pTempConfigFile->fileName());
    if (!configFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = tr("The display profile was saved, but the running server "
                   "configuration could not be updated. Check file permissions "
                   "and save again.");
        return false;
    }
    serverConfig().save(configFile);
    configFile.flush();
    const bool configWritten = configFile.error() == QFile::NoError;
    configFile.close();
    if (!configWritten) {
        error = tr("The display profile was saved, but the running server "
                   "configuration could not be written completely. Check the "
                   "available disk space and save again.");
        return false;
    }

    if (appConfig().processMode() == Service) {
        if (!m_IpcClient.sendReload()) {
            error = tr("The display profile was saved, but the Barrier service "
                       "is not connected. Reconnect the service and save again.");
            return false;
        }
        return true;
    }

#if defined(Q_OS_UNIX)
    if (barrierProcess() == NULL ||
        barrierProcess()->state() == QProcess::NotRunning ||
        barrierProcess()->processId() <= 0 ||
        ::kill(static_cast<pid_t>(barrierProcess()->processId()), SIGHUP) != 0) {
        error = tr("The display profile was saved, but the running server "
                   "could not be signaled. Verify it is running and save again.");
        return false;
    }
    return true;
#else
    error = tr("The display profile was saved, but live desktop reload is "
               "not supported on this platform.");
    return false;
#endif
}

void MainWindow::showConfigureServer(const QString& message)
{
    ServerConfigDialog dlg(this, serverConfig(), appConfig().screenName());
    dlg.message(message);
    if (dlg.exec() == QDialog::Accepted &&
        serverConfig().hasCurrentTopology()) {
        QString reloadError;
        if (!reloadRunningServerConfig(reloadError)) {
            QMessageBox::warning(
                this, tr("Display profile saved"), reloadError);
        }
    }
}

void MainWindow::on_m_pButtonConfigureServer_clicked()
{
    showConfigureServer();
}

void MainWindow::on_m_pButtonReload_clicked()
{
    restartBarrier();
}

#if defined(Q_OS_WIN)
bool MainWindow::isServiceRunning(QString name)
{
    SC_HANDLE hSCManager;
    hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (hSCManager == NULL) {
        appendLogError("failed to open a service controller manager, error: " +
            GetLastError());
        return false;
    }

    auto array = name.toLocal8Bit();
    SC_HANDLE hService = OpenService(hSCManager, array.data(), SERVICE_QUERY_STATUS);

    if (hService == NULL) {
        appendLogDebug("failed to open service: " + name);
        return false;
    }

    SERVICE_STATUS status;
    if (QueryServiceStatus(hService, &status)) {
        if (status.dwCurrentState == SERVICE_RUNNING) {
            return true;
        }
    }

    return false;
}
#else
bool MainWindow::isServiceRunning()
{
    return false;
}
#endif

bool MainWindow::isBonjourRunning()
{
    bool result = false;

#if defined(Q_OS_WIN)
    result = isServiceRunning("Bonjour Service");
#else
    result = true;
#endif

    return result;
}

void MainWindow::downloadBonjour()
{
#if defined(Q_OS_WIN)
    QUrl url;
    int arch = getProcessorArch();
    if (arch == kProcessorArchWin32) {
        url.setUrl(bonjourBaseUrl + bonjourFilename32);
        appendLogInfo("downloading 32-bit Bonjour");
    }
    else if (arch == kProcessorArchWin64) {
        url.setUrl(bonjourBaseUrl + bonjourFilename64);
        appendLogInfo("downloading 64-bit Bonjour");
    }
    else {
        QMessageBox::critical(
            this, tr("Barrier"),
            tr("Failed to detect system architecture."));
        return;
    }

    if (m_pDataDownloader == NULL) {
        m_pDataDownloader = new DataDownloader(this);
        connect(m_pDataDownloader, SIGNAL(isComplete()), SLOT(installBonjour()));
    }

    m_pDataDownloader->download(url);

    if (m_DownloadMessageBox == NULL) {
        m_DownloadMessageBox = new QMessageBox(this);
        m_DownloadMessageBox->setWindowTitle("Barrier");
        m_DownloadMessageBox->setIcon(QMessageBox::Information);
        m_DownloadMessageBox->setText("Installing Bonjour, please wait...");
        m_DownloadMessageBox->setStandardButtons(0);
        m_pCancelButton = m_DownloadMessageBox->addButton(
            tr("Cancel"), QMessageBox::RejectRole);
    }

    m_DownloadMessageBox->exec();

    if (m_DownloadMessageBox->clickedButton() == m_pCancelButton) {
        m_pDataDownloader->cancel();
    }
#endif
}

void MainWindow::installBonjour()
{
#if defined(Q_OS_WIN)
#if QT_VERSION >= 0x050000
    QString tempLocation = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
#else
    QString tempLocation = QDesktopServices::storageLocation(
                                QDesktopServices::TempLocation);
#endif
    QString filename = tempLocation;
    filename.append("\\").append(bonjourTargetFilename);
    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly)) {
        m_DownloadMessageBox->hide();

        QMessageBox::warning(
            this, "Barrier",
            tr("Failed to download Bonjour installer to location: %1")
            .arg(tempLocation));
        return;
    }

    file.write(m_pDataDownloader->data());
    file.close();

    QStringList arguments;
    arguments.append("/i");
    QString winFilename = QDir::toNativeSeparators(filename);
    arguments.append(winFilename);
    arguments.append("/passive");
    if (m_BonjourInstall == NULL) {
        m_BonjourInstall = new CommandProcess("msiexec", arguments);
    }

    QThread* thread = new QThread;
    connect(m_BonjourInstall, SIGNAL(finished()), this,
        SLOT(bonjourInstallFinished()));
    connect(m_BonjourInstall, SIGNAL(finished()), thread, SLOT(quit()));
    connect(thread, SIGNAL(finished()), thread, SLOT(deleteLater()));

    m_BonjourInstall->moveToThread(thread);
    thread->start();

    QMetaObject::invokeMethod(m_BonjourInstall, "run", Qt::QueuedConnection);

    m_DownloadMessageBox->hide();
#endif
}

void MainWindow::promptAutoConfig()
{
    if (!isBonjourRunning()) {
        int r = QMessageBox::question(
            this, tr("Barrier"),
            tr("Do you want to enable auto config and install Bonjour?\n\n"
               "This feature helps you establish the connection."),
            QMessageBox::Yes | QMessageBox::No);

        if (r == QMessageBox::Yes) {
            m_AppConfig->setAutoConfig(true);
            downloadBonjour();
        }
        else {
            m_AppConfig->setAutoConfig(false);
            m_pCheckBoxAutoConfig->setChecked(false);
        }
    }

    m_AppConfig->setAutoConfigPrompted(true);
}

void MainWindow::on_m_pComboServerList_currentIndexChanged(QString )
{
#if defined(Q_OS_MAC)
    if (proximityClientEnabled()) {
        reconcileProximityClient();
        return;
    }
#endif
    if (m_pComboServerList->count() != 0) {
        restartBarrier();
    }
}

void MainWindow::on_m_pCheckBoxAutoConfig_toggled(bool checked)
{
    if (!isBonjourRunning() && checked) {
        if (!m_SuppressAutoConfigWarning) {
            int r = QMessageBox::information(
                this, tr("Barrier"),
                tr("Auto config feature requires Bonjour.\n\n"
                   "Do you want to install Bonjour?"),
                QMessageBox::Yes | QMessageBox::No);

            if (r == QMessageBox::Yes) {
                downloadBonjour();
            }
        }

        m_pCheckBoxAutoConfig->setChecked(false);
        return;
    }

    m_pLineEditHostname->setDisabled(checked);
    appConfig().setAutoConfig(checked);
    updateZeroconfService();

    if (!checked) {
        m_pComboServerList->clear();
        m_pComboServerList->hide();
    }
}

void MainWindow::bonjourInstallFinished()
{
    appendLogInfo("Bonjour install finished");

    m_pCheckBoxAutoConfig->setChecked(true);
}

void MainWindow::windowStateChanged()
{
    if (windowState() == Qt::WindowMinimized && appConfig().getMinimizeToTray())
        hide();
}

void MainWindow::showLogWindow()
{
    showAndActivate(m_pLogWindow);
}
