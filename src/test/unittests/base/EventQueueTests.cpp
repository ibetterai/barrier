/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "base/EventQueue.h"
#include "base/SimpleEventQueueBuffer.h"
#include "base/TMethodEventJob.h"
#include "mt/Lock.h"
#include "mt/Thread.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace barrier_test {

class EventQueueTestAccess {
public:
    static UInt32 readyWaiterCount(const EventQueue& events)
    {
        Lock lock(events.m_readyMutex);
        return events.m_readyWaiterCount;
    }
};

bool waitForThreadOrCancel(Thread& thread, double timeout)
{
    if (thread.wait(timeout)) {
        return true;
    }

    thread.cancel();
    thread.wait();
    return false;
}

bool waitForReadyWaiters(const EventQueue& events, UInt32 expected,
    std::chrono::seconds timeout)
{
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + timeout;
    do {
        if (EventQueueTestAccess::readyWaiterCount(events) >= expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);

    return EventQueueTestAccess::readyWaiterCount(events) >= expected;
}

class BlockingFirstAddBuffer : public SimpleEventQueueBuffer {
public:
    BlockingFirstAddBuffer() :
        m_firstAddEntered(false),
        m_releaseFirstAdd(false)
    {
    }

    bool addEvent(UInt32 dataID) override
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (!m_firstAddEntered) {
                m_firstAddEntered = true;
                m_condition.notify_all();
                m_condition.wait(lock, [this] { return m_releaseFirstAdd; });
            }
        }
        return SimpleEventQueueBuffer::addEvent(dataID);
    }

    void waitForFirstAdd()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_condition.wait(lock, [this] { return m_firstAddEntered; });
    }

    void releaseFirstAdd()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_releaseFirstAdd = true;
        m_condition.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_firstAddEntered;
    bool m_releaseFirstAdd;
};

class EventQueueStartupTests : public ::testing::Test {
public:
    EventQueueStartupTests() :
        m_eventType(Event::kUnknown),
        m_target(0),
        m_delivered(false)
    {
        m_eventType = m_events.registerTypeOnce(
            m_eventType, "eventQueueStartupTest");
        m_events.adoptHandler(
            m_eventType, &m_target,
            new TMethodEventJob<EventQueueStartupTests>(
                this, &EventQueueStartupTests::handleEvent));
    }

    ~EventQueueStartupTests() override
    {
        m_events.removeHandler(m_eventType, &m_target);
    }

    void handleEvent(const Event&, void*)
    {
        m_delivered = true;
        m_events.addEvent(Event(Event::kQuit));
    }

protected:
    EventQueue m_events;
    Event::Type m_eventType;
    char m_target;
    bool m_delivered;
};

TEST_F(EventQueueStartupTests, DeliversEventQueuedBeforeLoopBecomesReady)
{
    m_events.addEvent(Event(m_eventType, &m_target));

    m_events.loop();

    EXPECT_TRUE(m_delivered);
}

TEST_F(EventQueueStartupTests, BecomesReadyOnlyAfterPendingEventsReachBuffer)
{
    BlockingFirstAddBuffer* buffer = new BlockingFirstAddBuffer();
    m_events.adoptBuffer(buffer);
    m_events.addEvent(Event(m_eventType, &m_target));

    std::mutex readyMutex;
    std::condition_variable readyCondition;
    bool waitForReadyReturned = false;
    Thread readyThread([this, &readyMutex, &readyCondition,
        &waitForReadyReturned] {
        m_events.waitForReady();
        std::lock_guard<std::mutex> lock(readyMutex);
        waitForReadyReturned = true;
        readyCondition.notify_all();
    });
    Thread loopThread([this] { m_events.loop(); });

    buffer->waitForFirstAdd();
    {
        std::unique_lock<std::mutex> lock(readyMutex);
        EXPECT_FALSE(readyCondition.wait_for(
            lock, std::chrono::milliseconds(50),
            [&waitForReadyReturned] { return waitForReadyReturned; }));
    }

    buffer->releaseFirstAdd();
    {
        std::unique_lock<std::mutex> lock(readyMutex);
        EXPECT_TRUE(readyCondition.wait_for(
            lock, std::chrono::seconds(10),
            [&waitForReadyReturned] { return waitForReadyReturned; }));
    }
    EXPECT_TRUE(waitForThreadOrCancel(readyThread, 10.0));
    EXPECT_TRUE(waitForThreadOrCancel(loopThread, 10.0));

    EXPECT_TRUE(m_delivered);
}

TEST_F(EventQueueStartupTests, WakesAllCallersAlreadyWaitingForReady)
{
    std::mutex returnedMutex;
    std::condition_variable returnedCondition;
    UInt32 returnedCount = 0;
    const std::function<void()> waitForReady = [this, &returnedMutex,
        &returnedCondition, &returnedCount] {
        m_events.waitForReady();
        std::lock_guard<std::mutex> lock(returnedMutex);
        ++returnedCount;
        returnedCondition.notify_all();
    };

    Thread firstWaiter(waitForReady);
    Thread secondWaiter(waitForReady);
    EXPECT_TRUE(waitForReadyWaiters(
        m_events, 2, std::chrono::seconds(10)));

    m_events.addEvent(Event(Event::kQuit));
    Thread loopThread([this] { m_events.loop(); });

    {
        std::unique_lock<std::mutex> lock(returnedMutex);
        EXPECT_TRUE(returnedCondition.wait_for(
            lock, std::chrono::seconds(10),
            [&returnedCount] { return returnedCount == 2; }));
    }

    EXPECT_TRUE(waitForThreadOrCancel(firstWaiter, 10.0));
    EXPECT_TRUE(waitForThreadOrCancel(secondWaiter, 10.0));
    EXPECT_TRUE(waitForThreadOrCancel(loopThread, 10.0));
}

} // namespace barrier_test
