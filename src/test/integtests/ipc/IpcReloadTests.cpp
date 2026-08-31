/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#define BARRIER_TEST_ENV

#include "test/global/TestEventQueue.h"
#include "ipc/Ipc.h"
#include "ipc/IpcClient.h"
#include "ipc/IpcMessage.h"
#include "ipc/IpcServer.h"
#include "base/EventQueue.h"
#include "base/TMethodEventJob.h"
#include "net/SocketMultiplexer.h"
#include "test/global/gtest.h"

#include <cstring>

namespace {

TEST(IpcReloadProtocolTests, WireMessageIsExactFourByteIrld)
{
    EXPECT_EQ(4u, std::strlen(kIpcMsgReload));
    EXPECT_STREQ("IRLD", kIpcMsgReload);
    EXPECT_EQ(kIpcReload, IpcReloadMessage().type());
}

#ifndef WINAPI_CARBON

const UInt16 kReloadTestPort = 24812;

class IpcReloadTests : public ::testing::Test
{
public:
    IpcReloadTests() :
        m_client(nullptr),
        m_server(nullptr),
        m_received(false)
    {
    }

    void handleServerMessage(const Event& event, void*)
    {
        IpcMessage* message = static_cast<IpcMessage*>(event.getDataObject());
        if (message->type() == kIpcHello) {
            IpcReloadMessage reload;
            m_client->send(reload);
        }
        else if (message->type() == kIpcReload) {
            m_received = true;
            m_events.raiseQuitEvent();
        }
    }

    void handleServerHello(const Event& event, void*)
    {
        IpcMessage* message = static_cast<IpcMessage*>(event.getDataObject());
        if (message->type() == kIpcHello) {
            IpcReloadMessage reload;
            m_server->send(reload, kIpcClientNode);
        }
    }

    void handleClientMessage(const Event& event, void*)
    {
        IpcMessage* message = static_cast<IpcMessage*>(event.getDataObject());
        if (message->type() == kIpcReload) {
            m_received = true;
            m_events.raiseQuitEvent();
        }
    }

    TestEventQueue m_events;
    IpcClient* m_client;
    IpcServer* m_server;
    bool m_received;
};


TEST_F(IpcReloadTests, GuiReloadReachesDaemon)
{
    SocketMultiplexer socketMultiplexer;
    IpcServer server(&m_events, &socketMultiplexer, kReloadTestPort);
    server.listen();
    m_events.adoptHandler(
        m_events.forIpcServer().messageReceived(), &server,
        new TMethodEventJob<IpcReloadTests>(
            this, &IpcReloadTests::handleServerMessage));

    IpcClient client(
        &m_events, &socketMultiplexer, kReloadTestPort, kIpcClientGui);
    m_client = &client;
    client.connect();

    m_events.initQuitTimeout(5);
    m_events.loop();
    m_events.removeHandler(m_events.forIpcServer().messageReceived(), &server);
    m_events.cleanupQuitTimeout();

    EXPECT_TRUE(m_received);
}

TEST_F(IpcReloadTests, DaemonReloadReachesOwnedNode)
{
    SocketMultiplexer socketMultiplexer;
    IpcServer server(&m_events, &socketMultiplexer, kReloadTestPort);
    server.listen();
    m_server = &server;
    m_events.adoptHandler(
        m_events.forIpcServer().messageReceived(), &server,
        new TMethodEventJob<IpcReloadTests>(
            this, &IpcReloadTests::handleServerHello));

    IpcClient client(&m_events, &socketMultiplexer, kReloadTestPort);
    client.connect();
    m_events.adoptHandler(
        m_events.forIpcClient().messageReceived(), &client,
        new TMethodEventJob<IpcReloadTests>(
            this, &IpcReloadTests::handleClientMessage));

    m_events.initQuitTimeout(5);
    m_events.loop();
    m_events.removeHandler(m_events.forIpcServer().messageReceived(), &server);
    m_events.removeHandler(m_events.forIpcClient().messageReceived(), &client);
    m_events.cleanupQuitTimeout();

    EXPECT_TRUE(m_received);
}

#endif // WINAPI_CARBON

} // namespace
