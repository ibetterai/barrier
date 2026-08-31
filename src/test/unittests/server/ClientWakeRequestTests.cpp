/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "server/ClientWakeRequest.h"

#include "test/global/gtest.h"

#include <limits>
#include <string>

namespace {

using barrier::ClientWakeRequestParseResult;
using barrier::ClientWakeRequestTracker;
using barrier::formatClientWakeRequest;
using barrier::parseClientWakeRequest;

TEST(ClientWakeRequestTests, formatsCanonicalLowerCaseHexPayload)
{
    std::string payload;

    ASSERT_TRUE(formatClientWakeRequest("client-alpha", payload));
    EXPECT_EQ("BARRIER_WAKE\tv=1\ttarget=636c69656e742d616c706861", payload);
}

TEST(ClientWakeRequestTests, roundTripsUtf8TargetInRawControlLine)
{
    const std::string original = u8"客户端-m1";
    std::string payload;
    ASSERT_TRUE(formatClientWakeRequest(original, payload));

    std::string decoded("unchanged until success");
    EXPECT_EQ(ClientWakeRequestParseResult::Valid,
              parseClientWakeRequest(payload, decoded));
    EXPECT_EQ(original, decoded);
}

TEST(ClientWakeRequestTests, acceptsMaximumTargetAndRejectsInvalidSizes)
{
    std::string payload("sentinel");
    ASSERT_TRUE(formatClientWakeRequest(
        std::string(barrier::kClientWakeMaxTargetBytes, 'x'), payload));
    const std::string maximumPayload = payload;

    EXPECT_FALSE(formatClientWakeRequest(std::string(), payload));
    EXPECT_EQ(maximumPayload, payload);

    EXPECT_FALSE(formatClientWakeRequest(
        std::string(barrier::kClientWakeMaxTargetBytes + 1, 'x'), payload));
    EXPECT_EQ(maximumPayload, payload);
}

TEST(ClientWakeRequestTests, returnsNotWakeWithoutChangingOutput)
{
    std::string target("sentinel");

    EXPECT_EQ(ClientWakeRequestParseResult::NotWake,
              parseClientWakeRequest("[timestamp] ordinary Barrier log", target));
    EXPECT_EQ("sentinel", target);
}

TEST(ClientWakeRequestTests, invalidPayloadsDoNotChangeOutput)
{
    const std::string invalidLines[] = {
        "BARRIER_WAKE\tv=2\ttarget=6d31",
        "BARRIER_WAKE\tv=1\ttarget=",
        "BARRIER_WAKE\tv=1\ttarget=6d3",
        "BARRIER_WAKE\tv=1\ttarget=6D31",
        "BARRIER_WAKE\tv=1\ttarget=6x31",
        "BARRIER_WAKE\tv=1\ttarget=6d31\textra=1",
        "BARRIER_WAKE\tv=1\ttarget=6d31 BARRIER_WAKE\tv=1\ttarget=6d31",
        "[timestamp] BARRIER_WAKE\tv=1\ttarget=6d31"
    };

    for (const std::string& line : invalidLines) {
        std::string target("sentinel");
        EXPECT_EQ(ClientWakeRequestParseResult::Invalid,
                  parseClientWakeRequest(line, target)) << line;
        EXPECT_EQ("sentinel", target) << line;
    }
}

TEST(ClientWakeRequestTests, rejectsOversizedTargetAndBoundedLine)
{
    std::string target("sentinel");
    const std::string oversizedTarget =
        std::string("BARRIER_WAKE\tv=1\ttarget=") +
        std::string((barrier::kClientWakeMaxTargetBytes + 1) * 2, '6');
    EXPECT_EQ(ClientWakeRequestParseResult::Invalid,
              parseClientWakeRequest(oversizedTarget, target));
    EXPECT_EQ("sentinel", target);

    const std::string oversizedLine(barrier::kClientWakeMaxLineBytes + 1, 'x');
    EXPECT_EQ(ClientWakeRequestParseResult::Invalid,
              parseClientWakeRequest(oversizedLine, target));
    EXPECT_EQ("sentinel", target);
}

TEST(ClientWakeRequestTests, acceptsCrLfTerminatedPayload)
{
    std::string target;

    EXPECT_EQ(ClientWakeRequestParseResult::Valid,
              parseClientWakeRequest(
                  "BARRIER_WAKE\tv=1\ttarget=636c69656e742d616c706861\r\n",
                  target));
    EXPECT_EQ("client-alpha", target);
}

TEST(ClientWakeRequestTrackerTests, allowsFirstAndExactFifteenSecondRequest)
{
    ClientWakeRequestTracker tracker;

    EXPECT_TRUE(tracker.shouldEmit("client-alpha", 100.0));
    EXPECT_FALSE(tracker.shouldEmit("client-alpha", 114.999));
    EXPECT_TRUE(tracker.shouldEmit("client-alpha", 115.0));
    EXPECT_FALSE(tracker.shouldEmit("client-alpha", 129.999));
    EXPECT_TRUE(tracker.shouldEmit("client-alpha", 130.0));
}

TEST(ClientWakeRequestTrackerTests, tracksTargetsIndependently)
{
    ClientWakeRequestTracker tracker;

    EXPECT_TRUE(tracker.shouldEmit("client-alpha", 10.0));
    EXPECT_TRUE(tracker.shouldEmit("client-beta", 10.1));
    EXPECT_FALSE(tracker.shouldEmit("client-alpha", 24.9));
    EXPECT_TRUE(tracker.shouldEmit("client-beta", 25.1));
    EXPECT_TRUE(tracker.shouldEmit("client-alpha", 25.0));
}

TEST(ClientWakeRequestTrackerTests, rejectsInvalidTargetAndTime)
{
    ClientWakeRequestTracker tracker;

    EXPECT_FALSE(tracker.shouldEmit(std::string(), 0.0));
    EXPECT_FALSE(tracker.shouldEmit(
        std::string(barrier::kClientWakeMaxTargetBytes + 1, 'x'), 0.0));
    EXPECT_FALSE(tracker.shouldEmit("client-alpha", -1.0));
    EXPECT_FALSE(tracker.shouldEmit(
        "client-alpha", std::numeric_limits<double>::quiet_NaN()));
    EXPECT_FALSE(tracker.shouldEmit(
        "client-alpha", std::numeric_limits<double>::infinity()));
    EXPECT_FALSE(tracker.shouldEmit(
        "client-alpha", -std::numeric_limits<double>::infinity()));

    // Invalid attempts do not consume the first valid request.
    EXPECT_TRUE(tracker.shouldEmit("client-alpha", 0.0));
}

TEST(ClientWakeRequestTrackerTests, rejectsBackwardTimeWithoutResettingCooldown)
{
    ClientWakeRequestTracker tracker;

    EXPECT_TRUE(tracker.shouldEmit("client-alpha", 100.0));
    EXPECT_FALSE(tracker.shouldEmit("client-alpha", 99.0));
    EXPECT_FALSE(tracker.shouldEmit("client-alpha", 114.0));
    EXPECT_TRUE(tracker.shouldEmit("client-alpha", 115.0));
}

TEST(ClientWakeRequestTrackerTests, confirmedConnectionClearsOnlyThatTarget)
{
    ClientWakeRequestTracker tracker;
    ASSERT_TRUE(tracker.shouldEmit("client-alpha", 100.0));
    ASSERT_TRUE(tracker.shouldEmit("client-beta", 100.0));

    tracker.confirmConnected("client-alpha");

    EXPECT_TRUE(tracker.shouldEmit("client-alpha", 100.1));
    EXPECT_FALSE(tracker.shouldEmit("client-beta", 100.1));
}

} // namespace
