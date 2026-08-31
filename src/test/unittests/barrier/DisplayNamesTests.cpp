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

#include "barrier/DisplayNames.h"

#include "test/global/gtest.h"

using namespace barrier;

namespace {

// display names in the order the platform reports rectangles, including
// multi-byte UTF-8 so byte-length accounting is exercised
const std::vector<std::string> kNames = {"Main", "\xE7\xAC\xAC\xE4\xBA\x8C\xE5\xB1\x8F",
                                         "Studio"};

// expected payload for kNames under the per-name framing:
// [3, 4, 'M','a','i','n', 9, <第二屏 UTF-8>, 6, 'S','t','u','d','i','o']
std::vector<UInt32> expectedPayload()
{
    std::vector<UInt32> data;
    data.push_back(3);

    data.push_back(4); // "Main"
    const char* main_ = "Main";
    for (size_t i = 0; i < 4; ++i) {
        data.push_back((UInt8)main_[i]);
    }

    data.push_back(9); // "\xE7\xAC\xAC\xE4\xBA\x8C\xE5\xB1\x8F"
    const char* second = "\xE7\xAC\xAC\xE4\xBA\x8C\xE5\xB1\x8F";
    for (size_t i = 0; i < 9; ++i) {
        data.push_back((UInt8)second[i]);
    }

    data.push_back(6); // "Studio"
    const char* studio = "Studio";
    for (size_t i = 0; i < 6; ++i) {
        data.push_back((UInt8)studio[i]);
    }
    return data;
}

std::vector<UInt32> payloadWith(std::vector<UInt32> data, UInt32 at, UInt32 value)
{
    data[at] = value;
    return data;
}

} // namespace

TEST(DisplayNamesTests, encode_payloadShape)
{
    const std::vector<UInt32> data = encodeDisplayNames(kNames, 3);

    // [displayCount, length0, bytes0..., length1, bytes1..., ...] via %4I
    ASSERT_EQ(23u, data.size());
    EXPECT_EQ(3u, data[0]);
    EXPECT_EQ(4u, data[1]);  // "Main" byte length
    EXPECT_EQ((UInt8)'M', data[2]);
    EXPECT_EQ((UInt8)'n', data[5]);
    EXPECT_EQ(9u, data[6]);  // 第二屏 byte length
    EXPECT_EQ(6u, data[16]); // "Studio" byte length
    EXPECT_EQ(expectedPayload(), data);
}

TEST(DisplayNamesTests, encode_orderMatchesDisplays)
{
    // names must exactly cover the display count
    ASSERT_EQ(23u, encodeDisplayNames(kNames, 3).size());

    // swapped names swap the per-name framing, keeping lengths aligned
    const std::vector<std::string> swapped = {"Studio", kNames[1], "Main"};
    const std::vector<UInt32> data = encodeDisplayNames(swapped, 3);
    ASSERT_EQ(23u, data.size());
    EXPECT_EQ(6u, data[1]);  // "Studio"
    EXPECT_EQ((UInt8)'S', data[2]);
    EXPECT_EQ(9u, data[8]);  // 第二屏
    EXPECT_EQ(4u, data[18]); // "Main"
    EXPECT_EQ((UInt8)'M', data[19]);
    EXPECT_EQ((UInt8)'n', data[22]);
}

TEST(DisplayNamesTests, encode_emptyName_valid)
{
    // an empty name (unavailable product name) is a valid length-zero
    // entry and never suppresses the payload
    const std::vector<std::string> names = {"Left", "", "Right"};
    const std::vector<UInt32> data = encodeDisplayNames(names, 3);

    ASSERT_EQ(13u, data.size());
    EXPECT_EQ(3u, data[0]);
    EXPECT_EQ(4u, data[1]);  // "Left"
    EXPECT_EQ((UInt8)'t', data[5]);
    EXPECT_EQ(0u, data[6]);  // "" -- zero length, no bytes
    EXPECT_EQ(5u, data[7]);  // "Right"
    EXPECT_EQ((UInt8)'R', data[8]);

    std::vector<std::string> decoded;
    ASSERT_TRUE(decodeDisplayNames(data, 3, decoded));
    ASSERT_EQ(3u, decoded.size());
    EXPECT_EQ("Left", decoded[0]);
    EXPECT_EQ("", decoded[1]);
    EXPECT_EQ("Right", decoded[2]);
}

TEST(DisplayNamesTests, encode_emptyNameList_zeroDisplays)
{
    // zero displays, zero names: a valid [count] payload
    const std::vector<std::string> empty;
    const std::vector<UInt32> data = encodeDisplayNames(empty, 0);
    ASSERT_EQ(1u, data.size());
    EXPECT_EQ(0u, data[0]);
}

TEST(DisplayNamesTests, encode_countMismatch_notSent)
{
    // two names for three displays: the payload cannot be matched to the
    // DDIS rectangles, so nothing is encoded
    const std::vector<std::string> names = {"A", "B"};
    EXPECT_TRUE(encodeDisplayNames(names, 3).empty());
}

TEST(DisplayNamesTests, encode_oversizedPayload_notSent)
{
    const std::string huge(kMaxDisplayNamesByteLength + 1, 'x');
    const std::vector<std::string> names = {huge};
    EXPECT_TRUE(encodeDisplayNames(names, 1).empty());
}

TEST(DisplayNamesTests, decode_roundTrip_orderPreserved)
{
    std::vector<std::string> names;
    ASSERT_TRUE(decodeDisplayNames(expectedPayload(), 3, names));
    ASSERT_EQ(3u, names.size());
    EXPECT_EQ("Main", names[0]);
    EXPECT_EQ("\xE7\xAC\xAC\xE4\xBA\x8C\xE5\xB1\x8F", names[1]);
    EXPECT_EQ("Studio", names[2]);
}

TEST(DisplayNamesTests, decode_countMismatch_rejected)
{
    std::vector<std::string> names;
    // valid payload, but the peer sent two rectangles in DDIS
    EXPECT_FALSE(decodeDisplayNames(expectedPayload(), 2, names));
    EXPECT_TRUE(names.empty());
}

TEST(DisplayNamesTests, decode_truncatedLength_rejected)
{
    // "Main" but the declared length (5) exceeds the 4 trailing bytes
    std::vector<UInt32> data;
    data.push_back(1);
    data.push_back(4);
    data.push_back((UInt8)'M');
    data.push_back((UInt8)'a');
    data.push_back((UInt8)'i');
    data.push_back((UInt8)'n');
    data = payloadWith(data, 1, 5);

    std::vector<std::string> names;
    EXPECT_FALSE(decodeDisplayNames(data, 1, names));
    EXPECT_TRUE(names.empty());
}

TEST(DisplayNamesTests, decode_tooShort_rejected)
{
    std::vector<std::string> names;
    EXPECT_FALSE(decodeDisplayNames(std::vector<UInt32>(), 0, names));
    // count present but the single name's length is missing
    EXPECT_FALSE(decodeDisplayNames(std::vector<UInt32>(1, 1), 1, names));
    EXPECT_TRUE(names.empty());
}

TEST(DisplayNamesTests, decode_oversizedPayload_rejected)
{
    // count=1 with a length of kMax+1 and exactly that many byte elements:
    // the total byte length exceeds the codec bound
    std::vector<UInt32> data;
    data.push_back(1);
    data.push_back(kMaxDisplayNamesByteLength + 1);
    data.resize(data.size() + kMaxDisplayNamesByteLength + 1, 0x41);

    std::vector<std::string> names;
    EXPECT_FALSE(decodeDisplayNames(data, 1, names));
    EXPECT_TRUE(names.empty());
}

TEST(DisplayNamesTests, decode_notByteValues_rejected)
{
    // a "byte" element outside [0, 255] is malformed
    std::vector<UInt32> data;
    data.push_back(1);
    data.push_back(1);
    data.push_back(0x100);
    std::vector<std::string> names;
    EXPECT_FALSE(decodeDisplayNames(data, 1, names));
    EXPECT_TRUE(names.empty());
}

TEST(DisplayNamesTests, decode_trailingElements_rejected)
{
    // a valid single-name payload plus one extra element
    std::vector<UInt32> data = expectedPayload();
    data.push_back(0);
    std::vector<std::string> names;
    EXPECT_FALSE(decodeDisplayNames(data, 3, names));
    EXPECT_TRUE(names.empty());
}

TEST(DisplayNamesTests, decode_partialOutput_clearedOnLateFailure)
{
    // the first name parses, then a non-byte element in the second name
    // fails the decode: the already-parsed name must not leak out
    std::vector<UInt32> data;
    data.push_back(2);
    data.push_back(4); // "Main"
    data.push_back((UInt8)'M');
    data.push_back((UInt8)'a');
    data.push_back((UInt8)'i');
    data.push_back((UInt8)'n');
    data.push_back(4); // "L?ft" with a non-byte
    data.push_back((UInt8)'L');
    data.push_back(0x100);
    data.push_back((UInt8)'f');
    data.push_back((UInt8)'t');

    std::vector<std::string> names;
    EXPECT_FALSE(decodeDisplayNames(data, 2, names));
    EXPECT_TRUE(names.empty());
}

TEST(DisplayNamesTests, capability_negotiation)
{
    // protocol 1.7 introduced DDNM
    EXPECT_FALSE(supportsDisplayNames(6));
    EXPECT_TRUE(supportsDisplayNames(7));
    EXPECT_TRUE(supportsDisplayNames(10));

    // the client announces the highest version both ends support, so an
    // older server never sees an unknown minor in the hello reply
    EXPECT_EQ(6, negotiatedMinorVersion(6));
    EXPECT_EQ(7, negotiatedMinorVersion(7));
    EXPECT_EQ(7, negotiatedMinorVersion(10));
}
