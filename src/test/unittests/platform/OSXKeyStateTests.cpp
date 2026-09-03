/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2011 Nick Bolton
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

#include "base/EventQueue.h"
#include "platform/OSXKeyState.h"

#include "test/global/gtest.h"
#include "test/global/gmock.h"

TEST(OSXKeyStateTests, mapModifiersFromOSX_OSXMask_returnBarrierMask)
{
    barrier::KeyMap keyMap;
    EventQueue eventQueue;
    OSXKeyState keyState(&eventQueue, keyMap);

    KeyModifierMask outMask = 0;

    UInt32 shiftMask = 0 | kCGEventFlagMaskShift;
    outMask = keyState.mapModifiersFromOSX(shiftMask);
    EXPECT_EQ(KeyModifierShift, outMask);

    UInt32 ctrlMask = 0 | kCGEventFlagMaskControl;
    outMask = keyState.mapModifiersFromOSX(ctrlMask);
    EXPECT_EQ(KeyModifierControl, outMask);

    UInt32 altMask = 0 | kCGEventFlagMaskAlternate;
    outMask = keyState.mapModifiersFromOSX(altMask);
    EXPECT_EQ(KeyModifierAlt, outMask);

    UInt32 cmdMask = 0 | kCGEventFlagMaskCommand;
    outMask = keyState.mapModifiersFromOSX(cmdMask);
    EXPECT_EQ(KeyModifierSuper, outMask);

    UInt32 capsMask = 0 | kCGEventFlagMaskAlphaShift;
    outMask = keyState.mapModifiersFromOSX(capsMask);
    EXPECT_EQ(KeyModifierCapsLock, outMask);

    UInt32 numMask = 0 | kCGEventFlagMaskNumericPad;
    outMask = keyState.mapModifiersFromOSX(numMask);
    EXPECT_EQ(KeyModifierNumLock, outMask);
}

namespace {

struct RecordedKeyEvent {
    bool m_down;
    KeyID m_key;
    KeyButton m_button;
};

// Records key down/up events synchronously; the base queue keeps them
// pending until loop() runs, which unit tests never do.
class RecordingEventQueue : public EventQueue {
public:
    virtual void addEvent(const Event& event) override
    {
        Event::Type downType = forIKeyState().keyDown();
        Event::Type upType = forIKeyState().keyUp();
        if (event.getType() == downType || event.getType() == upType) {
            auto* info = static_cast<IKeyState::KeyInfo*>(event.getData());
            EXPECT_TRUE(info != nullptr);
            if (info != nullptr) {
                m_events.push_back({event.getType() == downType,
                                    info->m_key, info->m_button});
            }
        }
        EventQueue::addEvent(event);
    }

    std::vector<RecordedKeyEvent> m_events;
};

} // namespace

TEST(OSXKeyStateTests, rightCommandDown_sendsSuperR)
{
    barrier::KeyMap keyMap;
    RecordingEventQueue eventQueue;
    OSXKeyState keyState(&eventQueue, keyMap);

    // right Command key code is 54
    keyState.handleModifierKeysEx(NULL, 0, KeyModifierSuper,
                                  54, kCGEventFlagMaskCommand);

    ASSERT_EQ(1u, eventQueue.m_events.size());
    EXPECT_TRUE(eventQueue.m_events[0].m_down);
    EXPECT_EQ(kKeySuper_R, eventQueue.m_events[0].m_key);
}

TEST(OSXKeyStateTests, leftThenRightCommand_bothKeysForwarded)
{
    barrier::KeyMap keyMap;
    RecordingEventQueue eventQueue;
    OSXKeyState keyState(&eventQueue, keyMap);

    // left Command key code is 55, right Command is 54.  The shared
    // Command mask bit is already set when the second key goes down,
    // so mask-diff alone would swallow it.
    keyState.handleModifierKeysEx(NULL, 0, KeyModifierSuper,
                                  55, kCGEventFlagMaskCommand);
    keyState.handleModifierKeysEx(NULL, KeyModifierSuper, KeyModifierSuper,
                                  54, kCGEventFlagMaskCommand);

    ASSERT_EQ(2u, eventQueue.m_events.size());
    EXPECT_TRUE(eventQueue.m_events[0].m_down);
    EXPECT_EQ(kKeySuper_L, eventQueue.m_events[0].m_key);
    EXPECT_TRUE(eventQueue.m_events[1].m_down);
    EXPECT_EQ(kKeySuper_R, eventQueue.m_events[1].m_key);
    EXPECT_NE(eventQueue.m_events[0].m_button,
              eventQueue.m_events[1].m_button);
}

TEST(OSXKeyStateTests, releaseRightWhileLeftHeld_sendsOnlySuperRUp)
{
    barrier::KeyMap keyMap;
    RecordingEventQueue eventQueue;
    OSXKeyState keyState(&eventQueue, keyMap);

    keyState.handleModifierKeysEx(NULL, 0, KeyModifierSuper,
                                  55, kCGEventFlagMaskCommand);
    keyState.handleModifierKeysEx(NULL, KeyModifierSuper, KeyModifierSuper,
                                  54, kCGEventFlagMaskCommand);
    ASSERT_EQ(2u, eventQueue.m_events.size());
    eventQueue.m_events.clear();

    // Releasing right Command while left is held leaves the mask set;
    // only the right key may report an up event.
    keyState.handleModifierKeysEx(NULL, KeyModifierSuper, KeyModifierSuper,
                                  54, kCGEventFlagMaskCommand);

    ASSERT_EQ(1u, eventQueue.m_events.size());
    EXPECT_FALSE(eventQueue.m_events[0].m_down);
    EXPECT_EQ(kKeySuper_R, eventQueue.m_events[0].m_key);
}

TEST(OSXKeyStateTests, fnTap_sendsFunctionDownAndUp)
{
    barrier::KeyMap keyMap;
    RecordingEventQueue eventQueue;
    OSXKeyState keyState(&eventQueue, keyMap);

    // Fn/Globe key code is 63 and carries no mask bit.
    keyState.handleModifierKeysEx(NULL, 0, 0, 63,
                                  kCGEventFlagMaskSecondaryFn);
    keyState.handleModifierKeysEx(NULL, 0, 0, 63, 0);

    ASSERT_EQ(2u, eventQueue.m_events.size());
    EXPECT_TRUE(eventQueue.m_events[0].m_down);
    EXPECT_EQ(kKeyFunction, eventQueue.m_events[0].m_key);
    EXPECT_FALSE(eventQueue.m_events[1].m_down);
    EXPECT_EQ(kKeyFunction, eventQueue.m_events[1].m_key);
}

TEST(OSXKeyStateTests, unknownKeycodeMaskChange_keepsLegacyLeftMapping)
{
    barrier::KeyMap keyMap;
    RecordingEventQueue eventQueue;
    OSXKeyState keyState(&eventQueue, keyMap);

    keyState.handleModifierKeysEx(NULL, 0, KeyModifierSuper,
                                  0xffffffffu, kCGEventFlagMaskCommand);

    ASSERT_EQ(1u, eventQueue.m_events.size());
    EXPECT_TRUE(eventQueue.m_events[0].m_down);
    EXPECT_EQ(kKeySuper_L, eventQueue.m_events[0].m_key);
}

namespace {

class TestableKeyState : public OSXKeyState {
public:
    TestableKeyState(IEventQueue* events, barrier::KeyMap& keyMap) :
        OSXKeyState(events, keyMap) {}
    using OSXKeyState::getButton;
};

} // namespace

TEST(OSXKeyStateTests, clientKeyMap_resolvesFunctionKey)
{
    // The client synthesizes whatever its key map resolves.  If the map
    // has no button for the key, KeyState::fakeKeyDown drops it before
    // any HID event is posted.
    barrier::KeyMap keyMap;
    RecordingEventQueue eventQueue;
    TestableKeyState keyState(&eventQueue, keyMap);
    keyState.updateKeyMap();

    EXPECT_NE(0, keyState.getButton(kKeyFunction, 0));
    EXPECT_NE(0, keyState.getButton(kKeyGlobe, 0));
}

TEST(OSXKeyStateTests, globeTapKeyDown_mapsToGlobe)
{
    // A Magic Keyboard Fn tap carries a KeyDown/KeyUp pair with key code
    // 179 alongside the FlagsChanged(63) edge.  The down must map instead
    // of being swallowed: KeyUp events always produce a button, so an
    // unmapped down leaks its release as KeyID 0.
    barrier::KeyMap keyMap;
    RecordingEventQueue eventQueue;
    OSXKeyState keyState(&eventQueue, keyMap);

    CGEventRef event = CGEventCreateKeyboardEvent(NULL, 179, true);
    ASSERT_TRUE(event != NULL);
    OSXKeyState::KeyIDs ids;
    KeyModifierMask mask = 0;
    KeyButton button = keyState.mapKeyFromEvent(ids, &mask, event);
    CFRelease(event);

    EXPECT_NE(0, button);
    ASSERT_EQ(1u, ids.size());
    EXPECT_EQ(kKeyGlobe, ids[0]);
}
