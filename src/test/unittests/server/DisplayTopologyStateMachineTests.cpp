/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "server/DisplayTopologyStateMachine.h"

#include "test/global/gtest.h"

#include <cstdint>
#include <string>

namespace {

barrier::DisplayTopology topology(const std::string& stableId, SInt32 width = 100)
{
    barrier::DisplayTopology value;
    value.displays.push_back({stableId, {0, 0, width, 100}, 0, true});
    return value;
}

void expectDecision(const DisplayTopologyDecision& decision,
                    DisplayTopologyState state,
                    bool switchingEnabled,
                    bool disconnectClients,
                    bool displayReady,
                    std::int64_t nextDeadlineMs)
{
    EXPECT_EQ(state, decision.state);
    EXPECT_EQ(switchingEnabled, decision.switchingEnabled);
    EXPECT_EQ(disconnectClients, decision.disconnectClients);
    EXPECT_EQ(displayReady, decision.displayReady);
    EXPECT_EQ(nextDeadlineMs, decision.nextDeadlineMs);
}

} // namespace

TEST(DisplayTopologyStateMachineTests, nonEmptyTopologySettlesAfterQuietPeriod)
{
    DisplayTopologyStateMachine machine;

    expectDecision(machine.observe(topology("internal"), true, 100),
                   DisplayTopologyState::Reconfiguring,
                   false, false, false, 2100);
    expectDecision(machine.onDeadline(2099),
                   DisplayTopologyState::Reconfiguring,
                   false, false, false, 2100);
    expectDecision(machine.onDeadline(2100),
                   DisplayTopologyState::StableKnown,
                   true, false, true, -1);
}

TEST(DisplayTopologyStateMachineTests, topologyChangeResetsQuietDeadline)
{
    DisplayTopologyStateMachine machine;
    machine.observe(topology("internal"), true, 100);

    expectDecision(machine.observe(topology("external"), false, 1500),
                   DisplayTopologyState::Reconfiguring,
                   false, false, false, 3500);
    expectDecision(machine.onDeadline(2100),
                   DisplayTopologyState::Reconfiguring,
                   false, false, false, 3500);
    expectDecision(machine.onDeadline(3500),
                   DisplayTopologyState::StableUnknown,
                   false, false, true, -1);
}

TEST(DisplayTopologyStateMachineTests, identicalConfigurationObservationsAreIdempotent)
{
    DisplayTopologyStateMachine machine;
    machine.observe(topology("internal"), false, 100);

    expectDecision(machine.observe(topology("internal"), false, 1000),
                   DisplayTopologyState::Reconfiguring,
                   false, false, false, 2100);
    machine.onDeadline(2100);
    expectDecision(machine.observe(topology("internal"), false, 9000),
                   DisplayTopologyState::StableUnknown,
                   false, false, true, -1);
    expectDecision(machine.onDeadline(20000),
                   DisplayTopologyState::StableUnknown,
                   false, false, true, -1);
}

TEST(DisplayTopologyStateMachineTests, beginCallbackSuspendsStableTopologyImmediately)
{
    DisplayTopologyStateMachine machine;
    machine.observe(topology("internal"), true, 0);
    machine.onDeadline(2000);

    expectDecision(machine.beginReconfiguration(3000),
                   DisplayTopologyState::Reconfiguring,
                   false, false, false, -1);
    expectDecision(machine.onDeadline(20000),
                   DisplayTopologyState::Reconfiguring,
                   false, false, false, -1);
}

TEST(DisplayTopologyStateMachineTests, duplicateReconfigurationSnapshotsResetQuietDeadline)
{
    DisplayTopologyStateMachine machine;
    machine.observe(topology("internal"), true, 0);
    machine.onDeadline(2000);

    machine.beginReconfiguration(3000);
    expectDecision(machine.observeReconfigurationSnapshot(
                       topology("internal"), true, 3100),
                   DisplayTopologyState::Reconfiguring,
                   false, false, false, 5100);
    machine.beginReconfiguration(4500);
    expectDecision(machine.observeReconfigurationSnapshot(
                       topology("internal"), true, 4600),
                   DisplayTopologyState::Reconfiguring,
                   false, false, false, 6600);
    expectDecision(machine.onDeadline(5100),
                   DisplayTopologyState::Reconfiguring,
                   false, false, false, 6600);
    expectDecision(machine.onDeadline(6600),
                   DisplayTopologyState::StableKnown,
                   true, false, true, -1);
}

TEST(DisplayTopologyStateMachineTests, configReloadCannotSettlePendingCapture)
{
    DisplayTopologyStateMachine machine;
    machine.observe(topology("internal"), false, 0);
    machine.onDeadline(2000);
    machine.beginReconfiguration(3000);

    expectDecision(machine.observe(topology("internal"), true, 3500),
                   DisplayTopologyState::Reconfiguring,
                   false, false, false, -1);
    expectDecision(machine.onDeadline(20000),
                   DisplayTopologyState::Reconfiguring,
                   false, false, false, -1);
    expectDecision(machine.observeReconfigurationSnapshot(
                       topology("internal"), true, 21000),
                   DisplayTopologyState::Reconfiguring,
                   false, false, false, 23000);
    expectDecision(machine.onDeadline(23000),
                   DisplayTopologyState::StableKnown,
                   true, false, true, -1);
}

TEST(DisplayTopologyStateMachineTests, profileAvailabilityUpdatesWithoutTopologyDelay)
{
    DisplayTopologyStateMachine machine;
    machine.observe(topology("internal"), false, 0);
    machine.onDeadline(2000);

    expectDecision(machine.observe(topology("internal"), true, 3000),
                   DisplayTopologyState::StableKnown,
                   true, false, true, -1);
}

TEST(DisplayTopologyStateMachineTests, zeroDisplayStartsGraceAndDisconnectsOnce)
{
    DisplayTopologyStateMachine machine;
    machine.observe(topology("internal"), true, 0);
    machine.onDeadline(2000);

    expectDecision(machine.observe(barrier::DisplayTopology(), false, 3000),
                   DisplayTopologyState::NoDisplayGrace,
                   false, false, false, 13000);
    expectDecision(machine.onDeadline(12999),
                   DisplayTopologyState::NoDisplayGrace,
                   false, false, false, 13000);
    expectDecision(machine.onDeadline(13000),
                   DisplayTopologyState::Unavailable,
                   false, true, false, -1);
    expectDecision(machine.onDeadline(13000),
                   DisplayTopologyState::Unavailable,
                   false, false, false, -1);
    expectDecision(machine.observe(barrier::DisplayTopology(), false, 14000),
                   DisplayTopologyState::Unavailable,
                   false, false, false, -1);
}

TEST(DisplayTopologyStateMachineTests, repeatedZeroDoesNotExtendGrace)
{
    DisplayTopologyStateMachine machine;

    machine.observe(barrier::DisplayTopology(), false, 100);
    expectDecision(machine.observe(barrier::DisplayTopology(), false, 9000),
                   DisplayTopologyState::NoDisplayGrace,
                   false, false, false, 10100);
}

TEST(DisplayTopologyStateMachineTests, callbacksDoNotExtendExistingZeroDisplayGrace)
{
    DisplayTopologyStateMachine machine;

    machine.observe(barrier::DisplayTopology(), false, 100);
    machine.beginReconfiguration(5000);
    expectDecision(machine.observeReconfigurationSnapshot(
                       barrier::DisplayTopology(), false, 5100),
                   DisplayTopologyState::NoDisplayGrace,
                   false, false, false, 10100);
}

TEST(DisplayTopologyStateMachineTests, pendingCaptureSuspendsOldZeroDisplayDeadline)
{
    DisplayTopologyStateMachine machine;

    machine.observe(barrier::DisplayTopology(), false, 100);
    expectDecision(machine.beginReconfiguration(5000),
                   DisplayTopologyState::NoDisplayGrace,
                   false, false, false, -1);
    expectDecision(machine.onDeadline(20000),
                   DisplayTopologyState::NoDisplayGrace,
                   false, false, false, -1);

    // A config reload re-observes the old empty primary snapshot. It is not a
    // completed CoreGraphics capture and must not reactivate the old timer.
    expectDecision(machine.observe(
                       barrier::DisplayTopology(), false, 20500),
                   DisplayTopologyState::NoDisplayGrace,
                   false, false, false, -1);
    expectDecision(machine.onDeadline(20500),
                   DisplayTopologyState::NoDisplayGrace,
                   false, false, false, -1);

    // Once a fresh empty snapshot confirms the display is still absent after
    // the original deadline, unavailability is applied synchronously. Never
    // return an expired deadline to EventQueue::newOneShotTimer(0).
    expectDecision(machine.observeReconfigurationSnapshot(
                       barrier::DisplayTopology(), false, 21000),
                   DisplayTopologyState::Unavailable,
                   false, true, false, -1);
}

TEST(DisplayTopologyStateMachineTests, displayReturnDuringGraceStartsNewQuietPeriod)
{
    DisplayTopologyStateMachine machine;
    machine.observe(barrier::DisplayTopology(), false, 100);

    expectDecision(machine.observe(topology("internal"), true, 5000),
                   DisplayTopologyState::Reconfiguring,
                   false, false, false, 7000);
    expectDecision(machine.onDeadline(7000),
                   DisplayTopologyState::StableKnown,
                   true, false, true, -1);
}

TEST(DisplayTopologyStateMachineTests, invalidTopologyFailsClosedIntoGrace)
{
    DisplayTopologyStateMachine machine;
    barrier::DisplayTopology invalid = topology("internal", 0);

    expectDecision(machine.observe(invalid, true, 100),
                   DisplayTopologyState::NoDisplayGrace,
                   false, false, false, 10100);
}
