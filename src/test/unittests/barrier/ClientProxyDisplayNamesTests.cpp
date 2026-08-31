/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
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

#include "server/ClientProxy1_0.h"

#include "barrier/DisplayNames.h"
#include "barrier/ProtocolUtil.h"
#include "barrier/protocol_types.h"

#include "test/global/gtest.h"
#include "test/global/TestEventQueue.h"
#include "test/mock/io/MockStream.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

// Stream that replays a scripted byte buffer on read and records writes,
// so the proxy parses exact ProtocolUtil-encoded wire messages.
class ScriptedStream : public MockStream {
public:
    std::vector<UInt8> in;
    std::vector<UInt8> out;
    std::size_t pos = 0;

    UInt32 read(void* buffer, UInt32 count) override
    {
        const std::size_t avail = in.size() - pos;
        const std::size_t n = (count < avail) ? count : avail;
        if (n > 0) {
            std::memcpy(buffer, &in[pos], n);
            pos += n;
        }
        return static_cast<UInt32>(n);
    }

    void write(const void* data, UInt32 count) override
    {
        const UInt8* p = static_cast<const UInt8*>(data);
        out.insert(out.end(), p, p + count);
    }
};

// Encodes complete wire messages (code + payload) with the real
// ProtocolUtil codec, concatenated in call order.
class Wire {
public:
    template <typename... Args>
    void add(const char* fmt, Args... args)
    {
        ScriptedStream cap;
        ProtocolUtil::writef(&cap, fmt, args...);
        bytes.insert(bytes.end(), cap.out.begin(), cap.out.end());
    }

    std::vector<UInt8> bytes;
};

// Exposes the protected message parser for tests.
class ClientProxy1_0Test : public ClientProxy1_0 {
public:
    ClientProxy1_0Test(const std::string& name, barrier::IStream* stream,
                       IEventQueue* events) :
        ClientProxy1_0(name, stream, events)
    {
    }

    bool parse(const UInt8* code) { return parseMessage(code); }
};

std::vector<UInt32> twoRectDdis()
{
    std::vector<UInt32> rects;
    rects.push_back(0);
    rects.push_back(0);
    rects.push_back(1920);
    rects.push_back(1080);
    rects.push_back(1920);
    rects.push_back(0);
    rects.push_back(1080);
    rects.push_back(1920);
    return rects;
}

std::vector<UInt32> oneRectDdis()
{
    std::vector<UInt32> rects;
    rects.push_back(0);
    rects.push_back(0);
    rects.push_back(1920);
    rects.push_back(1080);
    return rects;
}

std::vector<UInt32> portraitRectDdis()
{
    std::vector<UInt32> rects;
    rects.push_back(0);
    rects.push_back(0);
    rects.push_back(1080);
    rects.push_back(1920);
    return rects;
}

void addInfo(Wire& wire)
{
    wire.add(kMsgDInfo + 4, (SInt32)0, (SInt32)0, (SInt32)1920, (SInt32)1080,
             (SInt32)0, (SInt32)100, (SInt32)100);
}

std::vector<UInt32> twoNameDdnm()
{
    std::vector<std::string> names;
    names.push_back("Left");
    names.push_back("Right");
    return barrier::encodeDisplayNames(names, 2);
}

} // namespace

TEST(ClientProxyDisplayNamesTests, dinfOnlyPeerReportsSyntheticDisplayRect)
{
    TestEventQueue events;
    Wire wire;
    addInfo(wire);

    ScriptedStream* stream = new ScriptedStream;
    stream->in = wire.bytes;
    ClientProxy1_0Test proxy("peer", stream, &events);
    proxy.setPeerMinorVersion(6);

    ASSERT_TRUE(proxy.parse(reinterpret_cast<const UInt8*>("DINF")));

    std::vector<ScreenRect> displays;
    proxy.getDisplays(displays);
    ASSERT_EQ(1u, displays.size());
    EXPECT_EQ(0, displays[0].x);
    EXPECT_EQ(0, displays[0].y);
    EXPECT_EQ(1920, displays[0].w);
    EXPECT_EQ(1080, displays[0].h);
}

TEST(ClientProxyDisplayNamesTests, ddisPeerReportsRealDisplayRects)
{
    TestEventQueue events;
    Wire wire;
    addInfo(wire);
    std::vector<UInt32> rects = twoRectDdis();
    wire.add(kMsgDDisplayInfo + 4, &rects);

    ScriptedStream* stream = new ScriptedStream;
    stream->in = wire.bytes;
    ClientProxy1_0Test proxy("peer", stream, &events);
    proxy.setPeerMinorVersion(7);

    ASSERT_TRUE(proxy.parse(reinterpret_cast<const UInt8*>("DINF")));
    ASSERT_TRUE(proxy.parse(reinterpret_cast<const UInt8*>("DDIS")));

    std::vector<ScreenRect> displays;
    proxy.getDisplays(displays);
    ASSERT_EQ(2u, displays.size());
    EXPECT_EQ(0, displays[0].x);
    EXPECT_EQ(1920, displays[0].w);
    EXPECT_EQ(1920, displays[1].x);
    EXPECT_EQ(1080, displays[1].w);
}

TEST(ClientProxyDisplayNamesTests, ddisPeerPreservesPortraitRectOrientation)
{
    TestEventQueue events;
    Wire wire;
    addInfo(wire);
    std::vector<UInt32> rects = portraitRectDdis();
    wire.add(kMsgDDisplayInfo + 4, &rects);

    ScriptedStream* stream = new ScriptedStream;
    stream->in = wire.bytes;
    ClientProxy1_0Test proxy("peer", stream, &events);
    proxy.setPeerMinorVersion(7);

    ASSERT_TRUE(proxy.parse(reinterpret_cast<const UInt8*>("DINF")));
    ASSERT_TRUE(proxy.parse(reinterpret_cast<const UInt8*>("DDIS")));

    std::vector<ScreenRect> displays;
    proxy.getDisplays(displays);
    ASSERT_EQ(1u, displays.size());
    EXPECT_EQ(1080, displays[0].w);
    EXPECT_EQ(1920, displays[0].h);
}

TEST(ClientProxyDisplayNamesTests, ddnmParsedFor17Peer_orderMatchesDisplays)
{
    TestEventQueue events;
    Wire wire;
    addInfo(wire);
    std::vector<UInt32> rects = twoRectDdis();
    wire.add(kMsgDDisplayInfo + 4, &rects);
    std::vector<UInt32> ddnm = twoNameDdnm();
    wire.add(kMsgDDisplayNames + 4, &ddnm);

    ScriptedStream* stream = new ScriptedStream;
    stream->in = wire.bytes;
    ClientProxy1_0Test proxy("peer", stream, &events);
    proxy.setPeerMinorVersion(7);

    ASSERT_TRUE(proxy.parse(reinterpret_cast<const UInt8*>("DINF")));
    ASSERT_TRUE(proxy.parse(reinterpret_cast<const UInt8*>("DDIS")));
    ASSERT_TRUE(proxy.parse(reinterpret_cast<const UInt8*>("DDNM")));

    // names are ordered exactly with the DDIS rectangles
    std::vector<ScreenRect> displays;
    proxy.getDisplays(displays);
    ASSERT_EQ(2u, displays.size());
    EXPECT_EQ(0, displays[0].x);
    EXPECT_EQ(1920, displays[0].w);
    EXPECT_EQ(1920, displays[1].x);

    std::vector<std::string> names;
    proxy.getDisplayNames(names);
    ASSERT_EQ(2u, names.size());
    EXPECT_EQ("Left", names[0]);
    EXPECT_EQ("Right", names[1]);
}

TEST(ClientProxyDisplayNamesTests, ddnmIgnoredFor16Peer_noNamesNoDesync)
{
    TestEventQueue events;
    Wire wire;
    addInfo(wire);
    std::vector<UInt32> rects = twoRectDdis();
    wire.add(kMsgDDisplayInfo + 4, &rects);
    std::vector<UInt32> ddnm = twoNameDdnm();
    wire.add(kMsgDDisplayNames + 4, &ddnm);
    // a message after the ignored DDNM must still parse: the payload has
    // to be consumed so the message stream stays in sync
    std::vector<UInt32> fresh = oneRectDdis();
    wire.add(kMsgDDisplayInfo + 4, &fresh);

    ScriptedStream* stream = new ScriptedStream;
    stream->in = wire.bytes;
    ClientProxy1_0Test proxy("peer", stream, &events);
    proxy.setPeerMinorVersion(6);

    ASSERT_TRUE(proxy.parse(reinterpret_cast<const UInt8*>("DINF")));
    ASSERT_TRUE(proxy.parse(reinterpret_cast<const UInt8*>("DDIS")));
    ASSERT_TRUE(proxy.parse(reinterpret_cast<const UInt8*>("DDNM")));
    ASSERT_TRUE(proxy.parse(reinterpret_cast<const UInt8*>("DDIS")));

    // a pre-1.7 peer never populates display names
    std::vector<std::string> names;
    proxy.getDisplayNames(names);
    EXPECT_TRUE(names.empty());

    // and its geometry is untouched by the ignored DDNM
    std::vector<ScreenRect> displays;
    proxy.getDisplays(displays);
    ASSERT_EQ(1u, displays.size());
    EXPECT_EQ(1920, displays[0].w);
}

TEST(ClientProxyDisplayNamesTests, ddisRefreshClearsStaleNames)
{
    TestEventQueue events;
    Wire wire;
    addInfo(wire);
    std::vector<UInt32> rects = twoRectDdis();
    wire.add(kMsgDDisplayInfo + 4, &rects);
    std::vector<UInt32> ddnm = twoNameDdnm();
    wire.add(kMsgDDisplayNames + 4, &ddnm);
    // fresh geometry without a following DDNM
    std::vector<UInt32> fresh = oneRectDdis();
    wire.add(kMsgDDisplayInfo + 4, &fresh);

    ScriptedStream* stream = new ScriptedStream;
    stream->in = wire.bytes;
    ClientProxy1_0Test proxy("peer", stream, &events);
    proxy.setPeerMinorVersion(7);

    ASSERT_TRUE(proxy.parse(reinterpret_cast<const UInt8*>("DINF")));
    ASSERT_TRUE(proxy.parse(reinterpret_cast<const UInt8*>("DDIS")));
    ASSERT_TRUE(proxy.parse(reinterpret_cast<const UInt8*>("DDNM")));

    std::vector<std::string> names;
    proxy.getDisplayNames(names);
    ASSERT_EQ(2u, names.size());

    // the new DDIS invalidates the names exchanged with the old rectangles
    ASSERT_TRUE(proxy.parse(reinterpret_cast<const UInt8*>("DDIS")));
    proxy.getDisplayNames(names);
    EXPECT_TRUE(names.empty());

    std::vector<ScreenRect> displays;
    proxy.getDisplays(displays);
    ASSERT_EQ(1u, displays.size());
    EXPECT_EQ(1920, displays[0].w);
}
