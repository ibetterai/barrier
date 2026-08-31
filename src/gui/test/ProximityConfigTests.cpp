/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "../src/ProximityConfig.h"

#include <gtest/gtest.h>

#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>

namespace {

QString settingsPath(QTemporaryDir& directory)
{
    return directory.filePath("barrier.ini");
}

barrier::ProximityPairing pairing(const QString& id,
                                  const QUuid& peripheralId = QUuid::createUuid())
{
    return {id, peripheralId, QStringLiteral("Desk server")};
}

} // namespace

TEST(ProximityConfigTests, pairingAndFlagsSurviveRelaunch)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(settingsPath(directory), QSettings::IniFormat);
    barrier::ProximityConfig config(settings);
    const barrier::ProximityPairing original = pairing(
        QStringLiteral("00112233445566778899aabbccddeeff"));

    ASSERT_TRUE(config.setServerAdvertiserEnabled(true));
    ASSERT_TRUE(config.setClientGatingEnabled(true));
    ASSERT_TRUE(config.replacePairing(original));

    QSettings reloadedSettings(settingsPath(directory), QSettings::IniFormat);
    barrier::ProximityConfig reloaded(reloadedSettings);
    barrier::ProximityPairing loaded;
    ASSERT_TRUE(reloaded.pairing(loaded));
    EXPECT_EQ(original.proximityId, loaded.proximityId);
    EXPECT_EQ(original.peripheralId, loaded.peripheralId);
    EXPECT_EQ(original.displayName, loaded.displayName);
    EXPECT_TRUE(reloaded.serverAdvertiserEnabled());
    EXPECT_TRUE(reloaded.clientGatingEnabled());
}

TEST(ProximityConfigTests, rejectsMalformedPairingWithoutReplacingCurrent)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(settingsPath(directory), QSettings::IniFormat);
    barrier::ProximityConfig config(settings);
    const barrier::ProximityPairing original = pairing(
        QStringLiteral("00112233445566778899aabbccddeeff"));
    ASSERT_TRUE(config.replacePairing(original));

    EXPECT_FALSE(config.replacePairing(pairing(
        QStringLiteral("00112233445566778899AABBCCDDEEFF"))));
    EXPECT_FALSE(config.replacePairing(pairing(QStringLiteral("abcd"))));
    EXPECT_FALSE(config.replacePairing(pairing(
        QStringLiteral("00112233445566778899aabbccddeefg"))));
    EXPECT_FALSE(config.replacePairing(pairing(
        QStringLiteral("00112233445566778899aabbccddeef٠"))));
    EXPECT_FALSE(config.replacePairing(pairing(
        QStringLiteral("ffeeddccbbaa99887766554433221100"), QUuid())));

    barrier::ProximityPairing retained;
    ASSERT_TRUE(config.pairing(retained));
    EXPECT_EQ(original.proximityId, retained.proximityId);
    EXPECT_EQ(original.peripheralId, retained.peripheralId);
}

TEST(ProximityConfigTests, legacyThresholdsNeverControlOrInvalidatePairing)
{
    const QList<QPair<QVariant, QVariant>> legacyThresholds{
        {-45, -51},
        {-64, -70},
        {QVariant(), QVariant()},
        {QStringLiteral("bad"), QStringLiteral("values")},
        {-90, -75}
    };

    for (const auto& thresholds : legacyThresholds) {
        QTemporaryDir directory;
        ASSERT_TRUE(directory.isValid());
        QSettings settings(settingsPath(directory), QSettings::IniFormat);
        settings.beginGroup("proximity/pairing");
        settings.setValue("proximityId",
                          "00112233445566778899aabbccddeeff");
        settings.setValue("peripheralId",
                          QUuid::createUuid().toString());
        settings.setValue("displayName", "Desk server");
        if (thresholds.first.isValid()) {
            settings.setValue("enterDbm", thresholds.first);
        }
        if (thresholds.second.isValid()) {
            settings.setValue("exitDbm", thresholds.second);
        }
        settings.endGroup();
        settings.sync();

        barrier::ProximityConfig config(settings);
        barrier::ProximityPairing loaded;
        ASSERT_TRUE(config.pairing(loaded))
            << thresholds.first.toString().toStdString() << "/"
            << thresholds.second.toString().toStdString();
        EXPECT_EQ(barrier::kProximityEnterThresholdDbm,
                  loaded.thresholdPolicy.connectDbm);
        EXPECT_EQ(barrier::kProximityExitThresholdDbm,
                  loaded.thresholdPolicy.departureDbm);
    }
}

TEST(ProximityConfigTests, versionedThresholdPolicyRoundTripsAtomically)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(settingsPath(directory), QSettings::IniFormat);
    barrier::ProximityConfig config(settings);
    const barrier::ProximityPairing original = pairing(
        QStringLiteral("00112233445566778899aabbccddeeff"));
    ASSERT_TRUE(config.replacePairing(original));

    const barrier::ProximityThresholdPolicy policy{-65, -82};
    EXPECT_EQ(
        barrier::ProximityConfigApplyResult::Changed,
        config.applyClientSettings(
            true, original.proximityId, original.peripheralId, policy));

    QSettings reloadedSettings(settingsPath(directory), QSettings::IniFormat);
    barrier::ProximityConfig reloaded(reloadedSettings);
    barrier::ProximityPairing loaded;
    ASSERT_TRUE(reloaded.pairing(loaded));
    EXPECT_TRUE(reloaded.clientGatingEnabled());
    EXPECT_EQ(policy, loaded.thresholdPolicy);
    EXPECT_EQ(1, reloadedSettings.value(
        QStringLiteral("proximity/pairing/thresholdPolicyVersion")).toInt());
    EXPECT_EQ(-65, reloadedSettings.value(
        QStringLiteral("proximity/pairing/connectDbm")).toInt());
    EXPECT_EQ(-82, reloadedSettings.value(
        QStringLiteral("proximity/pairing/departureDbm")).toInt());
    // Rollback readers still see the fixed legacy policy, never a newly
    // configurable value that older builds would misinterpret.
    EXPECT_EQ(-75, reloadedSettings.value(
        QStringLiteral("proximity/pairing/enterDbm")).toInt());
    EXPECT_EQ(-90, reloadedSettings.value(
        QStringLiteral("proximity/pairing/exitDbm")).toInt());
}

TEST(ProximityConfigTests, malformedVersionedPolicyFallsBackWithoutLosingPairing)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(settingsPath(directory), QSettings::IniFormat);
    settings.beginGroup("proximity/pairing");
    settings.setValue("proximityId",
                      "00112233445566778899aabbccddeeff");
    settings.setValue("peripheralId", QUuid::createUuid().toString());
    settings.setValue("displayName", "Desk server");
    settings.setValue("thresholdPolicyVersion", 1);
    settings.setValue("connectDbm", -80);
    settings.setValue("departureDbm", -75);
    settings.endGroup();
    settings.sync();

    barrier::ProximityConfig config(settings);
    barrier::ProximityPairing loaded;
    ASSERT_TRUE(config.pairing(loaded));
    EXPECT_EQ(barrier::ProximityThresholdPolicy(), loaded.thresholdPolicy);
}

TEST(ProximityConfigTests, invalidOrStaleThresholdDraftDoesNotPartiallyWrite)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(settingsPath(directory), QSettings::IniFormat);
    barrier::ProximityConfig config(settings);
    const barrier::ProximityPairing original = pairing(
        QStringLiteral("00112233445566778899aabbccddeeff"));
    ASSERT_TRUE(config.replacePairing(original));

    EXPECT_EQ(
        barrier::ProximityConfigApplyResult::Error,
        config.applyClientSettings(
            true, original.proximityId, original.peripheralId,
            barrier::ProximityThresholdPolicy{-80, -85}));
    EXPECT_EQ(
        barrier::ProximityConfigApplyResult::Error,
        config.applyClientSettings(
            true,
            QStringLiteral("ffeeddccbbaa99887766554433221100"),
            original.peripheralId,
            barrier::ProximityThresholdPolicy{-65, -82}));

    QSettings reloadedSettings(settingsPath(directory), QSettings::IniFormat);
    barrier::ProximityConfig reloaded(reloadedSettings);
    barrier::ProximityPairing loaded;
    ASSERT_TRUE(reloaded.pairing(loaded));
    EXPECT_FALSE(reloaded.clientGatingEnabled());
    EXPECT_EQ(barrier::ProximityThresholdPolicy(), loaded.thresholdPolicy);
}

TEST(ProximityConfigTests, unchangedClientSettingsAreANoOp)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(settingsPath(directory), QSettings::IniFormat);
    barrier::ProximityConfig config(settings);
    const barrier::ProximityPairing original = pairing(
        QStringLiteral("00112233445566778899aabbccddeeff"));
    ASSERT_TRUE(config.replacePairing(original));

    EXPECT_EQ(
        barrier::ProximityConfigApplyResult::Unchanged,
        config.applyClientSettings(
            false, original.proximityId, original.peripheralId,
            barrier::ProximityThresholdPolicy()));
}

TEST(ProximityConfigTests, pairingCreatesAndRotatesOpaqueClientRoutingId)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(settingsPath(directory), QSettings::IniFormat);
    barrier::ProximityConfig config(settings);
    const barrier::ProximityPairing firstDraft = pairing(
        QStringLiteral("00112233445566778899aabbccddeeff"));
    ASSERT_TRUE(config.replacePairing(firstDraft));

    barrier::ProximityPairing first;
    ASSERT_TRUE(config.pairing(first));
    EXPECT_EQ(32, first.clientRoutingId.size());
    EXPECT_FALSE(first.signalSharingEnabled);

    const barrier::ProximityPairing secondDraft = pairing(
        QStringLiteral("ffeeddccbbaa99887766554433221100"));
    ASSERT_TRUE(config.replacePairing(secondDraft));
    barrier::ProximityPairing second;
    ASSERT_TRUE(config.pairing(second));
    EXPECT_EQ(32, second.clientRoutingId.size());
    EXPECT_NE(first.clientRoutingId, second.clientRoutingId);
    EXPECT_FALSE(second.signalSharingEnabled);

    ASSERT_TRUE(config.clearPairing());
    EXPECT_FALSE(settings.contains(
        QStringLiteral("proximity/pairing/clientRoutingId")));
}

TEST(ProximityConfigTests, signalSharingOptInCommitsWithClientDraft)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(settingsPath(directory), QSettings::IniFormat);
    barrier::ProximityConfig config(settings);
    const barrier::ProximityPairing draft = pairing(
        QStringLiteral("00112233445566778899aabbccddeeff"));
    ASSERT_TRUE(config.replacePairing(draft));
    barrier::ProximityPairing paired;
    ASSERT_TRUE(config.pairing(paired));

    EXPECT_EQ(
        barrier::ProximityConfigApplyResult::Changed,
        config.applyClientSettings(
            true, true, paired.proximityId, paired.peripheralId,
            barrier::ProximityThresholdPolicy{-65, -85}));

    QSettings reloadedSettings(settingsPath(directory), QSettings::IniFormat);
    barrier::ProximityConfig reloaded(reloadedSettings);
    barrier::ProximityPairing loaded;
    ASSERT_TRUE(reloaded.pairing(loaded));
    EXPECT_TRUE(loaded.signalSharingEnabled);
    EXPECT_EQ(paired.clientRoutingId, loaded.clientRoutingId);
    EXPECT_EQ((barrier::ProximityThresholdPolicy{-65, -85}),
              loaded.thresholdPolicy);
}

TEST(ProximityConfigTests,
     malformedLegacyClientRoutingIdDisablesSharingButKeepsPairing)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(settingsPath(directory), QSettings::IniFormat);
    settings.beginGroup("proximity/pairing");
    settings.setValue("proximityId",
                      "00112233445566778899aabbccddeeff");
    settings.setValue("peripheralId", QUuid::createUuid().toString());
    settings.setValue("displayName", "Desk server");
    settings.setValue("clientRoutingId", "not-valid");
    settings.setValue("signalSharingEnabled", true);
    settings.endGroup();
    settings.sync();

    barrier::ProximityConfig config(settings);
    barrier::ProximityPairing loaded;
    ASSERT_TRUE(config.pairing(loaded));
    EXPECT_TRUE(loaded.clientRoutingId.isEmpty());
    EXPECT_FALSE(loaded.signalSharingEnabled);
}

TEST(ProximityConfigTests,
     associatedPresenceRoutesPersistRotateAndRejectCollisions)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(settingsPath(directory), QSettings::IniFormat);
    barrier::ProximityConfig config(settings);
    QString error;
    const QString serverId = config.serverProximityId(&error);
    ASSERT_TRUE(error.isEmpty());
    const QString firstId =
        QStringLiteral("11112222333344445555666677778888");
    const QString rotatedId =
        QStringLiteral("9999aaaabbbbccccddddeeeeffff0000");

    EXPECT_EQ(
        barrier::ClientPresenceAssociationResult::Changed,
        config.associateClientPresenceRoute(
            QStringLiteral("client-alpha"), firstId, serverId, &error));
    EXPECT_EQ(
        barrier::ClientPresenceAssociationResult::Changed,
        config.associateClientPresenceRoute(
            QStringLiteral("client-alpha"), rotatedId, serverId, &error));
    EXPECT_EQ(
        barrier::ClientPresenceAssociationResult::Conflict,
        config.associateClientPresenceRoute(
            QStringLiteral("client-beta"), rotatedId, serverId, &error));

    QSettings reloadedSettings(settingsPath(directory), QSettings::IniFormat);
    barrier::ProximityConfig reloaded(reloadedSettings);
    const QMap<QString, QString> routes =
        reloaded.associatedClientPresenceRoutes(serverId);
    EXPECT_EQ(1, routes.size());
    EXPECT_EQ(QString::fromLatin1("client-alpha"), routes.value(rotatedId));
    EXPECT_FALSE(routes.contains(firstId));

    ASSERT_TRUE(reloaded.resetServerProximityId(&error));
    EXPECT_TRUE(reloaded.associatedClientPresenceRoutes(serverId).isEmpty());
}

TEST(ProximityConfigTests, writesFixedCompatibilityThresholds)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(settingsPath(directory), QSettings::IniFormat);
    barrier::ProximityConfig config(settings);
    ASSERT_TRUE(config.replacePairing(pairing(
        QStringLiteral("00112233445566778899aabbccddeeff"))));

    settings.sync();
    EXPECT_EQ(-75, settings.value(
        QStringLiteral("proximity/pairing/enterDbm")).toInt());
    EXPECT_EQ(-90, settings.value(
        QStringLiteral("proximity/pairing/exitDbm")).toInt());
}

TEST(ProximityConfigTests, cancelledReplacementLeavesPersistentPairingUnchanged)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(settingsPath(directory), QSettings::IniFormat);
    barrier::ProximityConfig config(settings);
    const barrier::ProximityPairing original = pairing(
        QStringLiteral("00112233445566778899aabbccddeeff"));
    ASSERT_TRUE(config.replacePairing(original));

    const barrier::ProximityPairing draft = pairing(
        QStringLiteral("ffeeddccbbaa99887766554433221100"));
    (void) draft; // Dialog cancellation discards temporary state.

    QSettings reloadedSettings(settingsPath(directory), QSettings::IniFormat);
    barrier::ProximityConfig reloaded(reloadedSettings);
    barrier::ProximityPairing retained;
    ASSERT_TRUE(reloaded.pairing(retained));
    EXPECT_EQ(original.proximityId, retained.proximityId);
    EXPECT_EQ(original.peripheralId, retained.peripheralId);
}

TEST(ProximityConfigTests, serverProximityIdIsGeneratedOnce)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings firstSettings(settingsPath(directory), QSettings::IniFormat);
    QSettings concurrentSettings(settingsPath(directory), QSettings::IniFormat);
    barrier::ProximityConfig config(firstSettings);
    barrier::ProximityConfig concurrent(concurrentSettings);
    QString error;

    const QString first = config.serverProximityId(&error);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_EQ(32, first.size());
    for (const QChar character : first) {
        EXPECT_TRUE((character >= QLatin1Char('0') &&
                     character <= QLatin1Char('9')) ||
                    (character >= QLatin1Char('a') &&
                     character <= QLatin1Char('f')));
    }

    EXPECT_EQ(first, concurrent.serverProximityId(&error));
    EXPECT_TRUE(error.isEmpty()) << error.toStdString();
    QSettings reloadedSettings(settingsPath(directory), QSettings::IniFormat);
    barrier::ProximityConfig reloaded(reloadedSettings);
    EXPECT_EQ(first, reloaded.serverProximityId(&error));
    EXPECT_TRUE(error.isEmpty()) << error.toStdString();
    EXPECT_EQ(first, config.serverProximityId(&error));
}

TEST(ProximityConfigTests, resetServerIdentityReplacesOpaqueValue)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(settingsPath(directory), QSettings::IniFormat);
    barrier::ProximityConfig config(settings);
    QString error;
    const QString first = config.serverProximityId(&error);
    ASSERT_FALSE(first.isEmpty());
    ASSERT_TRUE(config.hasServerProximityId());

    ASSERT_TRUE(config.resetServerProximityId(&error)) <<
        error.toStdString();
    EXPECT_FALSE(config.hasServerProximityId());
    const QString replacement = config.serverProximityId(&error);
    EXPECT_FALSE(replacement.isEmpty());
    EXPECT_NE(first, replacement);

    QSettings reloadedSettings(settingsPath(directory), QSettings::IniFormat);
    barrier::ProximityConfig reloaded(reloadedSettings);
    EXPECT_EQ(replacement, reloaded.serverProximityId(&error));
}

TEST(ProximityConfigTests, malformedPersistedPairingIsIgnoredExplicitly)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(settingsPath(directory), QSettings::IniFormat);
    settings.beginGroup("proximity/pairing");
    settings.setValue("proximityId", "bad-id");
    settings.setValue("peripheralId", QUuid::createUuid().toString());
    settings.setValue("displayName", "Desk server");
    settings.setValue("enterDbm", -56);
    settings.setValue("exitDbm", -62);
    settings.endGroup();
    settings.sync();

    barrier::ProximityConfig config(settings);
    barrier::ProximityPairing loaded;
    EXPECT_FALSE(config.pairing(loaded));
    EXPECT_FALSE(config.error().isEmpty());
    ASSERT_TRUE(config.clearPairing());
    QSettings reloadedSettings(settingsPath(directory), QSettings::IniFormat);
    barrier::ProximityConfig reloaded(reloadedSettings);
    EXPECT_FALSE(reloaded.pairing(loaded));
    EXPECT_TRUE(reloaded.error().isEmpty());
}

TEST(ProximityConfigTests, staleInstanceClearsCurrentPersistentPairing)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings staleSettings(settingsPath(directory), QSettings::IniFormat);
    QSettings writerSettings(settingsPath(directory), QSettings::IniFormat);
    barrier::ProximityConfig stale(staleSettings);
    barrier::ProximityConfig writer(writerSettings);
    ASSERT_TRUE(writer.replacePairing(pairing(
        QStringLiteral("00112233445566778899aabbccddeeff"))));

    ASSERT_TRUE(stale.clearPairing());

    QSettings reloadedSettings(settingsPath(directory), QSettings::IniFormat);
    barrier::ProximityConfig reloaded(reloadedSettings);
    barrier::ProximityPairing loaded;
    EXPECT_FALSE(reloaded.pairing(loaded));
}
