/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "ui_ProximitySettingsDialogBase.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QAbstractButton>
#include <QFontMetrics>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QTreeWidget>

namespace {

class ProximityDialogSurface
{
public:
    explicit ProximityDialogSurface(bool serverMode = false)
    {
        ui.setupUi(&dialog);
        ui.serverGroup->setVisible(serverMode);
        ui.clientGroup->setVisible(!serverMode);
        dialog.resize(dialog.minimumSize());
        dialog.show();
        QApplication::processEvents();
    }

    QDialog dialog;
    Ui::ProximitySettingsDialogBase ui;
};

} // namespace

TEST(ProximitySettingsDialogUiTests, fieldsAreLabelledAndStatusTextFits)
{
    ProximityDialogSurface clientSurface;
    ProximityDialogSurface serverSurface(true);

    EXPECT_EQ(clientSurface.ui.nearbyServersList,
              clientSurface.ui.nearbyServersLabel->buddy());
    EXPECT_FALSE(
        clientSurface.ui.nearbyServersList->accessibleName().isEmpty());
    EXPECT_FALSE(serverSurface.ui.advertisingCheckBox->text().isEmpty());
    EXPECT_EQ(serverSurface.ui.nearbyClientsTree,
              serverSurface.ui.nearbyClientsLabel->buddy());
    EXPECT_FALSE(
        serverSurface.ui.nearbyClientsTree->accessibleName().isEmpty());
    EXPECT_FALSE(clientSurface.ui.gatingCheckBox->text().isEmpty());
    EXPECT_FALSE(clientSurface.ui.signalSharingCheckBox->text().isEmpty());

    const QList<QLabel*> statusLabels{
        clientSurface.ui.introLabel,
        clientSurface.ui.pairedServerLabel,
        clientSurface.ui.filteredRssiLabel,
        clientSurface.ui.signalSharingInfoLabel,
        clientSurface.ui.thresholdHelpLabel,
        clientSurface.ui.thresholdValidationLabel,
        clientSurface.ui.pairingStatusLabel,
        serverSurface.ui.introLabel,
        serverSurface.ui.serverIdentityStatusLabel,
        serverSurface.ui.wakeInfoLabel,
        serverSurface.ui.clientPresenceStatusLabel
    };
    for (QLabel* label : statusLabels) {
        EXPECT_TRUE(label->wordWrap());
        const QRect textBounds = label->fontMetrics().boundingRect(
            QRect(0, 0, label->contentsRect().width(), 1000),
            Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
            label->text());
        EXPECT_GE(label->contentsRect().height(), textBounds.height())
            << label->objectName().toStdString();
    }
}

TEST(ProximitySettingsDialogUiTests, buttonsMeetDesktopHitTargetFloor)
{
    ProximityDialogSurface surface;
    const QList<QAbstractButton*> buttons =
        surface.dialog.findChildren<QAbstractButton*>();
    ASSERT_FALSE(buttons.isEmpty());
    for (QAbstractButton* button : buttons) {
        EXPECT_GE(button->height(), 32)
            << button->text().toStdString();
        EXPECT_GE(button->width(), 32)
            << button->text().toStdString();
    }
    EXPECT_GE(surface.ui.connectDbmSpinBox->height(), 32);
    EXPECT_GE(surface.ui.departureDbmSpinBox->height(), 32);
}

TEST(ProximitySettingsDialogUiTests, keyboardFocusReachesPairingAndCancelControls)
{
    ProximityDialogSurface surface;
    QSet<QWidget*> focusChain;
    QWidget* current = surface.ui.advertisingCheckBox;
    do {
        focusChain.insert(current);
        current = current->nextInFocusChain();
    } while (current != surface.ui.advertisingCheckBox &&
             focusChain.size() < 100);

    EXPECT_TRUE(focusChain.contains(surface.ui.pairButton));
    EXPECT_TRUE(focusChain.contains(surface.ui.forgetButton));
    EXPECT_TRUE(focusChain.contains(
        surface.ui.buttonBox->button(QDialogButtonBox::Cancel)));
    EXPECT_TRUE(focusChain.contains(
        surface.ui.buttonBox->button(QDialogButtonBox::Ok)));
}

TEST(ProximitySettingsDialogUiTests, thresholdControlsAreLabelledAndBounded)
{
    ProximityDialogSurface surface;
    EXPECT_EQ(QString::fromLatin1("Pair"), surface.ui.pairButton->text());
    EXPECT_EQ(surface.ui.connectDbmSpinBox,
              surface.ui.connectDbmLabel->buddy());
    EXPECT_EQ(surface.ui.departureDbmSpinBox,
              surface.ui.departureDbmLabel->buddy());
    EXPECT_EQ(-100, surface.ui.connectDbmSpinBox->minimum());
    EXPECT_EQ(-30, surface.ui.connectDbmSpinBox->maximum());
    EXPECT_EQ(-100, surface.ui.departureDbmSpinBox->minimum());
    EXPECT_EQ(-30, surface.ui.departureDbmSpinBox->maximum());
    EXPECT_EQ(QString::fromLatin1(" dBm"),
              surface.ui.connectDbmSpinBox->suffix());
    EXPECT_EQ(QString::fromLatin1(" dBm"),
              surface.ui.departureDbmSpinBox->suffix());
    EXPECT_FALSE(surface.ui.connectDbmSpinBox->accessibleName().isEmpty());
    EXPECT_FALSE(surface.ui.departureDbmSpinBox->accessibleName().isEmpty());
    EXPECT_TRUE(surface.ui.thresholdHelpLabel->text().contains(
        QStringLiteral("15 dB")));

    const QList<QPushButton*> buttons =
        surface.dialog.findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        EXPECT_FALSE(button->text().contains(
            QStringLiteral("calibrat"), Qt::CaseInsensitive));
        EXPECT_FALSE(button->text().contains(
            QStringLiteral("adjust"), Qt::CaseInsensitive));
    }
}
