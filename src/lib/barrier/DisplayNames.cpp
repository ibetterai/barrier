/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2002 Chris Schoeneman
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

namespace barrier {

bool
supportsDisplayNames(SInt16 minorVersion)
{
    return minorVersion >= kProtocolMinorVersion;
}

SInt16
negotiatedMinorVersion(SInt16 serverMinor)
{
    return (serverMinor < kProtocolMinorVersion) ? serverMinor
                                                 : kProtocolMinorVersion;
}

std::vector<UInt32>
encodeDisplayNames(const std::vector<std::string>& names, std::size_t displayCount)
{
    std::vector<UInt32> data;

    // names must exactly cover the DDIS display count, otherwise the
    // payload cannot be matched to rectangles.  an empty name is a valid
    // entry (a display without a product name) and never suppresses the
    // whole payload.
    if (names.size() != displayCount) {
        return data;
    }

    // total UTF-8 byte length across all names
    std::size_t byteLength = 0;
    for (std::vector<std::string>::const_iterator it = names.begin();
         it != names.end(); ++it) {
        byteLength += it->size();
    }

    // bounded payload: refuse to encode oversized name sets
    if (byteLength > kMaxDisplayNamesByteLength) {
        return data;
    }

    // payload is [displayCount, length0, bytes0..., length1, bytes1...]
    // where each length is a UTF-8 byte count and each byte is a UInt32.
    data.reserve(1 + displayCount + byteLength);
    data.push_back(static_cast<UInt32>(displayCount));
    for (std::vector<std::string>::const_iterator it = names.begin();
         it != names.end(); ++it) {
        data.push_back(static_cast<UInt32>(it->size()));
        for (std::string::const_iterator c = it->begin(); c != it->end(); ++c) {
            data.push_back(static_cast<UInt32>(static_cast<UInt8>(*c)));
        }
    }
    return data;
}

bool
decodeDisplayNames(const std::vector<UInt32>& data, std::size_t displayCount,
                   std::vector<std::string>& names)
{
    names.clear();

    // payload must at least carry the display count
    if (data.empty()) {
        return false;
    }

    const UInt32 count = data[0];

    // names only parse when they match the DDIS display count
    if (count != displayCount) {
        return false;
    }

    // collect the length-prefixed names into a temporary so a failed
    // decode can never leave partial names in the output (atomic commit
    // below).  a name of length zero is valid (no product name).
    std::vector<std::string> decoded;
    decoded.reserve(count);
    std::size_t index = 1;
    std::size_t totalBytes = 0;
    for (UInt32 i = 0; i < count; ++i) {
        if (index >= data.size()) {
            return false; // truncated: missing length or bytes
        }
        const UInt32 length = data[index++];
        if (length > data.size() - index) {
            return false; // truncated: length exceeds remaining bytes
        }
        totalBytes += length;
        if (totalBytes > kMaxDisplayNamesByteLength) {
            return false; // bounded payload
        }
        std::string name;
        name.reserve(length);
        for (UInt32 j = 0; j < length; ++j) {
            const UInt32 byte = data[index++];
            if (byte > 0xff) {
                return false; // not a byte -- malformed
            }
            name.push_back(static_cast<char>(byte));
        }
        decoded.push_back(name);
    }

    // exactly count length-prefixed names must consume the whole payload
    if (index != data.size()) {
        return false; // trailing elements -- malformed
    }

    names.swap(decoded);
    return true;
}

} // namespace barrier
