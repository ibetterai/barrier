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

#pragma once

#include "barrier/protocol_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace barrier {

//! Maximum total UTF-8 byte length of all display names in one DDNM payload.
/*!
The wire vector is also capped by PROTOCOL_MAX_LIST_LENGTH at read time;
this is the codec-level bound applied before sending or parsing a payload.
*/
static constexpr std::uint32_t kMaxDisplayNamesByteLength = PROTOCOL_MAX_STRING_LENGTH;

//! True iff a peer announcing \p minorVersion can exchange display geometry.
/*!
Protocol 1.7 introduced the DDIS message.  Peers below 1.7 must keep
bounding-box-only behavior and never receive DDIS.
*/
bool supportsDisplayGeometry(SInt16 minorVersion);

//! True iff a peer announcing \p minorVersion can exchange display names.
/*!
Protocol 1.7 introduced the DDNM message.  Peers below 1.7 must keep
geometry-only behavior and never receive DDNM.
*/
bool supportsDisplayNames(SInt16 minorVersion);

//! Protocol minor version a client announces to a server announcing \p serverMinor.
/*!
The client announces the highest minor version both ends support: its own
capped by the server's.  Announcing a version the server does not know would
make an older server reject the connection outright, so a 1.7 client talking
to a 1.6 server announces 1.6 and falls back to geometry-only behavior.
*/
SInt16 negotiatedMinorVersion(SInt16 serverMinor);

//! Encode display names into a DDNM %4I payload vector.
/*!
The payload is \c [displayCount, length0, bytes0..., length1, bytes1...]
where each length is a UTF-8 byte count and each byte is a UInt32, in
\p names order, and \p displayCount is the rectangle count of the
preceding DDIS message.  An empty name encodes as a length of zero: it is
a valid per-display entry meaning the display has no product name, and
never suppresses the whole payload.

Returns an empty vector (callers must not send DDNM) only when the number
of names does not match \p displayCount or the encoded payload would
exceed \c kMaxDisplayNamesByteLength.
*/
std::vector<UInt32> encodeDisplayNames(const std::vector<std::string>& names,
                                       std::size_t displayCount);

//! Decode a DDNM %4I payload vector into display names.
/*!
Returns true and fills \p names (in payload order, matching the DDIS
rectangle order) when the payload is well-formed, bounded, and its count
matches \p displayCount.  Empty names (length zero) are valid entries.
Otherwise returns false and leaves \p names empty; decoding is atomic, so
a rejected payload never exposes partial names.  A payload is rejected
when it is empty, a name's length exceeds the remaining elements
(truncated), any byte element is not a byte, the count does not match
\p displayCount, the total byte length exceeds
\c kMaxDisplayNamesByteLength, or trailing elements remain after exactly
\p displayCount length-prefixed names.
*/
bool decodeDisplayNames(const std::vector<UInt32>& data,
                        std::size_t displayCount,
                        std::vector<std::string>& names);

} // namespace barrier
