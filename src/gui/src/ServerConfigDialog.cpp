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

ServerConfigDialog::ServerConfigDialog(QWidget* parent, ServerConfig& config, const QString& defaultScreenName) :
    QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint),
    Ui::ServerConfigDialogBase(),
    m_OrigServerConfig(config),
    m_ServerConfig(config),
    m_ScreenSetupModel(serverConfig().screens(), serverConfig().numColumns(), serverConfig().numRows()),
    m_Message(""),
    m_LocalScreenName(defaultScreenName)
{
    setupUi(this);

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
    // Insert the freeform canvas right below the grid view
    QVBoxLayout* tabLayout = qobject_cast<QVBoxLayout*>(m_pTabScreens->layout());
    if (tabLayout) {
        // The grid view is the second item in the tab's layout (after the hbox with trash)
        // Insert the freeform widget after it
        tabLayout->insertWidget(2, m_pFreeformWidget);
    }
    m_pFreeformWidget->setServerScreenName(m_LocalScreenName);
    m_pFreeformWidget->syncFromSystemDisplays();
        // Seed client rects from the first screen that isn't this machine
        // (the server). Comparing against m_LocalScreenName -- not
        // "screens()[0]" -- because grid slot 0 is often an empty/unrelated
        // cell, not actually the server's own screen.
        QString clientName;
        for (const Screen& s : serverConfig().screens()) {
            if (!s.isNull() && s.name().compare(m_LocalScreenName, Qt::CaseInsensitive) != 0) {
                clientName = s.name();
                break;
            }
        }
        if (!clientName.isEmpty()) {
            // Client display names (ordered per-display product names from
            // the client's DDNM metadata) reach the dialog through
            // ServerConfig: the daemon's ClientProxy1_0 logs them and
            // MainWindow parses them into the config. Empty/absent names
            // keep the canvas "<client screen name> #<index>" fallbacks.
            QList<QRect> existing;
            if (serverConfig().getFreeformDisplayRects(clientName, existing) && !existing.isEmpty()) {
                m_pFreeformWidget->setClientDisplays(clientName, existing);
            } else {
                // Default single display for the client
                QList<QRect> def;
                def.append(QRect(0, 0, 1920, 1080));
                m_pFreeformWidget->setClientDisplays(clientName, def);
            }
            QStringList clientNames;
            if (serverConfig().getFreeformDisplayNames(clientName, clientNames)) {
                m_pFreeformWidget->setClientDisplayNames(clientNames);
            }
            int fx = 0, fy = 0;
            if (serverConfig().getFreeformPosition(clientName, fx, fy)) {
                m_pFreeformWidget->setClientPosition(QPoint(fx, fy));
            }
        }
        connect(m_pFreeformWidget, &FreeformServerConfigWidget::clientPositionChanged,
                this, [this](const QPoint& pos) {
                    // Persist freeform position for the client screen
                    QString cname;
                    for (const Screen& s : serverConfig().screens()) {
                        if (!s.isNull() && s.name().compare(m_LocalScreenName, Qt::CaseInsensitive) != 0) {
                            cname = s.name();
                            break;
                        }
                    }
                    if (!cname.isEmpty()) {
                        serverConfig().setFreeformPosition(cname, pos.x(), pos.y());
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

    if (m_pFreeformWidget) {
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
        // Client with its dragged position and display rects
        QString cname = m_pFreeformWidget->clientName();
        if (cname.isEmpty()) {
            for (const Screen& s : serverConfig().screens()) {
                if (!s.isNull() && s.name().compare(sname, Qt::CaseInsensitive) != 0) {
                    cname = s.name();
                    break;
                }
            }
        }
        if (!cname.isEmpty()) {
            serverConfig().setFreeformDisplayRects(cname, m_pFreeformWidget->clientDisplays());
            serverConfig().setFreeformDisplayNames(cname, m_pFreeformWidget->clientDisplayNames());
            QPoint pos = m_pFreeformWidget->clientPosition();
            serverConfig().setFreeformPosition(cname, pos.x(), pos.y());
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

    // now that the dialog has been accepted, copy the new server config to the original one,
    // which is a reference to the one in MainWindow.
    setOrigServerConfig(serverConfig());

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
