/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "TopologyStatusParser.h"

#include "barrier/DisplayTopology.h"
#include "gtest/gtest.h"

namespace {

barrier::DisplayTopology singleDisplayTopology()
{
    barrier::DisplayTopology topology;
    topology.displays.push_back({
        "internal", {0, 0, 1920, 1080}, 0, true
    });
    return topology;
}

QString statusLine(const QString& state,
                   const barrier::DisplayTopology& topology)
{
    return QStringLiteral("[2026-08-29] NOTE: BARRIER_TOPOLOGY\tstate=%1\tkey=%2\tdisplays=%3")
        .arg(state,
             QString::fromStdString(topology.profileKey()),
             QString::fromStdString(topology.canonicalIdentity()));
}

QString unavailableLine(const QString& state)
{
    return QStringLiteral("BARRIER_TOPOLOGY\tstate=%1\tkey=-\tdisplays=")
        .arg(state);
}

} // namespace

TEST(TopologyStatusParserTests, AcceptsEveryProtocolState)
{
    const barrier::DisplayTopology topology = singleDisplayTopology();
    const struct {
        const char* name;
        barrier::TopologyStatusState state;
        bool ready;
        bool hasTopology;
    } cases[] = {
        {"Reconfiguring", barrier::TopologyStatusState::Reconfiguring, false, true},
        {"StableKnown", barrier::TopologyStatusState::StableKnown, true, true},
        {"StableUnknown", barrier::TopologyStatusState::StableUnknown, true, true},
        {"NoDisplayGrace", barrier::TopologyStatusState::NoDisplayGrace, false, false},
        {"Unavailable", barrier::TopologyStatusState::Unavailable, false, false},
    };

    for (const auto& testCase : cases) {
        barrier::TopologyStatus status;
        const QString line = testCase.hasTopology
            ? statusLine(QString::fromLatin1(testCase.name), topology)
            : unavailableLine(QString::fromLatin1(testCase.name));
        EXPECT_EQ(barrier::TopologyStatusParseResult::Valid,
                  barrier::TopologyStatusParser::parse(line, status))
            << testCase.name;
        EXPECT_EQ(testCase.state, status.state) << testCase.name;
        EXPECT_EQ(testCase.ready, status.displayReady()) << testCase.name;
        EXPECT_EQ(testCase.hasTopology, !status.topology.empty()) << testCase.name;
    }
}

TEST(TopologyStatusParserTests, DistinguishesOrdinaryLogLines)
{
    barrier::TopologyStatus status;
    EXPECT_EQ(barrier::TopologyStatusParseResult::NotTopology,
              barrier::TopologyStatusParser::parse(
                  QStringLiteral("NOTE: server started"), status));
}

TEST(TopologyStatusParserTests, RejectsMissingDuplicateAndUnknownFields)
{
    barrier::TopologyStatus status;
    EXPECT_EQ(barrier::TopologyStatusParseResult::Invalid,
              barrier::TopologyStatusParser::parse(
                  QStringLiteral("BARRIER_TOPOLOGY\tstate=Unavailable\tkey=-"), status));
    EXPECT_EQ(barrier::TopologyStatusParseResult::Invalid,
              barrier::TopologyStatusParser::parse(
                  QStringLiteral("BARRIER_TOPOLOGY\tstate=Unavailable\tstate=Unavailable\tkey=-\tdisplays="), status));
    EXPECT_EQ(barrier::TopologyStatusParseResult::Invalid,
              barrier::TopologyStatusParser::parse(
                  QStringLiteral("BARRIER_TOPOLOGY\tstate=Unavailable\tkey=-\tdisplays=\textra=1"), status));
}

TEST(TopologyStatusParserTests, RejectsInvalidStateAndKey)
{
    const barrier::DisplayTopology topology = singleDisplayTopology();
    barrier::TopologyStatus status;
    EXPECT_EQ(barrier::TopologyStatusParseResult::Invalid,
              barrier::TopologyStatusParser::parse(
                  statusLine(QStringLiteral("Ready"), topology), status));

    QString line = statusLine(QStringLiteral("StableKnown"), topology);
    line.replace(QString::fromStdString(topology.profileKey()),
                 QStringLiteral("not-a-key"));
    EXPECT_EQ(barrier::TopologyStatusParseResult::Invalid,
              barrier::TopologyStatusParser::parse(line, status));
}

TEST(TopologyStatusParserTests, RejectsDuplicateDisplayIds)
{
    const QString identity = QStringLiteral(
        "display-topology-v1|2|1:61,0,0,100,100,0,1|1:61,100,0,100,100,0,0");
    const QString line = QStringLiteral(
        "BARRIER_TOPOLOGY\tstate=StableKnown\tkey=%1\tdisplays=%2")
        .arg(QString(64, QLatin1Char('0')), identity);
    barrier::TopologyStatus status;
    EXPECT_EQ(barrier::TopologyStatusParseResult::Invalid,
              barrier::TopologyStatusParser::parse(line, status));
}

TEST(TopologyStatusParserTests, RejectsOversizedMalformedAndImpossibleGeometry)
{
    barrier::TopologyStatus status;
    EXPECT_EQ(barrier::TopologyStatusParseResult::Invalid,
              barrier::TopologyStatusParser::parse(
                  QStringLiteral("BARRIER_TOPOLOGY\t") +
                      QString(17000, QLatin1Char('x')), status));

    const QString badInteger = QStringLiteral(
        "BARRIER_TOPOLOGY\tstate=StableKnown\tkey=%1\tdisplays="
        "display-topology-v1|1|1:61,+0,0,100,100,0,1")
        .arg(QString(64, QLatin1Char('0')));
    EXPECT_EQ(barrier::TopologyStatusParseResult::Invalid,
              barrier::TopologyStatusParser::parse(badInteger, status));

    const QString badGeometry = QStringLiteral(
        "BARRIER_TOPOLOGY\tstate=StableKnown\tkey=%1\tdisplays="
        "display-topology-v1|1|1:61,0,0,0,100,0,1")
        .arg(QString(64, QLatin1Char('0')));
    EXPECT_EQ(barrier::TopologyStatusParseResult::Invalid,
              barrier::TopologyStatusParser::parse(badGeometry, status));
}

TEST(TopologyStatusParserTests, RejectsMismatchedKeyWithoutChangingOutput)
{
    const barrier::DisplayTopology topology = singleDisplayTopology();
    barrier::TopologyStatus status;
    ASSERT_EQ(barrier::TopologyStatusParseResult::Valid,
              barrier::TopologyStatusParser::parse(
                  statusLine(QStringLiteral("StableKnown"), topology), status));
    const QString previousKey = status.key;
    const std::string previousIdentity = status.topology.canonicalIdentity();

    QString invalid = statusLine(QStringLiteral("StableUnknown"), topology);
    invalid.replace(QString::fromStdString(topology.profileKey()),
                    QString(64, QLatin1Char('0')));
    QString error;
    EXPECT_EQ(barrier::TopologyStatusParseResult::Invalid,
              barrier::TopologyStatusParser::parse(invalid, status, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(previousKey, status.key);
    EXPECT_EQ(previousIdentity, status.topology.canonicalIdentity());
}
