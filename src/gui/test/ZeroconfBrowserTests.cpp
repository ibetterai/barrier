/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "../src/ZeroconfBrowser.h"

#include <QtCore/QPointer>
#include <QtCore/QtEndian>

#include <gtest/gtest.h>

namespace {

ZeroconfRecord resolvedRecord(const QString& host,
                              const QByteArray& displayReady)
{
    ZeroconfRecord record(QStringLiteral("Desk"),
                          QStringLiteral("_barrier._tcp"),
                          QStringLiteral("local."));
    record.hostName = host;
    record.port = 49165;
    record.txt[QStringLiteral("display-ready")] = displayReady;
    return record;
}

}

class ZeroconfBrowserTestPeer
{
public:
    static QByteArray encodedTxt(const QMap<QString, QByteArray>& txt)
    {
        QByteArray rdata;
        for (auto item = txt.constBegin(); item != txt.constEnd(); ++item) {
            const QByteArray encoded =
                item.key().toUtf8() + QByteArrayLiteral("=") + item.value();
            EXPECT_LE(encoded.size(), 255);
            rdata.append(static_cast<char>(encoded.size()));
            rdata.append(encoded);
        }
        return rdata;
    }

    static void addResolver(ZeroconfBrowser& browser,
                            const ZeroconfRecord& record,
                            quint32 interfaceIndex,
                            bool resolved)
    {
        ZeroconfBrowser::Resolver* resolver =
            new ZeroconfBrowser::Resolver;
        resolver->browser = &browser;
        resolver->key = ZeroconfRecordAggregator::resolverKey(
            record, interfaceIndex);
        resolver->record = record;
        resolver->interfaceIndex = interfaceIndex;
        browser.m_Resolvers.insert(resolver->key, resolver);
        ZeroconfBrowser::BrowsedService service;
        service.record = ZeroconfRecord(
            record.serviceName, record.registeredType, record.replyDomain);
        service.interfaceIndex = interfaceIndex;
        browser.m_BrowsedServices.insert(resolver->key, service);
        if (resolved) {
            browser.m_RecordAggregator.update(record, interfaceIndex);
        }
    }

    static void removeFromBrowseBatch(ZeroconfBrowser& browser,
                                      const ZeroconfRecord& record,
                                      quint32 interfaceIndex,
                                      bool moreComing)
    {
        ZeroconfBrowser::browseReply(
            nullptr,
            moreComing ? kDNSServiceFlagsMoreComing : 0,
            interfaceIndex, kDNSServiceErr_NoError,
            record.serviceName.toUtf8().constData(),
            record.registeredType.toUtf8().constData(),
            record.replyDomain.toUtf8().constData(), &browser);
    }

    static void failResolver(ZeroconfBrowser& browser,
                             const ZeroconfRecord& record,
                             quint32 interfaceIndex)
    {
        browser.resolverFailed(
            ZeroconfRecordAggregator::resolverKey(record, interfaceIndex));
    }

    static void failBrowseCallback(ZeroconfBrowser& browser,
                                   DNSServiceErrorType errorCode)
    {
        ZeroconfBrowser::browseReply(
            nullptr, 0, 0, errorCode, nullptr, nullptr, nullptr, &browser);
        browser.finishBrowseFailure(browser.m_BrowseCallbackError);
    }

    static bool retryPending(const ZeroconfBrowser& browser,
                             const ZeroconfRecord& record,
                             quint32 interfaceIndex)
    {
        return browser.m_ResolverRetryTokens.contains(
            ZeroconfRecordAggregator::resolverKey(record, interfaceIndex));
    }

    static void txtCallback(ZeroconfBrowser& browser,
                            const ZeroconfRecord& record,
                            quint32 interfaceIndex,
                            const QMap<QString, QByteArray>& txt,
                            bool add,
                            bool moreComing)
    {
        const QByteArray rdata = encodedTxt(txt);
        ZeroconfBrowser::Resolver* resolver = browser.m_Resolvers.value(
            ZeroconfRecordAggregator::resolverKey(record, interfaceIndex),
            nullptr);
        ASSERT_NE(resolver, nullptr);
        DNSServiceFlags flags = 0;
        if (add) {
            flags |= kDNSServiceFlagsAdd;
        }
        if (moreComing) {
            flags |= kDNSServiceFlagsMoreComing;
        }
        ZeroconfBrowser::queryReply(
            nullptr, flags, interfaceIndex, kDNSServiceErr_NoError,
            nullptr, kDNSServiceType_TXT, kDNSServiceClass_IN,
            static_cast<quint16>(rdata.size()), rdata.constData(), 120,
            resolver);
    }

    static void resolveCallback(ZeroconfBrowser& browser,
                                const ZeroconfRecord& record,
                                quint32 interfaceIndex,
                                const QString& host,
                                quint16 port,
                                const QMap<QString, QByteArray>& txt,
                                bool moreComing)
    {
        ZeroconfBrowser::Resolver* resolver = browser.m_Resolvers.value(
            ZeroconfRecordAggregator::resolverKey(record, interfaceIndex),
            nullptr);
        ASSERT_NE(resolver, nullptr);
        const QByteArray rdata = encodedTxt(txt);
        const QByteArray fullName = QByteArrayLiteral("Desk._barrier._tcp.local.");
        const QByteArray hostBytes = host.toUtf8();
        ZeroconfBrowser::resolveReply(
            nullptr, moreComing ? kDNSServiceFlagsMoreComing : 0,
            interfaceIndex, kDNSServiceErr_NoError,
            fullName.constData(), hostBytes.constData(), qToBigEndian(port),
            static_cast<quint16>(rdata.size()),
            reinterpret_cast<const unsigned char*>(rdata.constData()),
            resolver);
    }
};

TEST(ZeroconfBrowserTests, identicalResolveCallbacksPublishOnlyOnce)
{
    ZeroconfBrowser browser;
    const ZeroconfRecord unresolved(
        QStringLiteral("Desk"), QStringLiteral("_barrier._tcp"),
        QStringLiteral("local."));
    ZeroconfBrowserTestPeer::addResolver(browser, unresolved, 4, false);

    int publicationCount = 0;
    QObject::connect(
        &browser, &ZeroconfBrowser::currentRecordsChanged,
        [&publicationCount](const QList<ZeroconfRecord>&) {
            ++publicationCount;
        });
    const QMap<QString, QByteArray> txt{
        {QStringLiteral("display-ready"), QByteArrayLiteral("1")}};

    ZeroconfBrowserTestPeer::resolveCallback(
        browser, unresolved, 4, QStringLiteral("desk.local."), 49165,
        txt, false);
    ASSERT_EQ(1, publicationCount);

    ZeroconfBrowserTestPeer::resolveCallback(
        browser, unresolved, 4, QStringLiteral("desk.local."), 49165,
        txt, false);
    EXPECT_EQ(1, publicationCount);
}

TEST(ZeroconfBrowserTests, resolveMoreComingPublishesFinalSnapshotOnce)
{
    ZeroconfBrowser browser;
    const ZeroconfRecord unresolved(
        QStringLiteral("Desk"), QStringLiteral("_barrier._tcp"),
        QStringLiteral("local."));
    ZeroconfBrowserTestPeer::addResolver(browser, unresolved, 4, false);

    int publicationCount = 0;
    QObject::connect(
        &browser, &ZeroconfBrowser::currentRecordsChanged,
        [&publicationCount](const QList<ZeroconfRecord>&) {
            ++publicationCount;
        });
    const QMap<QString, QByteArray> txt{
        {QStringLiteral("display-ready"), QByteArrayLiteral("1")}};

    ZeroconfBrowserTestPeer::resolveCallback(
        browser, unresolved, 4, QStringLiteral("desk.local."), 49165,
        txt, true);
    EXPECT_EQ(0, publicationCount);
    ZeroconfBrowserTestPeer::resolveCallback(
        browser, unresolved, 4, QStringLiteral("desk.local."), 49165,
        txt, false);
    EXPECT_EQ(1, publicationCount);

    ZeroconfBrowserTestPeer::resolveCallback(
        browser, unresolved, 4, QStringLiteral("desk.local."), 49165,
        txt, false);
    EXPECT_EQ(1, publicationCount);
}

TEST(ZeroconfBrowserTests, changedResolveFieldsStillPublish)
{
    ZeroconfBrowser browser;
    const ZeroconfRecord unresolved(
        QStringLiteral("Desk"), QStringLiteral("_barrier._tcp"),
        QStringLiteral("local."));
    ZeroconfBrowserTestPeer::addResolver(browser, unresolved, 4, false);

    int publicationCount = 0;
    QList<ZeroconfRecord> published;
    QObject::connect(
        &browser, &ZeroconfBrowser::currentRecordsChanged,
        [&publicationCount, &published](
            const QList<ZeroconfRecord>& records) {
            ++publicationCount;
            published = records;
        });
    const QMap<QString, QByteArray> notReady{
        {QStringLiteral("display-ready"), QByteArrayLiteral("0")}};
    const QMap<QString, QByteArray> ready{
        {QStringLiteral("display-ready"), QByteArrayLiteral("1")}};

    ZeroconfBrowserTestPeer::resolveCallback(
        browser, unresolved, 4, QStringLiteral("desk.local."), 49165,
        notReady, false);
    EXPECT_EQ(1, publicationCount);
    ZeroconfBrowserTestPeer::resolveCallback(
        browser, unresolved, 4, QStringLiteral("desk-new.local."), 49165,
        notReady, false);
    EXPECT_EQ(2, publicationCount);
    ZeroconfBrowserTestPeer::resolveCallback(
        browser, unresolved, 4, QStringLiteral("desk-new.local."), 49166,
        notReady, false);
    EXPECT_EQ(3, publicationCount);
    ZeroconfBrowserTestPeer::resolveCallback(
        browser, unresolved, 4, QStringLiteral("desk-new.local."), 49166,
        ready, false);
    EXPECT_EQ(4, publicationCount);

    ZeroconfBrowserTestPeer::addResolver(browser, unresolved, 5, false);
    ZeroconfBrowserTestPeer::resolveCallback(
        browser, unresolved, 5, QStringLiteral("desk-new.local."), 49166,
        ready, false);
    ASSERT_EQ(5, publicationCount);
    ASSERT_EQ(1, published.size());
    EXPECT_EQ(5u, published.first().interfaceIndex);
}

TEST(ZeroconfBrowserTests, resolverIdentityIncludesInterfaceIndex)
{
    const ZeroconfRecord record =
        resolvedRecord(QStringLiteral("desk.local."), QByteArrayLiteral("1"));

    EXPECT_EQ(ZeroconfRecordAggregator::resolverKey(record, 4),
              ZeroconfRecordAggregator::resolverKey(record, 4));
    EXPECT_NE(ZeroconfRecordAggregator::resolverKey(record, 4),
              ZeroconfRecordAggregator::resolverKey(record, 5));
}

TEST(ZeroconfBrowserTests, aggregatesInterfacesIntoOneLogicalRecord)
{
    ZeroconfRecordAggregator records;
    const ZeroconfRecord wifi =
        resolvedRecord(QStringLiteral("desk-wifi.local."),
                       QByteArrayLiteral("0"));
    const ZeroconfRecord ethernet =
        resolvedRecord(QStringLiteral("desk-ethernet.local."),
                       QByteArrayLiteral("1"));

    EXPECT_TRUE(records.update(wifi, 4));
    EXPECT_TRUE(records.update(ethernet, 5));
    ASSERT_EQ(records.records().size(), 1);
    EXPECT_EQ(records.records().first().hostName, ethernet.hostName);
    EXPECT_EQ(records.records().first().txt, ethernet.txt);
    EXPECT_EQ(5u, records.records().first().interfaceIndex);
}

TEST(ZeroconfBrowserTests, removingOneInterfaceRetainsLogicalService)
{
    ZeroconfRecordAggregator records;
    const ZeroconfRecord wifi =
        resolvedRecord(QStringLiteral("desk-wifi.local."),
                       QByteArrayLiteral("1"));
    const ZeroconfRecord ethernet =
        resolvedRecord(QStringLiteral("desk-ethernet.local."),
                       QByteArrayLiteral("1"));

    records.update(wifi, 4);
    records.update(ethernet, 5);

    EXPECT_FALSE(records.remove(wifi, 4));
    ASSERT_EQ(records.records().size(), 1);
    EXPECT_EQ(records.records().first().hostName, ethernet.hostName);
    EXPECT_EQ(5u, records.records().first().interfaceIndex);

    EXPECT_TRUE(records.remove(ethernet, 5));
    EXPECT_TRUE(records.records().isEmpty());
}

TEST(ZeroconfBrowserTests, failedInterfaceFallsBackToAnotherResolution)
{
    ZeroconfRecordAggregator records;
    const ZeroconfRecord wifi =
        resolvedRecord(QStringLiteral("desk-wifi.local."),
                       QByteArrayLiteral("0"));
    const ZeroconfRecord ethernet =
        resolvedRecord(QStringLiteral("desk-ethernet.local."),
                       QByteArrayLiteral("1"));

    records.update(wifi, 4);
    records.update(ethernet, 5);

    EXPECT_TRUE(records.remove(ethernet, 5));
    ASSERT_EQ(records.records().size(), 1);
    EXPECT_EQ(records.records().first().hostName, wifi.hostName);
    EXPECT_EQ(records.records().first().txt, wifi.txt);
    EXPECT_EQ(4u, records.records().first().interfaceIndex);

    EXPECT_FALSE(records.remove(ethernet, 6));
    EXPECT_EQ(records.records().size(), 1);
}

TEST(ZeroconfBrowserTests, txtUpdatesReplaceTheLogicalRecord)
{
    ZeroconfRecordAggregator records;
    ZeroconfRecord record =
        resolvedRecord(QStringLiteral("desk.local."), QByteArrayLiteral("0"));

    EXPECT_TRUE(records.update(record, 4));
    record.txt[QStringLiteral("display-ready")] = QByteArrayLiteral("1");
    EXPECT_TRUE(records.update(record, 4));
    ASSERT_EQ(records.records().size(), 1);
    EXPECT_EQ(records.records().first().txt, record.txt);

    EXPECT_FALSE(records.update(record, 4));
}

TEST(ZeroconfBrowserTests,
     retiringOldTxtAfterAddingNewKeepsSettledMetadata)
{
    ZeroconfBrowser browser;
    ZeroconfRecord oldRecord =
        resolvedRecord(QStringLiteral("desk.local."), QByteArrayLiteral("0"));
    oldRecord.txt[QStringLiteral("proximity-id")] = QByteArrayLiteral("paired");
    ZeroconfRecord newRecord = oldRecord;
    newRecord.txt[QStringLiteral("display-ready")] = QByteArrayLiteral("1");
    ZeroconfBrowserTestPeer::addResolver(browser, oldRecord, 4, true);

    int publicationCount = 0;
    QList<ZeroconfRecord> publishedRecords;
    QObject::connect(
        &browser, &ZeroconfBrowser::currentRecordsChanged,
        [&publicationCount, &publishedRecords](
            const QList<ZeroconfRecord>& records) {
            ++publicationCount;
            publishedRecords = records;
        });

    ZeroconfBrowserTestPeer::txtCallback(
        browser, oldRecord, 4, newRecord.txt, true, true);
    EXPECT_EQ(0, publicationCount);
    ZeroconfBrowserTestPeer::txtCallback(
        browser, oldRecord, 4, oldRecord.txt, false, false);

    ASSERT_EQ(1, publicationCount);
    ASSERT_EQ(1, publishedRecords.size());
    EXPECT_EQ(newRecord.txt, publishedRecords.first().txt);
    EXPECT_EQ(newRecord.txt, browser.currentRecords().first().txt);
}

TEST(ZeroconfBrowserTests,
     removingOldTxtBeforeAddingNewPublishesOnlySettledMetadata)
{
    ZeroconfBrowser browser;
    ZeroconfRecord oldRecord =
        resolvedRecord(QStringLiteral("desk.local."), QByteArrayLiteral("0"));
    oldRecord.txt[QStringLiteral("proximity-id")] = QByteArrayLiteral("paired");
    ZeroconfRecord newRecord = oldRecord;
    newRecord.txt[QStringLiteral("display-ready")] = QByteArrayLiteral("1");
    ZeroconfBrowserTestPeer::addResolver(browser, oldRecord, 4, true);

    int publicationCount = 0;
    QList<ZeroconfRecord> publishedRecords;
    QObject::connect(
        &browser, &ZeroconfBrowser::currentRecordsChanged,
        [&publicationCount, &publishedRecords](
            const QList<ZeroconfRecord>& records) {
            ++publicationCount;
            publishedRecords = records;
        });

    ZeroconfBrowserTestPeer::txtCallback(
        browser, oldRecord, 4, oldRecord.txt, false, true);
    EXPECT_EQ(0, publicationCount);
    ZeroconfBrowserTestPeer::txtCallback(
        browser, oldRecord, 4, newRecord.txt, true, false);

    ASSERT_EQ(1, publicationCount);
    ASSERT_EQ(1, publishedRecords.size());
    EXPECT_EQ(newRecord.txt, publishedRecords.first().txt);
    EXPECT_EQ(newRecord.txt, browser.currentRecords().first().txt);
}

TEST(ZeroconfBrowserTests,
     laterRetirementOfOldTxtDoesNotUndoPublishedReplacement)
{
    ZeroconfBrowser browser;
    ZeroconfRecord oldRecord =
        resolvedRecord(QStringLiteral("desk.local."), QByteArrayLiteral("0"));
    oldRecord.txt[QStringLiteral("proximity-id")] = QByteArrayLiteral("paired");
    ZeroconfRecord newRecord = oldRecord;
    newRecord.txt[QStringLiteral("display-ready")] = QByteArrayLiteral("1");
    ZeroconfBrowserTestPeer::addResolver(browser, oldRecord, 4, true);

    int publicationCount = 0;
    QObject::connect(
        &browser, &ZeroconfBrowser::currentRecordsChanged,
        [&publicationCount](const QList<ZeroconfRecord>&) {
            ++publicationCount;
        });

    ZeroconfBrowserTestPeer::txtCallback(
        browser, oldRecord, 4, newRecord.txt, true, false);
    ASSERT_EQ(1, publicationCount);
    ZeroconfBrowserTestPeer::txtCallback(
        browser, oldRecord, 4, oldRecord.txt, false, false);

    EXPECT_EQ(1, publicationCount);
    ASSERT_EQ(1, browser.currentRecords().size());
    EXPECT_EQ(newRecord.txt, browser.currentRecords().first().txt);
}

TEST(ZeroconfBrowserTests, removingOnlyCurrentTxtRetiresResolverRecord)
{
    ZeroconfBrowser browser;
    ZeroconfRecord record =
        resolvedRecord(QStringLiteral("desk.local."), QByteArrayLiteral("1"));
    record.txt[QStringLiteral("proximity-id")] = QByteArrayLiteral("paired");
    ZeroconfBrowserTestPeer::addResolver(browser, record, 4, true);

    int publicationCount = 0;
    QList<ZeroconfRecord> publishedRecords;
    QObject::connect(
        &browser, &ZeroconfBrowser::currentRecordsChanged,
        [&publicationCount, &publishedRecords](
            const QList<ZeroconfRecord>& records) {
            ++publicationCount;
            publishedRecords = records;
        });

    ZeroconfBrowserTestPeer::txtCallback(
        browser, record, 4, record.txt, false, false);

    ASSERT_EQ(1, publicationCount);
    EXPECT_TRUE(publishedRecords.isEmpty());
    EXPECT_TRUE(browser.currentRecords().isEmpty());
}

TEST(ZeroconfBrowserTests,
     removingCurrentTxtOnSiblingKeepsHealthyInterfaceSelected)
{
    ZeroconfBrowser browser;
    ZeroconfRecord oldRecord =
        resolvedRecord(QStringLiteral("desk-wifi.local."), QByteArrayLiteral("0"));
    oldRecord.txt[QStringLiteral("proximity-id")] = QByteArrayLiteral("paired");
    ZeroconfRecord healthyRecord =
        resolvedRecord(QStringLiteral("desk-ethernet.local."), QByteArrayLiteral("1"));
    healthyRecord.txt[QStringLiteral("proximity-id")] = QByteArrayLiteral("paired");
    ZeroconfBrowserTestPeer::addResolver(browser, oldRecord, 4, true);
    ZeroconfBrowserTestPeer::addResolver(browser, healthyRecord, 5, true);

    int publicationCount = 0;
    QObject::connect(
        &browser, &ZeroconfBrowser::currentRecordsChanged,
        [&publicationCount](const QList<ZeroconfRecord>&) {
            ++publicationCount;
        });

    ZeroconfBrowserTestPeer::txtCallback(
        browser, oldRecord, 4, oldRecord.txt, false, false);

    EXPECT_EQ(0, publicationCount);
    ASSERT_EQ(1, browser.currentRecords().size());
    EXPECT_EQ(healthyRecord.hostName,
              browser.currentRecords().first().hostName);
    EXPECT_EQ(healthyRecord.txt, browser.currentRecords().first().txt);
}

TEST(ZeroconfBrowserTests,
     resolverFailureFlushesPendingTxtRemoval)
{
    ZeroconfBrowser browser;
    ZeroconfRecord record =
        resolvedRecord(QStringLiteral("desk.local."), QByteArrayLiteral("1"));
    record.txt[QStringLiteral("proximity-id")] = QByteArrayLiteral("paired");
    ZeroconfBrowserTestPeer::addResolver(browser, record, 4, true);

    int publicationCount = 0;
    QList<ZeroconfRecord> publishedRecords;
    QObject::connect(
        &browser, &ZeroconfBrowser::currentRecordsChanged,
        [&publicationCount, &publishedRecords](
            const QList<ZeroconfRecord>& records) {
            ++publicationCount;
            publishedRecords = records;
        });

    ZeroconfBrowserTestPeer::txtCallback(
        browser, record, 4, record.txt, false, true);
    EXPECT_EQ(0, publicationCount);
    EXPECT_TRUE(browser.currentRecords().isEmpty());

    ZeroconfBrowserTestPeer::failResolver(browser, record, 4);
    EXPECT_EQ(1, publicationCount);
    EXPECT_TRUE(publishedRecords.isEmpty());
}

TEST(ZeroconfBrowserTests, browseRemovalBatchPublishesOnlyFinalState)
{
    ZeroconfBrowser browser;
    const ZeroconfRecord wifi =
        resolvedRecord(QStringLiteral("desk-wifi.local."),
                       QByteArrayLiteral("0"));
    const ZeroconfRecord ethernet =
        resolvedRecord(QStringLiteral("desk-ethernet.local."),
                       QByteArrayLiteral("1"));
    ZeroconfBrowserTestPeer::addResolver(browser, wifi, 4, true);
    ZeroconfBrowserTestPeer::addResolver(browser, ethernet, 5, true);

    int publicationCount = 0;
    QList<ZeroconfRecord> publishedRecords;
    QObject::connect(
        &browser, &ZeroconfBrowser::currentRecordsChanged,
        [&publicationCount, &publishedRecords](
            const QList<ZeroconfRecord>& records) {
            ++publicationCount;
            publishedRecords = records;
        });

    ZeroconfBrowserTestPeer::removeFromBrowseBatch(
        browser, ethernet, 5, true);
    EXPECT_EQ(publicationCount, 0);
    ASSERT_EQ(browser.currentRecords().size(), 1);
    EXPECT_EQ(browser.currentRecords().first().hostName, wifi.hostName);

    ZeroconfBrowserTestPeer::removeFromBrowseBatch(
        browser, wifi, 4, false);
    EXPECT_EQ(publicationCount, 1);
    EXPECT_TRUE(publishedRecords.isEmpty());
}

TEST(ZeroconfBrowserTests,
     failedUnresolvedInterfaceDoesNotHideHealthySibling)
{
    ZeroconfBrowser browser;
    const ZeroconfRecord wifi =
        resolvedRecord(QStringLiteral("desk-wifi.local."),
                       QByteArrayLiteral("1"));
    const ZeroconfRecord unresolved(
        wifi.serviceName, wifi.registeredType, wifi.replyDomain);
    ZeroconfBrowserTestPeer::addResolver(browser, wifi, 4, true);
    ZeroconfBrowserTestPeer::addResolver(browser, unresolved, 5, false);

    int errorCount = 0;
    int publicationCount = 0;
    QObject::connect(&browser, &ZeroconfBrowser::error,
                     [&errorCount](DNSServiceErrorType) { ++errorCount; });
    QObject::connect(
        &browser, &ZeroconfBrowser::currentRecordsChanged,
        [&publicationCount](const QList<ZeroconfRecord>&) {
            ++publicationCount;
        });

    ZeroconfBrowserTestPeer::failResolver(
        browser, unresolved, 5);
    EXPECT_EQ(errorCount, 0);
    EXPECT_EQ(publicationCount, 0);
    ASSERT_EQ(browser.currentRecords().size(), 1);
    EXPECT_EQ(browser.currentRecords().first().hostName, wifi.hostName);

    ZeroconfBrowserTestPeer::failResolver(
        browser, wifi, 4);
    EXPECT_EQ(errorCount, 0);
    EXPECT_EQ(publicationCount, 1);
    EXPECT_TRUE(browser.currentRecords().isEmpty());
}

TEST(ZeroconfBrowserTests, soleTransientResolverFailureSchedulesRecovery)
{
    ZeroconfBrowser browser;
    const ZeroconfRecord record =
        resolvedRecord(QStringLiteral("desk.local."), QByteArrayLiteral("1"));
    ZeroconfBrowserTestPeer::addResolver(browser, record, 4, true);

    ZeroconfBrowserTestPeer::failResolver(browser, record, 4);
    EXPECT_TRUE(browser.currentRecords().isEmpty());
    EXPECT_TRUE(ZeroconfBrowserTestPeer::retryPending(
        browser, record, 4));

    // A real browse removal cancels the pending retry, so a departed service
    // cannot be resurrected by the delayed callback.
    ZeroconfBrowserTestPeer::removeFromBrowseBatch(
        browser, record, 4, false);
    EXPECT_FALSE(ZeroconfBrowserTestPeer::retryPending(
        browser, record, 4));
}

TEST(ZeroconfBrowserTests, resolverFailureSurvivesDeletionByPublicationSlot)
{
    ZeroconfBrowser* browser = new ZeroconfBrowser;
    QPointer<ZeroconfBrowser> guard(browser);
    const ZeroconfRecord record =
        resolvedRecord(QStringLiteral("desk.local."), QByteArrayLiteral("1"));
    ZeroconfBrowserTestPeer::addResolver(*browser, record, 4, true);
    QObject::connect(
        browser, &ZeroconfBrowser::currentRecordsChanged,
        [browser](const QList<ZeroconfRecord>&) { delete browser; });

    ZeroconfBrowserTestPeer::failResolver(*browser, record, 4);
    EXPECT_TRUE(guard.isNull());
}

TEST(ZeroconfBrowserTests, browseErrorFlushesPendingBatchAsEmpty)
{
    ZeroconfBrowser browser;
    const ZeroconfRecord wifi =
        resolvedRecord(QStringLiteral("desk-wifi.local."),
                       QByteArrayLiteral("0"));
    const ZeroconfRecord ethernet =
        resolvedRecord(QStringLiteral("desk-ethernet.local."),
                       QByteArrayLiteral("1"));
    ZeroconfBrowserTestPeer::addResolver(browser, wifi, 4, true);
    ZeroconfBrowserTestPeer::addResolver(browser, ethernet, 5, true);

    int errorCount = 0;
    int publicationCount = 0;
    QList<ZeroconfRecord> publishedRecords;
    QObject::connect(&browser, &ZeroconfBrowser::error,
                     [&errorCount](DNSServiceErrorType) { ++errorCount; });
    QObject::connect(
        &browser, &ZeroconfBrowser::currentRecordsChanged,
        [&publicationCount, &publishedRecords](
            const QList<ZeroconfRecord>& records) {
            ++publicationCount;
            publishedRecords = records;
        });

    ZeroconfBrowserTestPeer::removeFromBrowseBatch(
        browser, ethernet, 5, true);
    EXPECT_EQ(publicationCount, 0);

    ZeroconfBrowserTestPeer::failBrowseCallback(
        browser, kDNSServiceErr_Unknown);
    EXPECT_EQ(errorCount, 1);
    EXPECT_EQ(publicationCount, 1);
    EXPECT_TRUE(publishedRecords.isEmpty());
    EXPECT_TRUE(browser.currentRecords().isEmpty());
}
