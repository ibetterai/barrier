/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2013-2016 Symless Ltd.
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

#define BARRIER_TEST_ENV

#include "server/PrimaryClient.h"
#include "base/String.h"

#include "test/global/gmock.h"

class MockPrimaryClient : public PrimaryClient
{
public:
    explicit MockPrimaryClient(const std::string& name = std::string()) :
        PrimaryClient(name)
    {
    }

    MOCK_CONST_METHOD0(getEventTarget, void*());
    MOCK_CONST_METHOD2(getCursorPos, void(SInt32&, SInt32&));
    MOCK_CONST_METHOD4(getShape, void(SInt32&, SInt32&, SInt32&, SInt32&));
    MOCK_CONST_METHOD1(getDisplays, void(std::vector<ScreenRect>&));
    MOCK_CONST_METHOD0(getDisplayTopology, barrier::DisplayTopology());
    MOCK_CONST_METHOD1(getDisplayNames, void(std::vector<std::string>&));
    MOCK_CONST_METHOD0(getJumpZoneSize, SInt32());
    MOCK_CONST_METHOD2(getCursorCenter, void(SInt32&, SInt32&));
    MOCK_CONST_METHOD0(isLockedToScreen, bool());
    MOCK_CONST_METHOD2(setJumpCursorPos, void(SInt32, SInt32));
    MOCK_METHOD1(reconfigure, void(UInt32));
    MOCK_METHOD0(resetOptions, void());
    MOCK_METHOD1(setOptions, void(const OptionsList&));
    MOCK_METHOD0(enable, void());
    MOCK_METHOD0(disable, void());
    MOCK_METHOD2(registerHotKey, UInt32(KeyID, KeyModifierMask));
    MOCK_CONST_METHOD0(getToggleMask, KeyModifierMask());
    MOCK_METHOD5(enter, void(SInt32, SInt32, UInt32, KeyModifierMask, bool));
    MOCK_METHOD0(leave, bool());
    MOCK_METHOD1(unregisterHotKey, void(UInt32));
};
