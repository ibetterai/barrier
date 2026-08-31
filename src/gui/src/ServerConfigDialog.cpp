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

#include "ServerConfigDialog.h"
#include "ServerConfig.h"
#include "HotkeyDialog.h"
#include "ActionDialog.h"

#include <QtCore>
#include <QtGui>
#include <QApplication>
#include <QMessageBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>

ServerConfigDialog::ServerConfigDialog(QWidget* parent, ServerConfig& config, const QString& defaultScreenName) :
    QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint),
    Ui::ServerConfigDialogBase(),
    m_OrigServerConfig(config),
    m_ServerConfig(config),
    m_ScreenSetupModel(serverConfig().screens(), serverConfig().numColumns(), serverConfig().numRows()),
    m_Message(""),
    m_pFreeformWidget(nullptr),
    m_pTopologyProfileStatusLabel(nullptr),
    m_LocalScreenName(defaultScreenName)
{
    setupUi(this);
    // This editable copy must never persist when the dialog is cancelled.
    // Accepted state is copied back to the owning ServerConfig below.
    m_ServerConfig.m_persistSettings = false;


    m_pCheckBoxHeartbeat->setChecked(serverConfig().hasHeartbeat());
    m_pSpinBoxHeartbeat->setValue(serverConfig().heartbeat());

    m_pCheckBoxRelativeMouseMoves->setChecked(serverConfig().relativeMouseMoves());
    m_pCheckBoxScreenSaverSync->setChecked(serverConfig().screenSaverSync());
    m_pCheckBoxWin32KeepForeground->setChecked(serverConfig().win32KeepForeground());

    m_pCheckBoxSwitchDelay->setChecked(serverConfig().hasSwitchDelay());
    m_pSpinBoxSwitchDelay->setValue(serverConfig().switchDelay());

    m_pCheckBoxSwitchDoubleTap->setChecked(serverConfig().hasSwitchDoubleTap());
    m_pSpinBoxSwitchDoubleTap->setValue(serverConfig().switchDoubleTap());

    m_pCheckBoxCornerTopLeft->setChecked(serverConfig().switchCorner(BaseConfig::SwitchCorner::TopLeft));
    m_pCheckBoxCornerTopRight->setChecked(serverConfig().switchCorner(BaseConfig::SwitchCorner::TopRight));
    m_pCheckBoxCornerBottomLeft->setChecked(serverConfig().switchCorner(BaseConfig::SwitchCorner::BottomLeft));
    m_pCheckBoxCornerBottomRight->setChecked(serverConfig().switchCorner(BaseConfig::SwitchCorner::BottomRight));
    m_pSpinBoxSwitchCornerSize->setValue(serverConfig().switchCornerSize());

    m_pCheckBoxIgnoreAutoConfigClient->setChecked(serverConfig().ignoreAutoConfigClient());

    m_pCheckBoxEnableDragAndDrop->setChecked(serverConfig().enableDragAndDrop());

    m_pCheckBoxEnableClipboard->setChecked(serverConfig().clipboardSharing());

    for (const Hotkey& hotkey : serverConfig().hotkeys()) {
        m_pListHotkeys->addItem(hotkey.text());
    }

    m_pScreenSetupView->setModel(&m_ScreenSetupModel);

    // Create the freeform canvas that shows the real L-shaped display
    // layout and lets the client's display be dragged into the notch.
    m_pFreeformWidget = new FreeformServerConfigWidget(this);
    m_pTopologyProfileStatusLabel = new QLabel(this);
    m_pTopologyProfileStatusLabel->setObjectName(
        QStringLiteral("topologyProfileStatusLabel"));
    m_pTopologyProfileStatusLabel->setWordWrap(true);
    QVBoxLayout* tabLayout = qobject_cast<QVBoxLayout*>(m_pTabScreens->layout());
    if (tabLayout) {
        tabLayout->insertWidget(2, m_pTopologyProfileStatusLabel);
        tabLayout->insertWidget(3, m_pFreeformWidget);
    }
    m_pFreeformWidget->setServerScreenName(m_LocalScreenName);
    if (serverConfig().hasCurrentTopology()) {
        m_pFreeformWidget->setServerDisplays(
            serverConfig().currentServerDisplayRects());
    }
    else {
        m_pFreeformWidget->syncFromSystemDisplays();
    }

    QPushButton* saveButton = m_pButtonBox->button(QDialogButtonBox::Ok);
    if (serverConfig().topologyProfileLoadResult() !=
        barrier::TopologyProfileStoreResult::Ok) {
        m_pTopologyProfileStatusLabel->setText(
            tr("Saved display profiles could not be loaded: %1. "
               "Profile editing is disabled to protect the saved data.")
                .arg(serverConfig().topologyProfileError()));
        if (saveButton != nullptr) {
            saveButton->setText(tr("Profiles Unavailable"));
            saveButton->setEnabled(false);
        }
    }
    else if (serverConfig().hasCurrentTopology()) {
        const bool known = serverConfig().isCurrentTopologyKnown();
        m_pTopologyProfileStatusLabel->setText(
            known
                ? tr("Editing the saved profile for this display arrangement.")
                : tr("This display arrangement is not configured. Saving creates a new exact profile."));
        if (saveButton != nullptr) {
            saveButton->setText(known ? tr("Save Profile")
                                      : tr("Create Profile"));
        }
    }
    else {
        m_pTopologyProfileStatusLabel->hide();
    }
    // Seed every non-server screen into the freeform canvas. Earlier code
    // stopped after the first client, so a second 3.3.0 client could be
    // connected and present in ServerConfig but never drawn or persisted.
    QRect occupiedBounds;
    for (const QRect& rect : m_pFreeformWidget->serverDisplays()) {
        occupiedBounds = occupiedBounds.united(rect);
    }
    int fallbackX = occupiedBounds.isEmpty()
            ? 0
            : occupiedBounds.x() + occupiedBounds.width() + 20;
    auto includeClientBounds = [&occupiedBounds](const QList<QRect>& rects, const QPoint& pos) {
        for (const QRect& rect : rects) {
            occupiedBounds = occupiedBounds.united(rect.translated(pos));
        }
    };
    const int fallbackY = occupiedBounds.isEmpty() ? 0 : occupiedBounds.y();
    for (const Screen& s : serverConfig().screens()) {
        if (s.isNull() || s.name().compare(m_LocalScreenName, Qt::CaseInsensitive) == 0) {
            continue;
        }

        const QString clientName = s.name();
        QList<QRect> displayRects;
        if (!serverConfig().getFreeformDisplayRects(clientName, displayRects) ||
                displayRects.isEmpty()) {
            displayRects.append(QRect(0, 0, 1920, 1080));
        }
        m_pFreeformWidget->setClientDisplays(clientName, displayRects);

        QStringList displayNames;
        if (serverConfig().getFreeformDisplayNames(clientName, displayNames)) {
            m_pFreeformWidget->setClientDisplayNames(clientName, displayNames);
        }

        int fx = 0, fy = 0;
        if (serverConfig().getFreeformPosition(clientName, fx, fy)) {
            const QPoint savedPos(fx, fy);
            m_pFreeformWidget->setClientPosition(clientName, savedPos);
            includeClientBounds(displayRects, savedPos);
            fallbackX = occupiedBounds.x() + occupiedBounds.width() + 20;
            continue;
        }

        const QPoint fallbackPos(fallbackX, fallbackY);
        m_pFreeformWidget->setClientPosition(clientName, fallbackPos);
        includeClientBounds(displayRects, fallbackPos);
        fallbackX = occupiedBounds.x() + occupiedBounds.width() + 20;
    }
    connect(m_pFreeformWidget, &FreeformServerConfigWidget::clientPositionChanged,
            this, [this](const QString& clientName, const QPoint& pos) {
                if (!clientName.isEmpty()) {
                    serverConfig().setFreeformPosition(clientName, pos.x(), pos.y());
                }
            });

    // QTabWidget otherwise sizes itself (and the whole dialog) to fit the
    // TALLEST page. The freeform canvas makes "Screens and links" far
    // taller than "Hotkeys"/"Advanced server settings", which then show a
    // large empty gap on the right/bottom when selected. Make only the
    // current page contribute to the size, so switching tabs resizes the
    // dialog to fit whichever page is actually showing.
    connect(m_pTabWidget, &QTabWidget::currentChanged, this, [this](int index) {
        for (int i = 0; i < m_pTabWidget->count(); ++i) {
            QWidget* page = m_pTabWidget->widget(i);
            const bool current = (i == index);
            page->setSizePolicy(current ? QSizePolicy::Preferred : QSizePolicy::Ignored,
                                 current ? QSizePolicy::Preferred : QSizePolicy::Ignored);
        }
        QWidget* current = m_pTabWidget->widget(index);
        current->resize(current->minimumSizeHint());
        m_pTabWidget->resize(m_pTabWidget->minimumSizeHint());
        adjustSize();
    });

    if (serverConfig().numScreens() == 0)
        model().screen(serverConfig().numColumns() / 2, serverConfig().numRows() / 2) = Screen(defaultScreenName);
}

void ServerConfigDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);

    if (m_pFreeformWidget && !serverConfig().hasCurrentTopology()) {
        m_pFreeformWidget->syncFromSystemDisplays();
    }

    if (!m_Message.isEmpty())
    {
        // TODO: ideally this message box should pop up after the dialog is shown
        QMessageBox::information(this, tr("Configure server"), m_Message);
    }
}

void ServerConfigDialog::accept()
{
    serverConfig().haveHeartbeat(m_pCheckBoxHeartbeat->isChecked());
    serverConfig().setHeartbeat(m_pSpinBoxHeartbeat->value());

    // Persist the freeform canvas geometry so the generated config can
    // produce per-display partial edges for the L-shape / notch layout.
    if (m_pFreeformWidget) {
        // Server at origin with its real display rects (global CG coords).
        // Use m_LocalScreenName (this machine's own name), not "first
        // non-null screen" -- grid slot order has no relation to which
        // screen is actually the server, and saving under the wrong name
        // clobbers the client's entry while leaving the server with no
        // freeform geometry at all (link generation then falls back to a
        // generic default rect for it, breaking every adjacency).
        const QString& sname = m_LocalScreenName;
        if (!sname.isEmpty()) {
            serverConfig().setFreeformDisplayRects(sname, m_pFreeformWidget->serverDisplays());
            serverConfig().setFreeformPosition(sname, 0, 0);
        }
        // Every connected client with its dragged position and display rects.
        const QStringList clientNames = m_pFreeformWidget->clientNames();
        for (const QString& cname : clientNames) {
            if (cname.isEmpty()) continue;
            serverConfig().setFreeformDisplayRects(cname, m_pFreeformWidget->clientDisplays(cname));
            serverConfig().setFreeformDisplayNames(cname, m_pFreeformWidget->clientDisplayNames(cname));
            QPoint pos = m_pFreeformWidget->clientPosition(cname);
            serverConfig().setFreeformPosition(cname, pos.x(), pos.y());
        }
    }
    if (serverConfig().hasCurrentTopology()) {
        QString profileError;
        if (!serverConfig().saveCurrentTopologyProfile(&profileError)) {
            QMessageBox::warning(
                this, tr("Configure server"),
                tr("The display profile could not be saved: %1")
                    .arg(profileError));
            return;
        }
    }
    serverConfig().setRelativeMouseMoves(m_pCheckBoxRelativeMouseMoves->isChecked());
    serverConfig().setScreenSaverSync(m_pCheckBoxScreenSaverSync->isChecked());
    serverConfig().setWin32KeepForeground(m_pCheckBoxWin32KeepForeground->isChecked());

    serverConfig().haveSwitchDelay(m_pCheckBoxSwitchDelay->isChecked());
    serverConfig().setSwitchDelay(m_pSpinBoxSwitchDelay->value());

    serverConfig().haveSwitchDoubleTap(m_pCheckBoxSwitchDoubleTap->isChecked());
    serverConfig().setSwitchDoubleTap(m_pSpinBoxSwitchDoubleTap->value());

    serverConfig().setSwitchCorner(BaseConfig::SwitchCorner::TopLeft,
                                   m_pCheckBoxCornerTopLeft->isChecked());
    serverConfig().setSwitchCorner(BaseConfig::SwitchCorner::TopRight,
                                   m_pCheckBoxCornerTopRight->isChecked());
    serverConfig().setSwitchCorner(BaseConfig::SwitchCorner::BottomLeft,
                                   m_pCheckBoxCornerBottomLeft->isChecked());
    serverConfig().setSwitchCorner(BaseConfig::SwitchCorner::BottomRight,
                                   m_pCheckBoxCornerBottomRight->isChecked());
    serverConfig().setSwitchCornerSize(m_pSpinBoxSwitchCornerSize->value());
    serverConfig().setIgnoreAutoConfigClient(m_pCheckBoxIgnoreAutoConfigClient->isChecked());
    serverConfig().setEnableDragAndDrop(m_pCheckBoxEnableDragAndDrop->isChecked());
    serverConfig().setClipboardSharing(m_pCheckBoxEnableClipboard->isChecked());

    // Persist the editable copy before exposing it to the running server. A
    // profile that is already active in memory must survive a crash or forced
    // app restart immediately after this dialog closes.
    QString persistenceError;
    if (!m_OrigServerConfig.commitAcceptedConfiguration(
            serverConfig(), &persistenceError)) {
        QMessageBox::warning(
            this, tr("Configure server"),
            tr("The server configuration could not be saved: %1")
                .arg(persistenceError));
        return;
    }

    QDialog::accept();
}

void ServerConfigDialog::on_m_pButtonNewHotkey_clicked()
{
    Hotkey hotkey;
    HotkeyDialog dlg(this, hotkey);
    if (dlg.exec() == QDialog::Accepted)
    {
        serverConfig().hotkeys().push_back(hotkey);
        m_pListHotkeys->addItem(hotkey.text());
    }
}

void ServerConfigDialog::on_m_pButtonEditHotkey_clicked()
{
    int idx = m_pListHotkeys->currentRow();
    Q_ASSERT(idx >= 0 && idx < serverConfig().hotkeys().size());
    Hotkey& hotkey = serverConfig().hotkeys()[idx];
    HotkeyDialog dlg(this, hotkey);
    if (dlg.exec() == QDialog::Accepted)
        m_pListHotkeys->currentItem()->setText(hotkey.text());
}

void ServerConfigDialog::on_m_pButtonRemoveHotkey_clicked()
{
    int idx = m_pListHotkeys->currentRow();
    Q_ASSERT(idx >= 0 && idx < serverConfig().hotkeys().size());
    serverConfig().hotkeys().erase(serverConfig().hotkeys().begin() + idx);
    m_pListActions->clear();
    delete m_pListHotkeys->item(idx);
}

void ServerConfigDialog::on_m_pListHotkeys_itemSelectionChanged()
{
    bool itemsSelected = !m_pListHotkeys->selectedItems().isEmpty();
    m_pButtonEditHotkey->setEnabled(itemsSelected);
    m_pButtonRemoveHotkey->setEnabled(itemsSelected);
    m_pButtonNewAction->setEnabled(itemsSelected);

    if (itemsSelected && serverConfig().hotkeys().size() > 0)
    {
        m_pListActions->clear();

        int idx = m_pListHotkeys->row(m_pListHotkeys->selectedItems()[0]);

        // There's a bug somewhere around here: We get idx == 1 right after we deleted the next to last item, so idx can
        // only possibly be 0. GDB shows we got called indirectly from the delete line in
        // on_m_pButtonRemoveHotkey_clicked() above, but the delete is of course necessary and seems correct.
        // The while() is a generalized workaround for all that and shouldn't be required.
        while (idx >= 0 && idx >= serverConfig().hotkeys().size())
            idx--;

        Q_ASSERT(idx >= 0 && idx < serverConfig().hotkeys().size());

        const Hotkey& hotkey = serverConfig().hotkeys()[idx];
        for (const Action& action : hotkey.actions()) {
            m_pListActions->addItem(action.text());
        }
    }
}

void ServerConfigDialog::on_m_pButtonNewAction_clicked()
{
    int idx = m_pListHotkeys->currentRow();
    Q_ASSERT(idx >= 0 && idx < serverConfig().hotkeys().size());
    Hotkey& hotkey = serverConfig().hotkeys()[idx];

    Action action;
    ActionDialog dlg(this, serverConfig(), hotkey, action);
    if (dlg.exec() == QDialog::Accepted)
    {
        hotkey.appendAction(action);
        m_pListActions->addItem(action.text());
    }
}

void ServerConfigDialog::on_m_pButtonEditAction_clicked()
{
    int idxHotkey = m_pListHotkeys->currentRow();
    Q_ASSERT(idxHotkey >= 0 && idxHotkey < serverConfig().hotkeys().size());
    Hotkey& hotkey = serverConfig().hotkeys()[idxHotkey];

    int idxAction = m_pListActions->currentRow();
    Q_ASSERT(idxAction >= 0 && idxAction < hotkey.actions().size());
    Action action = hotkey.actions()[idxAction];

    ActionDialog dlg(this, serverConfig(), hotkey, action);
    if (dlg.exec() == QDialog::Accepted) {
        hotkey.setAction(idxAction, action);
        m_pListActions->currentItem()->setText(action.text());
    }
}

void ServerConfigDialog::on_m_pButtonRemoveAction_clicked()
{
    int idxHotkey = m_pListHotkeys->currentRow();
    Q_ASSERT(idxHotkey >= 0 && idxHotkey < serverConfig().hotkeys().size());
    Hotkey& hotkey = serverConfig().hotkeys()[idxHotkey];

    int idxAction = m_pListActions->currentRow();
    Q_ASSERT(idxAction >= 0 && idxAction < hotkey.actions().size());

    hotkey.removeAction(idxAction);
    delete m_pListActions->currentItem();
}

void ServerConfigDialog::on_m_pListActions_itemSelectionChanged()
{
    m_pButtonEditAction->setEnabled(!m_pListActions->selectedItems().isEmpty());
    m_pButtonRemoveAction->setEnabled(!m_pListActions->selectedItems().isEmpty());
}
