/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "test/mock/server/MockPrimaryClient.h"
#include "test/mock/barrier/MockScreen.h"
#include "test/mock/io/MockStream.h"

#include "server/ClientProxy.h"
#include "server/DisplayTopologyStateMachine.h"
#include "server/Server.h"
#include "barrier/ServerArgs.h"
#include "base/ILogOutputter.h"
#include "base/Log.h"
#include "test/global/TestEventQueue.h"
#include "test/global/gtest.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;

class ServerTopologyTestAccess {
public:
    static void reset(Server& server);
    static DisplayTopologyDecision observe(
        Server& server, const barrier::DisplayTopology& topology,
        std::int64_t monotonicMs);
    static DisplayTopologyDecision deadline(
        Server& server, std::int64_t monotonicMs);
    static DisplayTopologyDecision begin(
        Server& server, std::int64_t monotonicMs);
    static DisplayTopologyDecision reconfigurationSnapshot(
        Server& server, const barrier::DisplayTopology& topology,
        std::int64_t monotonicMs);
    static bool dispatchReconfigurationStarted(
        Server& server, BaseClientProxy* client);
    static bool dispatchShapeChanged(
        Server& server, BaseClientProxy* client);
    static void removeActiveClient(
        Server& server, BaseClientProxy* client);
    static bool switchingEnabled(const Server& server);
    static DisplayTopologyState state(Server& server);
    static const std::string& status(const Server& server);
    static void movePrimaryToRightEdge(Server& server);
    static bool dispatchTopologyTimer(Server& server);
    static std::int64_t monotonicMs();
    static std::string activeName(const Server& server);
    static std::string rightEdgeDisconnectedTarget(Server& server);
    static void restorePrimaryForCleanup(Server& server);
};

void
ServerTopologyTestAccess::reset(Server& server)
{
    server.stopDisplayTopologyTimer();
    server.m_displayTopologyStateMachine = DisplayTopologyStateMachine();
    server.m_currentDisplayTopology = barrier::DisplayTopology();
    server.m_enableClipboard = false;
    server.m_switchingEnabled = false;
    server.m_lastDisplayTopologyStatus.clear();
    server.m_x = 50;
    server.m_y = 50;
    server.m_config->selectTopology(barrier::DisplayTopology());
}

DisplayTopologyDecision
ServerTopologyTestAccess::observe(
    Server& server, const barrier::DisplayTopology& topology,
    std::int64_t monotonicMs)
{
    return server.updateDisplayTopology(topology, monotonicMs);
}

DisplayTopologyDecision
ServerTopologyTestAccess::deadline(
    Server& server, std::int64_t monotonicMs)
{
    return server.updateDisplayTopologyDeadline(monotonicMs);
}

DisplayTopologyDecision
ServerTopologyTestAccess::begin(
    Server& server, std::int64_t monotonicMs)
{
    return server.beginDisplayTopologyReconfiguration(monotonicMs);
}

DisplayTopologyDecision
ServerTopologyTestAccess::reconfigurationSnapshot(
    Server& server, const barrier::DisplayTopology& topology,
    std::int64_t monotonicMs)
{
    return server.updateDisplayTopology(
        topology, monotonicMs,
        Server::DisplayTopologyUpdateSource::ReconfigurationSnapshot);
}

bool
ServerTopologyTestAccess::dispatchReconfigurationStarted(
    Server& server, BaseClientProxy* client)
{
    return server.m_events->dispatchEvent(Event(
        server.m_events->forIScreen().displayReconfigurationStarted(),
        client->getEventTarget()));
}

bool
ServerTopologyTestAccess::dispatchShapeChanged(
    Server& server, BaseClientProxy* client)
{
    return server.m_events->dispatchEvent(Event(
        server.m_events->forIScreen().shapeChanged(),
        client->getEventTarget()));
}

void
ServerTopologyTestAccess::removeActiveClient(
    Server& server, BaseClientProxy* client)
{
    server.removeActiveClient(client);
}

bool
ServerTopologyTestAccess::switchingEnabled(const Server& server)
{
    return server.m_switchingEnabled;
}

DisplayTopologyState
ServerTopologyTestAccess::state(Server& server)
{
    return server.m_displayTopologyStateMachine.onDeadline(0).state;
}

const std::string&
ServerTopologyTestAccess::status(const Server& server)
{
    return server.m_lastDisplayTopologyStatus;
}

void
ServerTopologyTestAccess::movePrimaryToRightEdge(Server& server)
{
    server.onMouseMovePrimary(99, 50);
}

bool
ServerTopologyTestAccess::dispatchTopologyTimer(Server& server)
{
    return server.m_events->dispatchEvent(
        Event(Event::kTimer, &server.m_displayTopologyStateMachine));
}

std::int64_t
ServerTopologyTestAccess::monotonicMs()
{
    return Server::displayTopologyMonotonicMs();
}

std::string
ServerTopologyTestAccess::activeName(const Server& server)
{
    return server.m_active->getName();
}

std::string
ServerTopologyTestAccess::rightEdgeDisconnectedTarget(Server& server)
{
    SInt32 x = 100;
    SInt32 y = 50;
    std::string target;
    server.mapToNeighbor(
        server.m_primaryClient, kRight, x, y, &target);
    return target;
}

void
ServerTopologyTestAccess::restorePrimaryForCleanup(Server& server)
{
    server.m_active = server.m_primaryClient;
}

namespace {

class CapturingLogOutputter : public ILogOutputter {
public:
    void open(const char*) override { }
    void close() override { }
    void show(bool) override { }
    bool write(ELevel, const char* message) override
    {
        messages.emplace_back(message);
        return true;
    }

    std::vector<std::string> messages;
};

class ScopedLogCapture {
public:
    ScopedLogCapture() : previousFilter(CLOG->getFilter())
    {
        CLOG->insert(&output, true);
    }
    ~ScopedLogCapture()
    {
        CLOG->setFilter(previousFilter);
        CLOG->remove(&output);
    }

    CapturingLogOutputter output;
    int previousFilter;
};

barrier::DisplayTopology topology(const std::string& primaryId,
                                  const std::string& secondaryId,
                                  SInt32 secondaryX,
                                  SInt32 secondaryY)
{
    barrier::DisplayTopology value;
    value.displays = {
        {primaryId, {0, 0, 100, 100}, 0, true},
        {secondaryId, {secondaryX, secondaryY, 100, 100}, 0, false}
    };
    return value.normalized();
}

Config::TopologyProfile profileFor(const barrier::DisplayTopology& value,
                                   SInt32 clientX,
                                   SInt32 clientY)
{
    Config::TopologyProfile profile;
    profile.topology = value;
    profile.key = value.profileKey();
    profile.screenPositions["server"] = {0, 0};
    profile.screenPositions["client"] = {clientX, clientY};
    profile.displayRects["server"] = {{0, 0, 100, 100}};
    profile.displayRects["client"] = {{0, 0, 100, 100}};
    return profile;
}

class TestClientProxy : public ClientProxy {
public:
    explicit TestClientProxy(const std::string& name) :
        ClientProxy(name, new NiceMock<MockStream>()),
        enterCount(0),
        displays{{0, 0, 100, 100}}
    {
    }

    void* getEventTarget() const override
    {
        return const_cast<TestClientProxy*>(this);
    }
    bool getClipboard(ClipboardID, IClipboard*) const override { return false; }
    void getShape(SInt32& x, SInt32& y, SInt32& w, SInt32& h) const override
    {
        x = 0;
        y = 0;
        w = 100;
        h = 100;
    }
    void getDisplays(std::vector<ScreenRect>& displays) const override
    {
        displays = this->displays;
    }
    void getDisplayNames(std::vector<std::string>& names) const override
    {
        names.clear();
    }
    void getCursorPos(SInt32& x, SInt32& y) const override
    {
        x = 50;
        y = 50;
    }
    void enter(SInt32, SInt32, UInt32, KeyModifierMask, bool) override
    {
        ++enterCount;
    }
    bool leave() override { return true; }
    void setClipboard(ClipboardID, const IClipboard*) override { }
    void grabClipboard(ClipboardID) override { }
    void setClipboardDirty(ClipboardID, bool) override { }
    void keyDown(KeyID, KeyModifierMask, KeyButton) override { }
    void keyRepeat(KeyID, KeyModifierMask, SInt32, KeyButton) override { }
    void keyUp(KeyID, KeyModifierMask, KeyButton) override { }
    void mouseDown(ButtonID) override { }
    void mouseUp(ButtonID) override { }
    void mouseMove(SInt32, SInt32) override { }
    void mouseRelativeMove(SInt32, SInt32) override { }
    void mouseWheel(SInt32, SInt32) override { }
    void screensaver(bool) override { }
    void resetOptions() override { }
    void setOptions(const OptionsList&) override { }
    void sendDragInfo(UInt32, const char*, size_t) override { }
    void fileChunkSending(UInt8, char*, size_t) override { }

    int enterCount;
    std::vector<ScreenRect> displays;
};

class ServerHarness {
public:
    ServerHarness() :
        config(&events),
        primary("server"),
        currentTopology(topology("internal", "external", 100, 0))
    {
        config.addScreen("server");
        config.addScreen("client");
        args.m_config = &config;

        ON_CALL(primary, getEventTarget())
            .WillByDefault(Return(&primary));
        ON_CALL(primary, getCursorPos(_, _))
            .WillByDefault(Invoke([](SInt32& x, SInt32& y) {
                x = 50;
                y = 50;
            }));
        ON_CALL(primary, getShape(_, _, _, _))
            .WillByDefault(Invoke([](SInt32& x, SInt32& y,
                                     SInt32& w, SInt32& h) {
                x = 0;
                y = 0;
                w = 100;
                h = 100;
            }));
        ON_CALL(primary, getDisplays(_))
            .WillByDefault(Invoke([](std::vector<ScreenRect>& displays) {
                displays = {{0, 0, 100, 100}};
            }));
        ON_CALL(primary, getDisplayNames(_))
            .WillByDefault(Invoke([](std::vector<std::string>& names) {
                names.clear();
            }));
        ON_CALL(primary, getDisplayTopology())
            .WillByDefault(Invoke([this]() { return currentTopology; }));
        ON_CALL(primary, getJumpZoneSize()).WillByDefault(Return(1));
        ON_CALL(primary, getCursorCenter(_, _))
            .WillByDefault(Invoke([](SInt32& x, SInt32& y) {
                x = 50;
                y = 50;
            }));
        ON_CALL(primary, isLockedToScreen()).WillByDefault(Return(false));
        ON_CALL(primary, getToggleMask()).WillByDefault(Return(0));
        ON_CALL(primary, leave()).WillByDefault(Return(true));

        server.reset(new Server(config, &primary, &screen, &events, args));
        ServerTopologyTestAccess::reset(*server);
    }

    ~ServerHarness()
    {
        if (server) {
            ServerTopologyTestAccess::restorePrimaryForCleanup(*server);
        }
    }

    TestClientProxy* connectClient()
    {
        TestClientProxy* client = new TestClientProxy("client");
        server->adoptClient(client);
        return client;
    }

    TestEventQueue events;
    Config config;
    NiceMock<MockPrimaryClient> primary;
    NiceMock<MockScreen> screen;
    ServerArgs args;
    barrier::DisplayTopology currentTopology;
    std::unique_ptr<Server> server;
};

} // namespace

TEST(ServerTests, TopologyKnownProfileEnablesEdgeTransition)
{
    ServerHarness harness;
    ASSERT_TRUE(harness.config.addTopologyProfile(
        profileFor(harness.currentTopology, 100, 0)));
    TestClientProxy* client = harness.connectClient();

    ServerTopologyTestAccess::observe(
        *harness.server, harness.currentTopology, 0);
    ServerTopologyTestAccess::deadline(*harness.server, 2000);

    EXPECT_TRUE(ServerTopologyTestAccess::switchingEnabled(*harness.server));
    EXPECT_EQ("client", harness.config.getNeighbor(
                            "server", kRight, 0.5f, nullptr));
    ServerTopologyTestAccess::movePrimaryToRightEdge(*harness.server);
    EXPECT_EQ("client", ServerTopologyTestAccess::activeName(*harness.server));
    EXPECT_EQ(1, client->enterCount);
}

TEST(ServerTests, DisconnectedNeighborIsReportedForNetworkWake)
{
    ServerHarness harness;
    ASSERT_TRUE(harness.config.addTopologyProfile(
        profileFor(harness.currentTopology, 100, 0)));
    ServerTopologyTestAccess::observe(
        *harness.server, harness.currentTopology, 0);
    ServerTopologyTestAccess::deadline(*harness.server, 2000);

    EXPECT_EQ("client",
              ServerTopologyTestAccess::rightEdgeDisconnectedTarget(
                  *harness.server));
}

TEST(ServerTests, ConnectedNeighborIsNotReportedForNetworkWake)
{
    ServerHarness harness;
    ASSERT_TRUE(harness.config.addTopologyProfile(
        profileFor(harness.currentTopology, 100, 0)));
    harness.connectClient();
    ServerTopologyTestAccess::observe(
        *harness.server, harness.currentTopology, 0);
    ServerTopologyTestAccess::deadline(*harness.server, 2000);

    EXPECT_TRUE(
        ServerTopologyTestAccess::rightEdgeDisconnectedTarget(
            *harness.server).empty());
}

TEST(ServerTests, EdgeIntentEmitsOneWakeRequestAtErrorLogLevel)
{
    ServerHarness harness;
    ASSERT_TRUE(harness.config.addTopologyProfile(
        profileFor(harness.currentTopology, 100, 0)));
    ServerTopologyTestAccess::observe(
        *harness.server, harness.currentTopology, 0);
    ServerTopologyTestAccess::deadline(*harness.server, 2000);

    ScopedLogCapture capture;
    CLOG->setFilter(kERROR);
    ServerTopologyTestAccess::movePrimaryToRightEdge(*harness.server);
    ServerTopologyTestAccess::movePrimaryToRightEdge(*harness.server);

    int wakeRequests = 0;
    for (const std::string& message : capture.output.messages) {
        if (message == "BARRIER_WAKE\tv=1\ttarget=636c69656e74") {
            ++wakeRequests;
        }
    }
    EXPECT_EQ(1, wakeRequests);
}

TEST(ServerTests, ClientConnectionConfirmationSurvivesErrorLogLevel)
{
    ServerHarness harness;
    ScopedLogCapture capture;
    CLOG->setFilter(kERROR);

    harness.connectClient();

    EXPECT_NE(capture.output.messages.end(),
              std::find(capture.output.messages.begin(),
                        capture.output.messages.end(),
                        "client \"client\" has connected"));
}

TEST(ServerTests, TopologyUnknownKeepsSessionAndBlocksSwitching)
{
    ServerHarness harness;
    harness.connectClient();

    ServerTopologyTestAccess::observe(
        *harness.server, harness.currentTopology, 0);
    ServerTopologyTestAccess::deadline(*harness.server, 2000);

    EXPECT_EQ(2u, harness.server->getNumClients());
    EXPECT_FALSE(ServerTopologyTestAccess::switchingEnabled(*harness.server));
    ServerTopologyTestAccess::movePrimaryToRightEdge(*harness.server);
    EXPECT_EQ("server", ServerTopologyTestAccess::activeName(*harness.server));
    EXPECT_NE(std::string::npos,
              ServerTopologyTestAccess::status(*harness.server)
                  .find("BARRIER_TOPOLOGY\tstate=StableUnknown\t"));
}

TEST(ServerTests, TopologySecondKnownProfileReplacesLinks)
{
    ServerHarness harness;
    const barrier::DisplayTopology vertical =
        topology("internal", "external", 0, 100);
    ASSERT_TRUE(harness.config.addTopologyProfile(
        profileFor(harness.currentTopology, 100, 0)));
    ASSERT_TRUE(harness.config.addTopologyProfile(profileFor(vertical, 0, 100)));

    ServerTopologyTestAccess::observe(
        *harness.server, harness.currentTopology, 0);
    ServerTopologyTestAccess::deadline(*harness.server, 2000);
    ServerTopologyTestAccess::observe(*harness.server, vertical, 3000);
    ServerTopologyTestAccess::deadline(*harness.server, 5000);

    EXPECT_TRUE(harness.config.getNeighbor(
                    "server", kRight, 0.5f, nullptr).empty());
    EXPECT_EQ("client", harness.config.getNeighbor(
                            "server", kBottom, 0.5f, nullptr));
}

TEST(ServerTests, TopologyZeroDisplayDisconnectsOnlyAfterGrace)
{
    ServerHarness harness;
    harness.connectClient();

    ServerTopologyTestAccess::observe(
        *harness.server, barrier::DisplayTopology(), 100);
    EXPECT_FALSE(ServerTopologyTestAccess::switchingEnabled(*harness.server));
    EXPECT_EQ(2u, harness.server->getNumClients());

    ServerTopologyTestAccess::deadline(*harness.server, 10099);
    EXPECT_EQ(2u, harness.server->getNumClients());
    ServerTopologyTestAccess::deadline(*harness.server, 10100);
    EXPECT_EQ(1u, harness.server->getNumClients());
    ServerTopologyTestAccess::deadline(*harness.server, 20000);
    EXPECT_EQ(1u, harness.server->getNumClients());
    EXPECT_EQ("BARRIER_TOPOLOGY\tstate=Unavailable\tkey=-\tdisplays=",
              ServerTopologyTestAccess::status(*harness.server));
}

TEST(ServerTests, TopologyLateEmptyCaptureDisconnectsWithoutZeroTimer)
{
    ServerHarness harness;
    harness.connectClient();

    ServerTopologyTestAccess::observe(
        *harness.server, barrier::DisplayTopology(), 100);
    ServerTopologyTestAccess::begin(*harness.server, 5000);
    EXPECT_EQ(2u, harness.server->getNumClients());

    const DisplayTopologyDecision decision =
        ServerTopologyTestAccess::reconfigurationSnapshot(
            *harness.server, barrier::DisplayTopology(), 21000);
    EXPECT_EQ(DisplayTopologyState::Unavailable, decision.state);
    EXPECT_TRUE(decision.disconnectClients);
    EXPECT_EQ(-1, decision.nextDeadlineMs);
    EXPECT_EQ(1u, harness.server->getNumClients());
}

TEST(ServerTests, TopologyTimerEventAppliesExpiredDeadline)
{
    ServerHarness harness;
    harness.connectClient();
    const std::int64_t now = ServerTopologyTestAccess::monotonicMs();
    ServerTopologyTestAccess::observe(
        *harness.server, barrier::DisplayTopology(), now - 10000);

    EXPECT_TRUE(
        ServerTopologyTestAccess::dispatchTopologyTimer(*harness.server));
    EXPECT_EQ(1u, harness.server->getNumClients());
    EXPECT_EQ("BARRIER_TOPOLOGY\tstate=Unavailable\tkey=-\tdisplays=",
              ServerTopologyTestAccess::status(*harness.server));
}

TEST(ServerTests, TopologyReturnBeforeGraceKeepsSession)
{
    ServerHarness harness;
    ASSERT_TRUE(harness.config.addTopologyProfile(
        profileFor(harness.currentTopology, 100, 0)));
    harness.connectClient();

    ServerTopologyTestAccess::observe(
        *harness.server, barrier::DisplayTopology(), 100);
    ServerTopologyTestAccess::observe(
        *harness.server, harness.currentTopology, 5000);
    ServerTopologyTestAccess::deadline(*harness.server, 7000);

    EXPECT_EQ(2u, harness.server->getNumClients());
    EXPECT_TRUE(ServerTopologyTestAccess::switchingEnabled(*harness.server));
}

TEST(ServerTests, TopologyConfigReloadActivatesCurrentUnknownProfile)
{
    ServerHarness harness;
    harness.connectClient();
    ServerTopologyTestAccess::observe(
        *harness.server, harness.currentTopology, 0);
    ServerTopologyTestAccess::deadline(*harness.server, 2000);
    ASSERT_EQ(DisplayTopologyState::StableUnknown,
              ServerTopologyTestAccess::state(*harness.server));

    ASSERT_TRUE(harness.config.addTopologyProfile(
        profileFor(harness.currentTopology, 100, 0)));
    ASSERT_TRUE(harness.server->setConfig(harness.config));

    EXPECT_EQ(DisplayTopologyState::StableKnown,
              ServerTopologyTestAccess::state(*harness.server));
    EXPECT_TRUE(ServerTopologyTestAccess::switchingEnabled(*harness.server));
    EXPECT_EQ(2u, harness.server->getNumClients());
}

TEST(ServerTests, TopologyBeginEventImmediatelySuspendsKnownLayout)
{
    ServerHarness harness;
    ASSERT_TRUE(harness.config.addTopologyProfile(
        profileFor(harness.currentTopology, 100, 0)));
    harness.connectClient();
    ServerTopologyTestAccess::observe(
        *harness.server, harness.currentTopology, 0);
    ServerTopologyTestAccess::deadline(*harness.server, 2000);
    ASSERT_TRUE(ServerTopologyTestAccess::switchingEnabled(*harness.server));

    EXPECT_TRUE(ServerTopologyTestAccess::dispatchReconfigurationStarted(
        *harness.server, &harness.primary));
    EXPECT_EQ(DisplayTopologyState::Reconfiguring,
              ServerTopologyTestAccess::state(*harness.server));
    EXPECT_FALSE(ServerTopologyTestAccess::switchingEnabled(*harness.server));
    EXPECT_TRUE(harness.config.getNeighbor(
                    "server", kRight, 0.5f, nullptr).empty());
}

TEST(ServerTests, TopologyPrimaryEmptyShapeEventEntersNoDisplayGrace)
{
    ServerHarness harness;
    harness.connectClient();
    ServerTopologyTestAccess::observe(
        *harness.server, harness.currentTopology, 0);
    ServerTopologyTestAccess::deadline(*harness.server, 2000);

    ASSERT_TRUE(ServerTopologyTestAccess::dispatchReconfigurationStarted(
        *harness.server, &harness.primary));
    harness.currentTopology = barrier::DisplayTopology();
    EXPECT_TRUE(ServerTopologyTestAccess::dispatchShapeChanged(
        *harness.server, &harness.primary));
    EXPECT_EQ(DisplayTopologyState::NoDisplayGrace,
              ServerTopologyTestAccess::state(*harness.server));
    EXPECT_FALSE(ServerTopologyTestAccess::switchingEnabled(*harness.server));
    EXPECT_EQ(2u, harness.server->getNumClients());
}

TEST(ServerTests, TopologyClientShapeChangeRebuildsLinksFromLiveDisplays)
{
    ServerHarness harness;
    ASSERT_TRUE(harness.config.addTopologyProfile(
        profileFor(harness.currentTopology, 100, 0)));
    TestClientProxy* client = harness.connectClient();
    ServerTopologyTestAccess::observe(
        *harness.server, harness.currentTopology, 0);
    ServerTopologyTestAccess::deadline(*harness.server, 2000);
    ASSERT_EQ("client", harness.config.getNeighbor(
                            "server", kRight, 0.8f, nullptr));

    client->displays = {{0, 0, 100, 60}};
    ASSERT_TRUE(ServerTopologyTestAccess::dispatchShapeChanged(
        *harness.server, client));

    EXPECT_TRUE(ServerTopologyTestAccess::switchingEnabled(*harness.server));
    EXPECT_EQ("client", harness.config.getNeighbor(
                            "server", kRight, 0.5f, nullptr));
    EXPECT_TRUE(harness.config.getNeighbor(
                    "server", kRight, 0.8f, nullptr).empty());
}

TEST(ServerTests, TopologyClientArrivalAppliesInitialLiveDisplays)
{
    ServerHarness harness;
    ASSERT_TRUE(harness.config.addTopologyProfile(
        profileFor(harness.currentTopology, 100, 0)));
    ServerTopologyTestAccess::observe(
        *harness.server, harness.currentTopology, 0);
    ServerTopologyTestAccess::deadline(*harness.server, 2000);
    ASSERT_TRUE(ServerTopologyTestAccess::switchingEnabled(*harness.server));

    TestClientProxy* client = new TestClientProxy("client");
    client->displays.clear();
    harness.server->adoptClient(client);

    EXPECT_FALSE(ServerTopologyTestAccess::switchingEnabled(*harness.server));
    EXPECT_NE(std::string::npos,
              ServerTopologyTestAccess::status(*harness.server)
                  .find("BARRIER_TOPOLOGY\tstate=StableUnknown\t"));
}

TEST(ServerTests, TopologyClientRemovalDropsInvalidLiveDisplayOverride)
{
    ServerHarness harness;
    ASSERT_TRUE(harness.config.addTopologyProfile(
        profileFor(harness.currentTopology, 100, 0)));
    TestClientProxy* client = harness.connectClient();
    ServerTopologyTestAccess::observe(
        *harness.server, harness.currentTopology, 0);
    ServerTopologyTestAccess::deadline(*harness.server, 2000);
    ASSERT_TRUE(ServerTopologyTestAccess::switchingEnabled(*harness.server));

    client->displays.clear();
    ASSERT_TRUE(ServerTopologyTestAccess::dispatchShapeChanged(
        *harness.server, client));
    ASSERT_FALSE(ServerTopologyTestAccess::switchingEnabled(*harness.server));
    ASSERT_NE(std::string::npos,
              ServerTopologyTestAccess::status(*harness.server)
                  .find("BARRIER_TOPOLOGY\tstate=StableUnknown\t"));

    ServerTopologyTestAccess::removeActiveClient(*harness.server, client);
    delete client;

    EXPECT_TRUE(ServerTopologyTestAccess::switchingEnabled(*harness.server));
    EXPECT_EQ("client", harness.config.getNeighbor(
                            "server", kRight, 0.5f, nullptr));
    EXPECT_NE(std::string::npos,
              ServerTopologyTestAccess::status(*harness.server)
                  .find("BARRIER_TOPOLOGY\tstate=StableKnown\t"));
}
